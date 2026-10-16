// SPDX-License-Identifier: MPL-2.0
#include "X11Window.hpp"

#include "LinuxMainThreadDispatcher.hpp"
#include "LinuxMenuBar.hpp"
#include "LinuxPaintContext.hpp"
#include "LinuxTextShaper.hpp"
#include "core/geometry/Rect.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <poll.h>
#include <unistd.h>

#include "X11Includes.hpp" // last: Xlib macros

namespace rivet::platform {

namespace {

using x11::kNone;

// Async-signal-safe bridge from SIGINT/SIGTERM to the event loop.
std::atomic<bool> gSignalQuit{false};
LinuxMainThreadDispatcher* gSignalDispatcher = nullptr;

extern "C" void onTerminationSignal(int) {
    gSignalQuit.store(true);
    if (gSignalDispatcher != nullptr) gSignalDispatcher->wake();
}

int ignoreXErrors(Display*, XErrorEvent*) {
    // Asynchronous protocol errors (e.g. a drag source window that vanished)
    // must not terminate the app; Xlib's default handler calls exit().
    return 0;
}

double parsePositive(const char* text) {
    if (text == nullptr || *text == '\0') return 0.0;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    return (end != text && std::isfinite(value) && value > 0.0) ? value : 0.0;
}

double detectScale(Display* display) {
    double scale = parsePositive(std::getenv("RIVET_SCALE"));
    if (scale <= 0.0) scale = parsePositive(std::getenv("GDK_SCALE"));
    if (scale <= 0.0) {
        XrmInitialize();
        if (const char* resources = XResourceManagerString(display); resources != nullptr) {
            XrmDatabase database = XrmGetStringDatabase(resources);
            char* type = nullptr;
            XrmValue value{};
            if (database != nullptr && XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) &&
                value.addr != nullptr) {
                const std::string dpi(value.addr, value.size > 0 ? value.size - 1 : 0);
                const double parsed = parsePositive(dpi.c_str());
                if (parsed > 0.0) scale = parsed / 96.0;
            }
            if (database != nullptr) XrmDestroyDatabase(database);
        }
    }
    if (scale <= 0.0) scale = 1.0;
    return std::clamp(scale, 0.5, 4.0);
}

std::string latin1ToUtf8(const char* bytes, int length) {
    std::string out;
    for (int i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

ui::ModifierFlags makeModifiers(unsigned int state) {
    // Ctrl is the primary shortcut modifier on Linux: it maps to `command`
    // (macOS Cmd) so the shell's Cmd shortcuts are Ctrl shortcuts here. Super
    // stands in for the macOS Control key; Alt is Option.
    ui::ModifierFlags flags;
    flags.shift = (state & ShiftMask) != 0;
    flags.command = (state & ControlMask) != 0;
    flags.option = (state & Mod1Mask) != 0;
    flags.control = (state & Mod4Mask) != 0;
    return flags;
}

ui::KeyEvent translateKey(XKeyEvent& event, XIC inputContext) {
    ui::KeyEvent result;
    result.modifiers = makeModifiers(event.state);
    const KeySym keysym = XLookupKeysym(&event, 0);
    switch (keysym) {
    case XK_Left:
    case XK_KP_Left: result.key = ui::Key::Left; return result;
    case XK_Right:
    case XK_KP_Right: result.key = ui::Key::Right; return result;
    case XK_Up:
    case XK_KP_Up: result.key = ui::Key::Up; return result;
    case XK_Down:
    case XK_KP_Down: result.key = ui::Key::Down; return result;
    case XK_Prior:
    case XK_KP_Prior: result.key = ui::Key::PageUp; return result;
    case XK_Next:
    case XK_KP_Next: result.key = ui::Key::PageDown; return result;
    case XK_Home:
    case XK_KP_Home: result.key = ui::Key::Home; return result;
    case XK_End:
    case XK_KP_End: result.key = ui::Key::End; return result;
    case XK_Escape: result.key = ui::Key::Escape; return result;
    case XK_Return:
    case XK_KP_Enter: result.key = ui::Key::Enter; return result;
    case XK_Tab:
    case XK_ISO_Left_Tab:
        result.key = ui::Key::Tab;
        // Ctrl+Tab cycles tabs: the shell checks `control` for it.
        if (result.modifiers.command) result.modifiers.control = true;
        return result;
    case XK_BackSpace: result.key = ui::Key::Backspace; return result;
    case XK_Delete:
    case XK_KP_Delete: result.key = ui::Key::Delete; return result;
    case XK_space:
        result.key = ui::Key::Space;
        result.text = " ";
        return result;
    default: break;
    }

    std::string text;
    const bool chord = (event.state & (ControlMask | Mod1Mask | Mod4Mask)) != 0;
    if (chord || inputContext == nullptr) {
        // Shortcuts: the character the key would type WITHOUT Ctrl/Alt/Super
        // (macOS -[NSEvent characters] for Cmd chords), not a control code.
        XKeyEvent plain = event;
        plain.state &= ~static_cast<unsigned>(ControlMask | Mod1Mask | Mod4Mask);
        char buffer[32];
        KeySym ignored = 0;
        const int length = XLookupString(&plain, buffer, sizeof buffer, &ignored, nullptr);
        text = latin1ToUtf8(buffer, std::max(length, 0));
    } else {
        char buffer[64];
        KeySym ignored = 0;
        int status = 0;
        int length = Xutf8LookupString(inputContext, &event, buffer, sizeof buffer, &ignored, &status);
        if (status == XBufferOverflow && length > 0) {
            std::vector<char> big(static_cast<std::size_t>(length) + 1);
            length = Xutf8LookupString(inputContext, &event, big.data(), length, &ignored, &status);
            text.assign(big.data(), static_cast<std::size_t>(std::max(length, 0)));
        } else if (status == XLookupChars || status == XLookupBoth) {
            text.assign(buffer, static_cast<std::size_t>(std::max(length, 0)));
        }
    }
    if (keysym == XK_KP_Add) text = "+";
    if (keysym == XK_KP_Subtract) text = "-";
    // Control characters are never typed text.
    if (text.empty() || static_cast<unsigned char>(text[0]) < 0x20 || text[0] == 0x7F) return result;

    if (text == "=" || text == "+") {
        result.key = ui::Key::Plus;
    } else if (text == "-") {
        result.key = ui::Key::Minus;
    } else {
        result.key = ui::Key::Character;
    }
    result.text = std::move(text);
    return result;
}

std::string percentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

// text/uri-list -> local file paths. Only file:// URIs with an empty or
// "localhost" host are accepted (remote hosts are not local files).
std::vector<std::filesystem::path> parseUriList(const std::string& list) {
    std::vector<std::filesystem::path> paths;
    std::size_t start = 0;
    while (start < list.size()) {
        std::size_t end = list.find('\n', start);
        if (end == std::string::npos) end = list.size();
        std::string line = list.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        constexpr std::string_view kScheme = "file://";
        if (line.compare(0, kScheme.size(), kScheme) != 0) continue;
        std::string rest = line.substr(kScheme.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) continue;
        const std::string host = rest.substr(0, slash);
        if (!host.empty() && host != "localhost") continue;
        const std::string path = percentDecode(rest.substr(slash));
        if (path.find('\0') != std::string::npos) continue;
        paths.emplace_back(path);
    }
    return paths;
}

void setUtf8Property(Display* display, Window window, Atom property, Atom utf8, const std::string& value) {
    XChangeProperty(display, window, property, utf8, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size()));
}

} // namespace

// ---------------------------------------------------------------------------
// X11Connection
// ---------------------------------------------------------------------------

std::unique_ptr<X11Connection> X11Connection::open(LinuxMainThreadDispatcher& dispatcher, std::string& error) {
    XInitThreads(); // cheap insurance: cairo-xlib may touch the display from its own helpers
    Display* display = XOpenDisplay(nullptr);
    if (display == nullptr) {
        const char* name = std::getenv("DISPLAY");
        error = std::string("cannot open X display ") + (name != nullptr ? name : "(DISPLAY is not set)");
        return nullptr;
    }
    return std::unique_ptr<X11Connection>(new X11Connection(display, dispatcher));
}

X11Connection::X11Connection(Display* display, LinuxMainThreadDispatcher& dispatcher)
    : display_(display), screen_(DefaultScreen(display)), dispatcher_(dispatcher) {
    XSetErrorHandler(ignoreXErrors);
    scale_ = detectScale(display_);

    // Input method for composed/dead-key text (LC_CTYPE must be set by main).
    XSetLocaleModifiers("");
    inputMethod_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    if (inputMethod_ == nullptr) {
        XSetLocaleModifiers("@im=none");
        inputMethod_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    }

    XSetWindowAttributes attributes{};
    attributes.event_mask = PropertyChangeMask;
    selectionWindow_ = XCreateWindow(display_, RootWindow(display_, screen_), -10, -10, 1, 1, 0, 0,
                                     InputOnly, CopyFromParent, CWEventMask, &attributes);
}

X11Connection::~X11Connection() {
    if (gSignalDispatcher == &dispatcher_) gSignalDispatcher = nullptr;
    if (selectionWindow_ != 0) XDestroyWindow(display_, selectionWindow_);
    if (inputMethod_ != nullptr) XCloseIM(inputMethod_);
    XCloseDisplay(display_);
}

unsigned long X11Connection::atom(const char* name) {
    const auto it = atoms_.find(name);
    if (it != atoms_.end()) return it->second;
    const Atom value = XInternAtom(display_, name, False);
    atoms_.emplace(name, value);
    return value;
}

void X11Connection::installSignalHandlers() {
    gSignalDispatcher = &dispatcher_;
    struct sigaction action{};
    action.sa_handler = onTerminationSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void X11Connection::pump(Mode mode, X11Window* modalWindow, int extraFd, int timeoutMs) {
    // XPending flushes the output buffer and reports events Xlib already
    // read; only block when there is genuinely nothing to do.
    const bool tasksWaiting = mode == Mode::Normal && dispatcher_.hasPending();
    if (XPending(display_) == 0 && !tasksWaiting && !gSignalQuit.load()) {
        pollfd fds[3];
        nfds_t count = 0;
        fds[count++] = pollfd{ConnectionNumber(display_), POLLIN, 0};
        fds[count++] = pollfd{dispatcher_.wakeFd(), POLLIN, 0};
        if (extraFd >= 0) fds[count++] = pollfd{extraFd, POLLIN, 0};
        while (::poll(fds, count, timeoutMs) < 0 && errno == EINTR) {
            if (gSignalQuit.load()) break;
        }
    }
    dispatcher_.consumeWakeups();

    while (XPending(display_) > 0) {
        XEvent event;
        XNextEvent(display_, &event);
        if (XFilterEvent(&event, kNone)) continue; // consumed by the input method
        dispatchEvent(event, mode, modalWindow);
    }

    if (mode == Mode::Normal) {
        if (gSignalQuit.exchange(false) && signalQuit_) signalQuit_();
        dispatcher_.drain();
    }

    // Copy: painting may not add/remove windows, but be defensive.
    std::vector<X11Window*> windows;
    windows.reserve(windows_.size());
    for (const auto& entry : windows_) windows.push_back(entry.second);
    for (X11Window* window : windows) window->paintIfNeeded();
    XFlush(display_);
}

void X11Connection::dispatchEvent(XEvent& event, Mode mode, X11Window* modalWindow) {
    switch (event.type) {
    case SelectionRequest: handleSelectionRequest(event); return;
    case SelectionClear:
        if (event.xselectionclear.window == selectionWindow_) {
            ownsClipboard_ = false;
            clipboard_.clear();
        }
        return;
    default: break;
    }
    const auto it = windows_.find(event.xany.window);
    if (it == windows_.end()) return;
    const bool inputAllowed = mode == Mode::Normal || it->second == modalWindow;
    it->second->handleEvent(event, inputAllowed);
}

bool X11Connection::setClipboardText(std::string text) {
    const Atom clipboard = atom("CLIPBOARD");
    XSetSelectionOwner(display_, clipboard, selectionWindow_, CurrentTime);
    if (XGetSelectionOwner(display_, clipboard) != selectionWindow_) return false;
    ownsClipboard_ = true;
    clipboard_ = std::move(text);
    return true;
}

void X11Connection::handleSelectionRequest(XEvent& event) {
    const XSelectionRequestEvent& request = event.xselectionrequest;
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = request.display;
    reply.xselection.requestor = request.requestor;
    reply.xselection.selection = request.selection;
    reply.xselection.target = request.target;
    reply.xselection.time = request.time;
    reply.xselection.property = kNone;

    const Atom property = request.property != kNone ? request.property : request.target;
    const Atom utf8 = atom("UTF8_STRING");
    const Atom targets = atom("TARGETS");
    const Atom textPlain = atom("text/plain;charset=utf-8");
    const Atom compound = atom("TEXT");
    // Without INCR support a transfer must fit one request.
    const long maxRequest = std::max(XExtendedMaxRequestSize(display_), XMaxRequestSize(display_));
    const auto maxBytes = static_cast<std::size_t>(std::max(0L, maxRequest * 4 - 1024));

    if (ownsClipboard_ && request.owner == selectionWindow_ && request.selection == atom("CLIPBOARD")) {
        if (request.target == targets) {
            const Atom supported[] = {targets, utf8, textPlain, compound, XA_STRING};
            XChangeProperty(display_, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(supported),
                            static_cast<int>(sizeof supported / sizeof supported[0]));
            reply.xselection.property = property;
        } else if ((request.target == utf8 || request.target == textPlain || request.target == compound) &&
                   clipboard_.size() <= maxBytes) {
            setUtf8Property(display_, request.requestor, property, utf8, clipboard_);
            reply.xselection.property = property;
        } else if (request.target == XA_STRING && clipboard_.size() <= maxBytes) {
            // ISO-8859-1 by definition: non-Latin-1 code points become '?'.
            std::string latin1;
            for (std::size_t i = 0; i < clipboard_.size(); ++i) {
                const auto c = static_cast<unsigned char>(clipboard_[i]);
                if (c < 0x80) {
                    latin1 += static_cast<char>(c);
                } else if ((c & 0xE0) == 0xC0 && i + 1 < clipboard_.size()) {
                    const unsigned cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(clipboard_[i + 1]) & 0x3Fu);
                    latin1 += cp < 0x100 ? static_cast<char>(cp) : '?';
                    ++i;
                } else if ((c & 0xC0) != 0x80) {
                    latin1 += '?';
                }
            }
            XChangeProperty(display_, request.requestor, property, XA_STRING, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(latin1.data()), static_cast<int>(latin1.size()));
            reply.xselection.property = property;
        }
    }
    XSendEvent(display_, request.requestor, False, 0, &reply);
}

bool X11Connection::readSelectionProperty(unsigned long target, std::string& out) {
    const Atom property = atom("RIVET_CLIPBOARD");
    XDeleteProperty(display_, selectionWindow_, property);
    XConvertSelection(display_, atom("CLIPBOARD"), target, property, selectionWindow_, CurrentTime);
    XFlush(display_);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    XEvent event;
    for (;;) {
        if (XCheckTypedWindowEvent(display_, selectionWindow_, SelectionNotify, &event)) break;
        // While waiting, keep serving requests addressed to us.
        if (XCheckTypedEvent(display_, SelectionRequest, &event)) {
            handleSelectionRequest(event);
            continue;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return false;
        pollfd fd{ConnectionNumber(display_), POLLIN, 0};
        ::poll(&fd, 1, static_cast<int>(remaining.count()));
        XEventsQueued(display_, QueuedAfterReading);
    }
    if (event.xselection.property == kNone) return false;

    Atom type = kNone;
    int format = 0;
    unsigned long items = 0;
    unsigned long remainingBytes = 0;
    unsigned char* data = nullptr;
    if (XGetWindowProperty(display_, selectionWindow_, property, 0, 0x7FFFFFFF / 4, True, AnyPropertyType, &type,
                           &format, &items, &remainingBytes, &data) != 0) {
        return false;
    }
    bool ok = false;
    if (data != nullptr && type != atom("INCR") && format == 8) {
        out.assign(reinterpret_cast<const char*>(data), items);
        if (type == XA_STRING) out = latin1ToUtf8(out.data(), static_cast<int>(out.size()));
        ok = true;
    }
    if (data != nullptr) XFree(data);
    return ok;
}

std::string X11Connection::clipboardText() {
    const Atom clipboard = atom("CLIPBOARD");
    const Window owner = XGetSelectionOwner(display_, clipboard);
    if (owner == kNone) return {};
    if (owner == selectionWindow_ && ownsClipboard_) return clipboard_;
    std::string text;
    if (readSelectionProperty(atom("UTF8_STRING"), text)) return text;
    if (readSelectionProperty(XA_STRING, text)) return text;
    return {};
}

// ---------------------------------------------------------------------------
// X11Window
// ---------------------------------------------------------------------------

void X11Window::Sink::requestRedraw() {
    // Any thread: flag + wake. The loop paints once per iteration however
    // many requests arrived (setNeedsDisplay coalescing).
    if (!dirty_.exchange(true)) owner_.connection_.dispatcher().wake();
}

X11Window::X11Window(X11Connection& connection, const Options& options)
    : connection_(connection), title_(options.title) {
    Display* display = connection_.display();
    const int screen = connection_.screen();
    const double scale = connection_.scale();

    const int screenWidth = DisplayWidth(display, screen);
    const int screenHeight = DisplayHeight(display, screen);
    pixelWidth_ = std::max(1, static_cast<int>(std::lround(options.logicalSize.width * scale)));
    pixelHeight_ = std::max(1, static_cast<int>(std::lround(options.logicalSize.height * scale)));
    if (!options.dialog) {
        pixelWidth_ = std::min(pixelWidth_, std::max(1, screenWidth * 9 / 10));
        pixelHeight_ = std::min(pixelHeight_, std::max(1, screenHeight * 9 / 10));
    }
    int x = (screenWidth - pixelWidth_) / 2;
    int y = (screenHeight - pixelHeight_) / 2;
    if (options.owner != nullptr) {
        Window child = 0;
        int ownerX = 0;
        int ownerY = 0;
        XTranslateCoordinates(display, options.owner->id(), RootWindow(display, screen), 0, 0, &ownerX, &ownerY,
                              &child);
        x = ownerX + (options.owner->pixelWidth_ - pixelWidth_) / 2;
        y = ownerY + std::max(0, (options.owner->pixelHeight_ - pixelHeight_) / 3);
    }

    XSetWindowAttributes attributes{};
    attributes.background_pixel = WhitePixel(display, screen);
    attributes.bit_gravity = NorthWestGravity;
    attributes.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask | FocusChangeMask | PropertyChangeMask;
    window_ = XCreateWindow(display, RootWindow(display, screen), x, y, static_cast<unsigned>(pixelWidth_),
                            static_cast<unsigned>(pixelHeight_), 0, CopyFromParent, InputOutput, CopyFromParent,
                            CWBackPixel | CWBitGravity | CWEventMask, &attributes);
    connection_.registerWindow(window_, this);

    Atom protocols[] = {static_cast<Atom>(connection_.atom("WM_DELETE_WINDOW")),
                        static_cast<Atom>(connection_.atom("_NET_WM_PING"))};
    XSetWMProtocols(display, window_, protocols, 2);

    XSizeHints* hints = XAllocSizeHints();
    if (hints != nullptr) {
        hints->flags = PMinSize | (options.owner != nullptr ? PPosition : 0);
        hints->x = x;
        hints->y = y;
        hints->min_width = std::max(1, static_cast<int>(std::lround(options.minLogicalSize.width * scale)));
        hints->min_height = std::max(1, static_cast<int>(std::lround(options.minLogicalSize.height * scale)));
        if (!options.resizable) {
            hints->flags |= PMaxSize;
            hints->min_width = hints->max_width = pixelWidth_;
            hints->min_height = hints->max_height = pixelHeight_;
        }
        XSetWMNormalHints(display, window_, hints);
        XFree(hints);
    }
    XClassHint* classHint = XAllocClassHint();
    if (classHint != nullptr) {
        char name[] = "rivet";
        char className[] = "Rivet";
        classHint->res_name = name;
        classHint->res_class = className;
        XSetClassHint(display, window_, classHint);
        XFree(classHint);
    }
    const long pid = static_cast<long>(::getpid());
    XChangeProperty(display, window_, connection_.atom("_NET_WM_PID"), XA_CARDINAL, 32, PropModeReplace,
                    reinterpret_cast<const unsigned char*>(&pid), 1);

    if (options.dialog) {
        const Atom dialogType = connection_.atom("_NET_WM_WINDOW_TYPE_DIALOG");
        XChangeProperty(display, window_, connection_.atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&dialogType), 1);
        const Atom modal = connection_.atom("_NET_WM_STATE_MODAL");
        XChangeProperty(display, window_, connection_.atom("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&modal), 1);
        if (options.owner != nullptr) XSetTransientForHint(display, window_, options.owner->id());
    } else {
        // XDND target, protocol version 5.
        const long version = 5;
        XChangeProperty(display, window_, connection_.atom("XdndAware"), XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&version), 1);
    }

    if (connection_.inputMethod() != nullptr) {
        inputContext_ = XCreateIC(connection_.inputMethod(), XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                  XNClientWindow, window_, XNFocusWindow, window_, nullptr);
        if (inputContext_ != nullptr) {
            long imEvents = 0;
            XGetICValues(inputContext_, XNFilterEvents, &imEvents, nullptr);
            XSelectInput(display, window_, attributes.event_mask | imEvents);
        }
    }

    windowSurface_ =
        cairo_xlib_surface_create(display, window_, DefaultVisual(display, screen), pixelWidth_, pixelHeight_);
    updateTitle();
}

X11Window::~X11Window() {
    Display* display = connection_.display();
    connection_.unregisterWindow(window_);
    if (backBuffer_ != nullptr) cairo_surface_destroy(backBuffer_);
    if (windowSurface_ != nullptr) {
        cairo_surface_finish(windowSurface_);
        cairo_surface_destroy(windowSurface_);
    }
    if (inputContext_ != nullptr) XDestroyIC(inputContext_);
    XDestroyWindow(display, window_);
    XFlush(display);
}

void X11Window::show() {
    XMapRaised(connection_.display(), window_);
    XFlush(connection_.display());
}

void X11Window::focusWhenViewable() {
    if (focusGiven_) return;
    Display* display = connection_.display();
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display, window_, &attributes) == 0 || attributes.map_state != IsViewable) return;
    XSetInputFocus(display, window_, RevertToParent, CurrentTime);
    focusGiven_ = true;
}

void X11Window::updateTitle() {
    const std::string shown = edited_ ? "*" + title_ : title_;
    Display* display = connection_.display();
    const Atom utf8 = connection_.atom("UTF8_STRING");
    setUtf8Property(display, window_, connection_.atom("_NET_WM_NAME"), utf8, shown);
    setUtf8Property(display, window_, XA_WM_NAME, utf8, shown);
    XFlush(display);
}

void X11Window::setTitle(const std::string& title) {
    title_ = title;
    updateTitle();
}

void X11Window::setDocumentEdited(bool edited) {
    // X11 has no "edited" decoration; the conventional "*" title prefix
    // stands in for the dot in the macOS close button.
    if (edited_ == edited) return;
    edited_ = edited;
    updateTitle();
}

double X11Window::contentTop() const {
    return menuBar_ != nullptr ? menuBar_->height() : 0.0;
}

core::Size X11Window::contentLogicalSize() const {
    const double scale = connection_.scale();
    return core::Size{pixelWidth_ / scale, std::max(0.0, pixelHeight_ / scale - contentTop())};
}

void X11Window::setRootWidget(ui::Widget* widget) {
    root_ = widget;
    if (root_ != nullptr) {
        root_->setRedrawSink(&sink_);
        applyRootFrame();
    }
    sink_.requestRedraw();
}

void X11Window::setMenuBar(LinuxMenuBar* menuBar) {
    menuBar_ = menuBar;
    applyRootFrame();
    sink_.requestRedraw();
}

void X11Window::applyRootFrame() {
    if (root_ == nullptr) return;
    // Root-local content space: {0, 0, w, h} below the menu strip.
    root_->setFrame(core::Rect{core::Point{}, contentLogicalSize()});
}

void X11Window::paintIfNeeded() {
    if (sink_.consume()) paint();
}

void X11Window::paint() {
    if (pixelWidth_ <= 0 || pixelHeight_ <= 0 || windowSurface_ == nullptr) return;
    if (backBuffer_ == nullptr || cairo_image_surface_get_width(backBuffer_) != pixelWidth_ ||
        cairo_image_surface_get_height(backBuffer_) != pixelHeight_) {
        if (backBuffer_ != nullptr) cairo_surface_destroy(backBuffer_);
        backBuffer_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, pixelWidth_, pixelHeight_);
    }
    const double scale = connection_.scale();

    cairo_t* cr = cairo_create(backBuffer_);
    cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    text::applyFontOptions(cr);
    {
        LinuxPaintContext context(cr, scale);
        const double logicalWidth = pixelWidth_ / scale;
        const double top = contentTop();
        if (menuBar_ != nullptr) menuBar_->paintBar(context, logicalWidth);
        if (root_ != nullptr) {
            const core::Size size = contentLogicalSize();
            context.pushClip(core::Rect{0.0, top, size.width, size.height});
            root_->paint(context);
            context.popClip();
        }
        if (menuBar_ != nullptr) menuBar_->paintOverlay(context);
    }
    cairo_destroy(cr);
    cairo_surface_flush(backBuffer_);

    cairo_t* front = cairo_create(windowSurface_);
    cairo_set_operator(front, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(front, backBuffer_, 0.0, 0.0);
    cairo_paint(front);
    cairo_destroy(front);
    cairo_surface_flush(windowSurface_);
}

void X11Window::handlePointer(const ui::PointerEvent& event) {
    if (menuBar_ != nullptr && !contentPressed_) {
        const bool overStrip = event.position.y < contentTop();
        const bool menuTakes =
            menuBar_->isOpen() || (overStrip && event.type == ui::PointerEventType::Down);
        if (menuTakes) {
            menuBar_->onPointer(event);
            sink_.requestRedraw();
            return;
        }
        if (overStrip && event.type != ui::PointerEventType::Up) return; // hovering the strip
    }
    if (event.type == ui::PointerEventType::Down) contentPressed_ = true;
    if (event.type == ui::PointerEventType::Up) contentPressed_ = false;
    if (root_ == nullptr) return;
    ui::PointerEvent local = event;
    local.position.y -= contentTop();
    root_->onMouse(local);
}

void X11Window::handleKey(XEvent& event) {
    const ui::KeyEvent keyEvent = translateKey(event.xkey, inputContext_);
    if (menuBar_ != nullptr) {
        if (menuBar_->isOpen()) {
            menuBar_->onKey(keyEvent);
            sink_.requestRedraw();
            return;
        }
        if (XLookupKeysym(&event.xkey, 0) == XK_F10 && !keyEvent.modifiers.any()) {
            menuBar_->openMenu(0);
            sink_.requestRedraw();
            return;
        }
        if (menuBar_->handleShortcut(keyEvent)) {
            sink_.requestRedraw();
            return;
        }
    }
    if (keyHandler_) {
        const auto handler = keyHandler_; // may be replaced while running
        handler(keyEvent);
    }
}

void X11Window::handleEvent(XEvent& event, bool inputAllowed) {
    const double scale = connection_.scale();
    switch (event.type) {
    case Expose:
        if (event.xexpose.count == 0) sink_.requestRedraw();
        return;
    case ConfigureNotify: {
        const int width = event.xconfigure.width;
        const int height = event.xconfigure.height;
        if (width != pixelWidth_ || height != pixelHeight_) {
            pixelWidth_ = width;
            pixelHeight_ = height;
            cairo_xlib_surface_set_size(windowSurface_, width, height);
            applyRootFrame();
            sink_.requestRedraw();
        }
        return;
    }
    case FocusIn:
        if (inputContext_ != nullptr) XSetICFocus(inputContext_);
        return;
    case FocusOut:
        if (inputContext_ != nullptr) XUnsetICFocus(inputContext_);
        return;
    case ClientMessage: handleClientMessage(event); return;
    case SelectionNotify: handleDropData(event); return;
    default: break;
    }
    if (!inputAllowed) return; // app-modal session elsewhere: drop input

    switch (event.type) {
    case KeyPress: handleKey(event); return;
    case ButtonPress:
    case ButtonRelease: {
        const XButtonEvent& button = event.xbutton;
        ui::PointerEvent pointer;
        pointer.position = core::Point{button.x / scale, button.y / scale};
        pointer.modifiers = makeModifiers(button.state);
        if (button.button >= 4 && button.button <= 7) {
            if (event.type != ButtonPress) return;
            // One wheel notch = three 16-point lines. Rivet's scrollDelta is
            // "positive = offset grows" (content moves up/left): wheel down
            // (button 5) scrolls toward the end. Shift turns the wheel
            // horizontal, as on macOS.
            constexpr double kNotch = 48.0;
            double dx = 0.0;
            double dy = 0.0;
            switch (button.button) {
            case 4: dy = -kNotch; break;
            case 5: dy = kNotch; break;
            case 6: dx = -kNotch; break;
            default: dx = kNotch; break;
            }
            if (pointer.modifiers.shift && dx == 0.0) std::swap(dx, dy);
            pointer.type = ui::PointerEventType::Scroll;
            pointer.scrollDelta = core::Point{dx, dy};
            if (menuBar_ != nullptr && menuBar_->isOpen()) return;
            if (root_ != nullptr && pointer.position.y >= contentTop()) {
                pointer.position.y -= contentTop();
                root_->onMouse(pointer);
            }
            return;
        }
        if (button.button == 1) {
            pointer.button = 1;
        } else if (button.button == 3) {
            pointer.button = 2;
        } else {
            return; // middle button: unused, like otherMouseDown on macOS
        }
        pointer.type = event.type == ButtonPress ? ui::PointerEventType::Down : ui::PointerEventType::Up;
        handlePointer(pointer);
        return;
    }
    case MotionNotify: {
        // Coalesce motion that is IMMEDIATELY queued behind this one (never
        // across a button event: a drag's last Move must precede its Up).
        Display* display = connection_.display();
        while (XEventsQueued(display, QueuedAlready) > 0) {
            XEvent next;
            XPeekEvent(display, &next);
            if (next.type != MotionNotify || next.xmotion.window != window_) break;
            XNextEvent(display, &event);
        }
        const XMotionEvent& motion = event.xmotion;
        ui::PointerEvent pointer;
        pointer.type = ui::PointerEventType::Move;
        pointer.position = core::Point{motion.x / scale, motion.y / scale};
        pointer.modifiers = makeModifiers(motion.state);
        if ((motion.state & Button1Mask) != 0) {
            pointer.button = 1;
        } else if ((motion.state & Button3Mask) != 0) {
            pointer.button = 2;
        }
        handlePointer(pointer);
        return;
    }
    default: return;
    }
}

void X11Window::handleClientMessage(XEvent& event) {
    Display* display = connection_.display();
    const XClientMessageEvent& message = event.xclient;
    if (message.message_type == connection_.atom("WM_PROTOCOLS")) {
        const auto protocol = static_cast<Atom>(message.data.l[0]);
        if (protocol == connection_.atom("_NET_WM_PING")) {
            XEvent reply = event;
            reply.xclient.window = RootWindow(display, connection_.screen());
            XSendEvent(display, reply.xclient.window, False, SubstructureNotifyMask | SubstructureRedirectMask,
                       &reply);
        } else if (protocol == connection_.atom("WM_DELETE_WINDOW")) {
            if (closeHandler_) {
                const auto handler = closeHandler_;
                handler();
            }
        }
        return;
    }

    // XDND (file drops from file managers).
    if (message.message_type == connection_.atom("XdndEnter")) {
        dndSource_ = static_cast<Window>(message.data.l[0]);
        dndVersion_ = static_cast<int>(static_cast<unsigned long>(message.data.l[1]) >> 24);
        const Atom uriList = connection_.atom("text/uri-list");
        dndAcceptable_ = false;
        if ((message.data.l[1] & 1) != 0) {
            Atom type = kNone;
            int format = 0;
            unsigned long count = 0;
            unsigned long remaining = 0;
            unsigned char* data = nullptr;
            if (XGetWindowProperty(display, dndSource_, connection_.atom("XdndTypeList"), 0, 1024, False, XA_ATOM,
                                   &type, &format, &count, &remaining, &data) == 0 &&
                data != nullptr) {
                const auto* atoms = reinterpret_cast<const Atom*>(static_cast<const void*>(data));
                for (unsigned long i = 0; i < count; ++i) {
                    if (atoms[i] == uriList) dndAcceptable_ = true;
                }
            }
            if (data != nullptr) XFree(data);
        } else {
            for (int i = 2; i <= 4; ++i) {
                if (static_cast<Atom>(message.data.l[i]) == uriList) dndAcceptable_ = true;
            }
        }
        return;
    }
    if (message.message_type == connection_.atom("XdndPosition")) {
        sendDndStatus(dndAcceptable_ && fileDropHandler_ != nullptr);
        return;
    }
    if (message.message_type == connection_.atom("XdndLeave")) {
        dndSource_ = 0;
        dndAcceptable_ = false;
        return;
    }
    if (message.message_type == connection_.atom("XdndDrop")) {
        if (!dndAcceptable_ || fileDropHandler_ == nullptr) {
            sendDndFinished(false);
            return;
        }
        const Time time = dndVersion_ >= 1 ? static_cast<Time>(message.data.l[2]) : CurrentTime;
        XConvertSelection(display, connection_.atom("XdndSelection"), connection_.atom("text/uri-list"),
                          connection_.atom("RIVET_DND"), window_, time);
        return;
    }
}

void X11Window::sendDndStatus(bool accept) {
    if (dndSource_ == 0) return;
    XEvent reply{};
    reply.xclient.type = ClientMessage;
    reply.xclient.display = connection_.display();
    reply.xclient.window = dndSource_;
    reply.xclient.message_type = connection_.atom("XdndStatus");
    reply.xclient.format = 32;
    reply.xclient.data.l[0] = static_cast<long>(window_);
    reply.xclient.data.l[1] = accept ? 1 : 0;
    reply.xclient.data.l[4] = accept ? static_cast<long>(connection_.atom("XdndActionCopy")) : 0;
    XSendEvent(connection_.display(), dndSource_, False, NoEventMask, &reply);
}

void X11Window::sendDndFinished(bool accepted) {
    if (dndSource_ == 0) return;
    XEvent reply{};
    reply.xclient.type = ClientMessage;
    reply.xclient.display = connection_.display();
    reply.xclient.window = dndSource_;
    reply.xclient.message_type = connection_.atom("XdndFinished");
    reply.xclient.format = 32;
    reply.xclient.data.l[0] = static_cast<long>(window_);
    reply.xclient.data.l[1] = accepted ? 1 : 0;
    reply.xclient.data.l[2] = accepted ? static_cast<long>(connection_.atom("XdndActionCopy")) : 0;
    XSendEvent(connection_.display(), dndSource_, False, NoEventMask, &reply);
    dndSource_ = 0;
    dndAcceptable_ = false;
}

void X11Window::handleDropData(XEvent& event) {
    const XSelectionEvent& selection = event.xselection;
    if (selection.property != connection_.atom("RIVET_DND")) return;
    Display* display = connection_.display();
    Atom type = kNone;
    int format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    std::vector<std::filesystem::path> paths;
    if (XGetWindowProperty(display, window_, selection.property, 0, 0x7FFFFFFF / 4, True, AnyPropertyType, &type,
                           &format, &count, &remaining, &data) == 0 &&
        data != nullptr && format == 8) {
        paths = parseUriList(std::string(reinterpret_cast<const char*>(data), count));
    }
    if (data != nullptr) XFree(data);
    const bool accepted = !paths.empty() && fileDropHandler_ != nullptr;
    sendDndFinished(accepted);
    if (accepted) {
        const auto handler = fileDropHandler_;
        handler(paths);
    }
}

} // namespace rivet::platform
