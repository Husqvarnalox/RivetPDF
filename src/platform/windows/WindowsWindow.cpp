// SPDX-License-Identifier: MPL-2.0
#include "WindowsWindow.hpp"

#include "WindowsAppearance.hpp"
#include "WindowsMenuBar.hpp"
#include "WindowsPaintContext.hpp"
#include "core/geometry/Rect.hpp"
#include "ui/Theme.hpp"

#include <d2d1.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace rivet::platform {

namespace {

constexpr wchar_t kClassName[] = L"RivetMainWindow";
constexpr UINT kRedrawMessage = WM_APP + 2;
// DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+; older SDKs lack the name).
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
// ToUnicode flag: do not change the keyboard state (Windows 10 1607+).
constexpr UINT kToUnicodeNoStateChange = 0x4;

void registerClassOnce(HINSTANCE instance, WNDPROC proc) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof windowClass;
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = proc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (windowClass.hIcon == nullptr) windowClass.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kClassName;
    registered = ::RegisterClassExW(&windowClass) != 0;
}

double scaleForDpi(UINT dpi) {
    return dpi > 0 ? static_cast<double>(dpi) / USER_DEFAULT_SCREEN_DPI : 1.0;
}

bool keyDown(int virtualKey) {
    return (::GetKeyState(virtualKey) & 0x8000) != 0;
}

// Ctrl is the primary shortcut modifier on Windows: it maps to `command`
// (macOS Cmd) so the shell's Cmd shortcuts are Ctrl shortcuts here. The
// Windows key stands in for the macOS Control key; Alt is Option.
ui::ModifierFlags currentModifiers() {
    ui::ModifierFlags flags;
    flags.shift = keyDown(VK_SHIFT);
    flags.command = keyDown(VK_CONTROL);
    flags.option = keyDown(VK_MENU);
    flags.control = keyDown(VK_LWIN) || keyDown(VK_RWIN);
    // AltGr arrives as LeftCtrl + RightAlt: it types characters and is not
    // a Ctrl+Alt chord.
    if (flags.command && flags.option && keyDown(VK_RMENU)) {
        flags.command = false;
        flags.option = false;
    }
    return flags;
}

ui::KeyEvent textEvent(std::string text, ui::ModifierFlags modifiers) {
    ui::KeyEvent event;
    event.modifiers = modifiers;
    if (text == "=" || text == "+") {
        event.key = ui::Key::Plus;
    } else if (text == "-") {
        event.key = ui::Key::Minus;
    } else {
        event.key = ui::Key::Character;
    }
    event.text = std::move(text);
    return event;
}

} // namespace

// ---------------------------------------------------------------------------

void WindowsWindow::Sink::requestRedraw() {
    // Any thread: one posted message per burst; InvalidateRect then lets the
    // system coalesce into a single WM_PAINT (setNeedsDisplay semantics).
    if (!pending_.exchange(true) && owner_.hwnd_ != nullptr) {
        ::PostMessageW(owner_.hwnd_, kRedrawMessage, 0, 0);
    }
}

WindowsWindow::WindowsWindow(const Options& options)
    : minLogicalSize_(options.minLogicalSize), initialLogicalSize_(options.logicalSize), title_(options.title) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    registerClassOnce(instance, &WindowsWindow::windowProc);

    D2D1_FACTORY_OPTIONS factoryOptions{};
    ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &factoryOptions,
                        reinterpret_cast<void**>(&factory_));

    // Created hidden at the default position; show() sizes it for the DPI
    // of the monitor it landed on.
    ::CreateWindowExW(0, kClassName, win::widen(options.title).c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                      CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) return;
    scale_ = scaleForDpi(::GetDpiForWindow(hwnd_));
    applyDarkTitleBar();
}

WindowsWindow::~WindowsWindow() {
    releaseTarget();
    if (hwnd_ != nullptr) {
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (factory_ != nullptr) factory_->Release();
}

void WindowsWindow::show(int showCommand) {
    if (hwnd_ == nullptr) return;
    // Logical size -> outer window size at this monitor's DPI, clamped to
    // 90% of the work area and centered on it.
    const UINT dpi = ::GetDpiForWindow(hwnd_);
    scale_ = scaleForDpi(dpi);
    RECT frame{0, 0, static_cast<LONG>(std::lround(initialLogicalSize_.width * scale_)),
               static_cast<LONG>(std::lround(initialLogicalSize_.height * scale_))};
    ::AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, ::GetMenu(hwnd_) != nullptr, 0, dpi);
    int width = frame.right - frame.left;
    int height = frame.bottom - frame.top;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof monitor;
    if (::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const RECT& work = monitor.rcWork;
        const int workWidth = work.right - work.left;
        const int workHeight = work.bottom - work.top;
        width = std::min(width, std::max(1, workWidth * 9 / 10));
        height = std::min(height, std::max(1, workHeight * 9 / 10));
        ::SetWindowPos(hwnd_, nullptr, work.left + (workWidth - width) / 2, work.top + (workHeight - height) / 2,
                       width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        ::SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ::ShowWindow(hwnd_, showCommand);
    ::UpdateWindow(hwnd_);
}

void WindowsWindow::updateTitle() {
    if (hwnd_ == nullptr) return;
    // Windows has no "edited" decoration; the conventional "*" title prefix
    // stands in for the dot in the macOS close button.
    const std::string shown = edited_ ? "*" + title_ : title_;
    ::SetWindowTextW(hwnd_, win::widen(shown).c_str());
}

void WindowsWindow::setTitle(const std::string& title) {
    title_ = title;
    updateTitle();
}

void WindowsWindow::setDocumentEdited(bool edited) {
    if (edited_ == edited) return;
    edited_ = edited;
    updateTitle();
}

void WindowsWindow::applyDarkTitleBar() {
    if (hwnd_ == nullptr) return;
    const BOOL dark = WindowsAppearance::systemPrefersDark() ? TRUE : FALSE;
    // Fails harmlessly on systems without the attribute.
    ::DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof dark);
}

void WindowsWindow::setFileDropHandler(std::function<void(const std::vector<std::filesystem::path>&)> handler) {
    fileDropHandler_ = std::move(handler);
    if (hwnd_ != nullptr) ::DragAcceptFiles(hwnd_, fileDropHandler_ ? TRUE : FALSE);
}

void WindowsWindow::setMenuBar(WindowsMenuBar* menuBar) {
    if (menuBar_ != nullptr) menuBar_->detached();
    menuBar_ = menuBar;
    if (hwnd_ == nullptr) return;
    ::SetMenu(hwnd_, menuBar_ != nullptr ? menuBar_->handle() : nullptr);
    if (menuBar_ != nullptr) menuBar_->attached();
    // The client area changed: WM_SIZE re-applies the root frame.
}

core::Size WindowsWindow::contentLogicalSize() const {
    return core::Size{pixelWidth_ / scale_, pixelHeight_ / scale_};
}

void WindowsWindow::setRootWidget(ui::Widget* widget) {
    root_ = widget;
    if (root_ != nullptr) {
        root_->setRedrawSink(&sink_);
        applyRootFrame();
    }
    sink_.requestRedraw();
}

void WindowsWindow::applyRootFrame() {
    if (root_ == nullptr) return;
    root_->setFrame(core::Rect{core::Point{}, contentLogicalSize()});
}

void WindowsWindow::releaseTarget() {
    if (target_ != nullptr) {
        target_->Release();
        target_ = nullptr;
    }
}

void WindowsWindow::paint() {
    if (factory_ == nullptr || pixelWidth_ <= 0 || pixelHeight_ <= 0) return;
    const auto dpi = static_cast<FLOAT>(USER_DEFAULT_SCREEN_DPI * scale_);
    if (target_ == nullptr) {
        D2D1_RENDER_TARGET_PROPERTIES properties{};
        properties.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        properties.dpiX = dpi;
        properties.dpiY = dpi;
        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties{};
        hwndProperties.hwnd = hwnd_;
        hwndProperties.pixelSize = D2D1_SIZE_U{static_cast<UINT32>(pixelWidth_), static_cast<UINT32>(pixelHeight_)};
        hwndProperties.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
        if (FAILED(factory_->CreateHwndRenderTarget(properties, hwndProperties, &target_))) {
            target_ = nullptr;
            return;
        }
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    target_->SetDpi(dpi, dpi);

    target_->BeginDraw();
    D2D1_MATRIX_3X2_F identity{};
    identity._11 = 1.0F;
    identity._22 = 1.0F;
    target_->SetTransform(identity);
    {
        // Behind the widget tree: the theme's chrome color (ADR-0033).
        const ui::Color background = ui::colors().chromeBackground;
        target_->Clear(D2D1_COLOR_F{static_cast<float>(background.r), static_cast<float>(background.g),
                                    static_cast<float>(background.b), 1.0F});
    }
    {
        WindowsPaintContext context(target_, scale_);
        if (root_ != nullptr) {
            const core::Size size = contentLogicalSize();
            context.pushClip(core::Rect{0.0, 0.0, size.width, size.height});
            root_->paint(context);
            context.popClip();
        }
    }
    if (target_->EndDraw() == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        // Device loss (driver update, GPU reset): rebuild on the next paint.
        releaseTarget();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void WindowsWindow::dispatchPointer(const ui::PointerEvent& event) {
    if (root_ == nullptr) return;
    root_->onMouse(event);
}

bool WindowsWindow::dispatchKey(const ui::KeyEvent& event) {
    if (menuBar_ != nullptr && menuBar_->handleShortcut(event)) return true;
    if (!keyHandler_) return false;
    const auto handler = keyHandler_; // may be replaced while running
    return handler(event);
}

void WindowsWindow::removeQueuedChars() {
    // TranslateMessage already queued the WM_CHAR/WM_SYSCHAR of this
    // keystroke; it must not type text after a handled key/chord.
    MSG pending;
    while (::PeekMessageW(&pending, hwnd_, WM_CHAR, WM_DEADCHAR, PM_REMOVE)) {
    }
    while (::PeekMessageW(&pending, hwnd_, WM_SYSCHAR, WM_SYSDEADCHAR, PM_REMOVE)) {
    }
}

bool WindowsWindow::handleKeyDown(WPARAM virtualKey, bool system) {
    ui::KeyEvent event;
    event.modifiers = currentModifiers();
    switch (virtualKey) {
    case VK_LEFT: event.key = ui::Key::Left; break;
    case VK_RIGHT: event.key = ui::Key::Right; break;
    case VK_UP: event.key = ui::Key::Up; break;
    case VK_DOWN: event.key = ui::Key::Down; break;
    case VK_PRIOR: event.key = ui::Key::PageUp; break;
    case VK_NEXT: event.key = ui::Key::PageDown; break;
    case VK_HOME: event.key = ui::Key::Home; break;
    case VK_END: event.key = ui::Key::End; break;
    case VK_ESCAPE: event.key = ui::Key::Escape; break;
    case VK_RETURN: event.key = ui::Key::Enter; break;
    case VK_TAB:
        event.key = ui::Key::Tab;
        // Ctrl+Tab cycles tabs: the shell checks `control` for it.
        if (event.modifiers.command) event.modifiers.control = true;
        break;
    case VK_BACK: event.key = ui::Key::Backspace; break;
    case VK_DELETE: event.key = ui::Key::Delete; break;
    case VK_SPACE:
        event.key = ui::Key::Space;
        event.text = " ";
        break;
    default: break;
    }
    if (event.key != ui::Key::Unknown) {
        // Alt+Space opens the system menu; Alt+Esc/Tab are system switches.
        if (system && (virtualKey == VK_SPACE || virtualKey == VK_ESCAPE || virtualKey == VK_TAB)) return false;
        const bool handled = dispatchKey(event);
        if (handled || !system) removeQueuedChars();
        return handled || !system;
    }

    // Chords: Ctrl and/or Alt with a character key. Plain typing (and
    // AltGr) arrives as WM_CHAR instead.
    if (!event.modifiers.command && !event.modifiers.option) return false;
    std::string text;
    if (virtualKey == VK_ADD) {
        text = "+";
    } else if (virtualKey == VK_SUBTRACT) {
        text = "-";
    } else {
        // The character the key would type WITHOUT Ctrl/Alt (macOS
        // -[NSEvent characters] for Cmd chords), not a control code.
        BYTE state[256] = {};
        if (!::GetKeyboardState(state)) return false;
        for (const int vk : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU}) state[vk] = 0;
        wchar_t buffer[8] = {};
        const UINT scanCode = ::MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
        const int length = ::ToUnicode(static_cast<UINT>(virtualKey), scanCode, state, buffer, 8,
                                       kToUnicodeNoStateChange);
        if (length <= 0) return false; // dead key or no character
        text = win::narrow(std::wstring_view(buffer, static_cast<std::size_t>(length)));
    }
    if (text.empty() || static_cast<unsigned char>(text[0]) < 0x20 || text[0] == 0x7F) return false;
    const ui::KeyEvent chord = textEvent(std::move(text), event.modifiers);
    // Alt+character (without Ctrl) is a Windows menu mnemonic, never text
    // (unlike macOS Option+key): only an Alt menu shortcut may claim it,
    // otherwise DefWindowProc opens the menu via WM_SYSCHAR.
    const bool altOnly = event.modifiers.option && !event.modifiers.command;
    const bool handled = altOnly ? (menuBar_ != nullptr && menuBar_->handleShortcut(chord)) : dispatchKey(chord);
    // Ctrl chords never type text.
    if (handled || !system) removeQueuedChars();
    return handled || !system;
}

void WindowsWindow::handleChar(wchar_t unit) {
    if (unit >= 0xD800 && unit <= 0xDBFF) {
        highSurrogate_ = unit;
        return;
    }
    std::wstring utf16;
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (highSurrogate_ == 0) return;
        utf16 = {highSurrogate_, unit};
    } else {
        utf16 = {unit};
    }
    highSurrogate_ = 0;
    // Control characters are never typed text (Enter/Tab/Backspace/Escape
    // are handled as keys in WM_KEYDOWN).
    if (utf16.size() == 1 && (unit < 0x20 || unit == 0x7F)) return;
    ui::ModifierFlags modifiers = currentModifiers();
    modifiers.command = false; // AltGr / plain typing: not a chord
    modifiers.option = false;
    dispatchKey(textEvent(win::narrow(utf16), modifiers));
}

LRESULT CALLBACK WindowsWindow::windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* self = static_cast<WindowsWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<WindowsWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) return ::DefWindowProcW(hwnd, message, wParam, lParam);
    return self->handleMessage(message, wParam, lParam);
}

LRESULT WindowsWindow::handleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) return 0;
        const int width = LOWORD(lParam);
        const int height = HIWORD(lParam);
        if (width != pixelWidth_ || height != pixelHeight_) {
            pixelWidth_ = width;
            pixelHeight_ = height;
            if (target_ != nullptr && width > 0 && height > 0) {
                target_->Resize(D2D1_SIZE_U{static_cast<UINT32>(width), static_cast<UINT32>(height)});
            }
            applyRootFrame();
            ::InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        scale_ = scaleForDpi(HIWORD(wParam));
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        // WM_SIZE (from SetWindowPos) relayouts at the new scale.
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                       suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        applyRootFrame();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = ::GetDpiForWindow(hwnd_);
        const double scale = scaleForDpi(dpi);
        RECT frame{0, 0, static_cast<LONG>(std::lround(minLogicalSize_.width * scale)),
                   static_cast<LONG>(std::lround(minLogicalSize_.height * scale))};
        ::AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, ::GetMenu(hwnd_) != nullptr, 0, dpi);
        info->ptMinTrackSize.x = frame.right - frame.left;
        info->ptMinTrackSize.y = frame.bottom - frame.top;
        return 0;
    }
    case WM_ERASEBKGND: return 1; // painted entirely by Direct2D
    case WM_PAINT: {
        PAINTSTRUCT ps;
        ::BeginPaint(hwnd_, &ps);
        paint();
        ::EndPaint(hwnd_, &ps);
        return 0;
    }
    case kRedrawMessage:
        sink_.clearPending();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DISPLAYCHANGE:
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        applyDarkTitleBar();
        if (appearanceChangedHandler_) {
            const auto handler = appearanceChangedHandler_;
            handler();
        }
        break;
    case WM_CLOSE:
        // windowShouldClose: the main loop decides (unsaved-changes prompt);
        // the window itself is destroyed only at teardown.
        if (closeHandler_) {
            const auto handler = closeHandler_;
            handler();
        }
        return 0;
    case WM_INITMENUPOPUP:
        if (menuBar_ != nullptr && HIWORD(lParam) == 0) menuBar_->refresh(reinterpret_cast<HMENU>(wParam));
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == 0 && lParam == 0 && menuBar_ != nullptr && menuBar_->command(LOWORD(wParam))) return 0;
        break;
    case WM_DROPFILES: {
        auto* drop = reinterpret_cast<HDROP>(wParam);
        std::vector<std::filesystem::path> paths;
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0) continue;
            std::wstring path(static_cast<std::size_t>(length) + 1, L'\0');
            const UINT copied = ::DragQueryFileW(drop, i, path.data(), length + 1);
            path.resize(copied);
            if (!path.empty()) paths.emplace_back(path);
        }
        ::DragFinish(drop);
        if (!paths.empty() && fileDropHandler_) {
            const auto handler = fileDropHandler_;
            handler(paths);
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP: {
        const bool down = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN;
        ui::PointerEvent pointer;
        pointer.type = down ? ui::PointerEventType::Down : ui::PointerEventType::Up;
        pointer.button = (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP) ? 1 : 2;
        pointer.position = core::Point{GET_X_LPARAM(lParam) / scale_, GET_Y_LPARAM(lParam) / scale_};
        pointer.modifiers = currentModifiers();
        // A drag that leaves the window keeps delivering moves and its Up.
        if (down) {
            if (buttonsDown_++ == 0) ::SetCapture(hwnd_);
        } else if (buttonsDown_ > 0 && --buttonsDown_ == 0) {
            ::ReleaseCapture();
        }
        dispatchPointer(pointer);
        return 0;
    }
    case WM_CAPTURECHANGED:
        buttonsDown_ = 0;
        return 0;
    case WM_MOUSEMOVE: {
        ui::PointerEvent pointer;
        pointer.type = ui::PointerEventType::Move;
        pointer.position = core::Point{GET_X_LPARAM(lParam) / scale_, GET_Y_LPARAM(lParam) / scale_};
        pointer.modifiers = currentModifiers();
        if ((wParam & MK_LBUTTON) != 0) {
            pointer.button = 1;
        } else if ((wParam & MK_RBUTTON) != 0) {
            pointer.button = 2;
        }
        if (!trackingMouseLeave_) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd_;
            trackingMouseLeave_ = TrackMouseEvent(&track) != FALSE;
        }
        dispatchPointer(pointer);
        return 0;
    }
    case WM_MOUSELEAVE: {
        // Leaving the window clears hover states and pending tooltips.
        trackingMouseLeave_ = false;
        ui::PointerEvent pointer;
        pointer.type = ui::PointerEventType::Exited;
        pointer.position = core::Point{-1.0, -1.0};
        dispatchPointer(pointer);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        // One notch (WHEEL_DELTA) = three 16-point lines, like the X11
        // backend. Rivet's scrollDelta is "positive = offset grows": wheel
        // toward the user (negative delta) scrolls toward the end; Shift
        // turns the vertical wheel horizontal, as on macOS.
        constexpr double kNotch = 48.0;
        const double notches = static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
        POINT where{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ::ScreenToClient(hwnd_, &where);
        ui::PointerEvent pointer;
        pointer.type = ui::PointerEventType::Scroll;
        pointer.position = core::Point{where.x / scale_, where.y / scale_};
        pointer.modifiers = currentModifiers();
        double dx = 0.0;
        double dy = 0.0;
        if (message == WM_MOUSEWHEEL) {
            dy = -notches * kNotch;
            if (pointer.modifiers.shift) std::swap(dx, dy);
        } else {
            dx = notches * kNotch;
        }
        pointer.scrollDelta = core::Point{dx, dy};
        dispatchPointer(pointer);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (handleKeyDown(wParam, message == WM_SYSKEYDOWN)) return 0;
        break;
    case WM_CHAR:
        handleChar(static_cast<wchar_t>(wParam));
        return 0;
    default: break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

} // namespace rivet::platform
