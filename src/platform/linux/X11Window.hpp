// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Size.hpp"
#include "ui/UiTypes.hpp"
#include "ui/Widget.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

// Opaque Xlib/cairo types: this header must stay free of <X11/Xlib.h> (its
// macros collide with Rivet names; see X11Includes.hpp).
struct _XDisplay;
union _XEvent;
struct _XIM;
struct _XIC;
typedef struct _cairo_surface cairo_surface_t;

namespace rivet::platform {

class LinuxMainThreadDispatcher;
class LinuxMenuBar;
class X11Window;

// The X server connection shared by every Rivet window: atoms, input method,
// HiDPI scale, the CLIPBOARD selection, and the event pump that multiplexes
// the X socket with the main-thread dispatcher's wake pipe (and, for modal
// helpers, one extra fd).
//
// Main thread only, except where noted.
class X11Connection {
public:
    // Opens $DISPLAY. Returns nullptr (and a reason) when no X server is
    // reachable.
    static std::unique_ptr<X11Connection> open(LinuxMainThreadDispatcher& dispatcher, std::string& error);
    ~X11Connection();
    X11Connection(const X11Connection&) = delete;
    X11Connection& operator=(const X11Connection&) = delete;

    _XDisplay* display() const { return display_; }
    int screen() const { return screen_; }
    _XIM* inputMethod() const { return inputMethod_; }
    LinuxMainThreadDispatcher& dispatcher() const { return dispatcher_; }

    // Device pixels per logical point (macOS backingScaleFactor analogue):
    // $RIVET_SCALE, else $GDK_SCALE, else the Xft.dpi resource / 96, else 1.
    double scale() const { return scale_; }

    unsigned long atom(const char* name);

    enum class Mode : std::uint8_t {
        // Every event is delivered, main-thread tasks run, signals are
        // handled.
        Normal,
        // App-modal (like an NSAlert / NSOpenPanel session): only
        // `modalWindow` (may be null) receives input; other windows still
        // repaint and the clipboard is still served. Main-thread tasks are
        // NOT run until the modal session ends.
        Modal,
    };

    // One loop iteration: waits (poll, `timeoutMs` = -1 for no limit) until
    // the X socket, the wake pipe or `extraFd` (-1 = none) is readable, then
    // processes every queued X event, runs main-thread tasks (Normal) and
    // repaints windows that requested it.
    void pump(Mode mode, X11Window* modalWindow, int extraFd, int timeoutMs);

    // Called from pump(Normal) after SIGINT/SIGTERM (installSignalHandlers).
    void setSignalQuitHandler(std::function<void()> handler) { signalQuit_ = std::move(handler); }
    void installSignalHandlers();

    // CLIPBOARD selection (UTF-8). setClipboardText takes ownership and
    // serves SelectionRequest events while Rivet stays the owner (the text is
    // lost when Rivet exits unless a clipboard manager copies it).
    bool setClipboardText(std::string text);
    // Current CLIPBOARD text: our own text while we own it; otherwise a
    // UTF8_STRING (then STRING) conversion with a 1 second timeout. Returns
    // empty when there is no text or the owner uses INCR transfers.
    std::string clipboardText();

    // Window registry (X11Window registers itself).
    void registerWindow(unsigned long id, X11Window* window) { windows_[id] = window; }
    void unregisterWindow(unsigned long id) { windows_.erase(id); }

private:
    X11Connection(_XDisplay* display, LinuxMainThreadDispatcher& dispatcher);

    void dispatchEvent(_XEvent& event, Mode mode, X11Window* modalWindow);
    void handleSelectionRequest(_XEvent& event);
    bool readSelectionProperty(unsigned long target, std::string& out);

    _XDisplay* display_ = nullptr;
    int screen_ = 0;
    LinuxMainThreadDispatcher& dispatcher_;
    _XIM* inputMethod_ = nullptr;
    double scale_ = 1.0;
    std::map<std::string, unsigned long> atoms_;
    std::map<unsigned long, X11Window*> windows_;
    unsigned long selectionWindow_ = 0; // unmapped helper window owning CLIPBOARD
    bool ownsClipboard_ = false;
    std::string clipboard_;
    std::function<void()> signalQuit_;
};

// A top-level X11 window hosting a Rivet widget tree: the X11 counterpart of
// RivetContentView. Paints through LinuxPaintContext into a back buffer and
// blits it to the window; translates X input into ui:: events (Ctrl is the
// primary/"command" modifier); thread-safe redraw sink that coalesces into
// one paint per loop iteration; optional in-window menu bar; XDND file drops.
class X11Window {
public:
    struct Options {
        std::string title = "Rivet";
        core::Size logicalSize{800.0, 600.0};
        core::Size minLogicalSize{0.0, 0.0};
        bool resizable = true;
        // Dialog: transient for `owner`, _NET_WM_WINDOW_TYPE_DIALOG, modal
        // state, centered over the owner.
        bool dialog = false;
        X11Window* owner = nullptr;
    };

    X11Window(X11Connection& connection, const Options& options);
    ~X11Window();
    X11Window(const X11Window&) = delete;
    X11Window& operator=(const X11Window&) = delete;

    unsigned long id() const { return window_; }
    X11Connection& connection() const { return connection_; }

    void show();
    // Gives the window keyboard focus once it is viewable (dialogs; a no-op
    // after the first success). Window managers usually focus new transient
    // windows themselves; this also covers WM-less servers.
    void focusWhenViewable();
    void setTitle(const std::string& title);
    void setDocumentEdited(bool edited);

    // Non-owning; installing propagates the redraw sink and applies the
    // current content bounds as the root frame (runs the shell layout).
    void setRootWidget(ui::Widget* widget);
    ui::IRedrawSink* redrawSink() { return &sink_; }

    void setKeyHandler(std::function<bool(const ui::KeyEvent&)> handler) { keyHandler_ = std::move(handler); }
    void setFileDropHandler(std::function<void(const std::vector<std::filesystem::path>&)> handler) {
        fileDropHandler_ = std::move(handler);
    }
    // WM_DELETE_WINDOW (the title-bar close button).
    void setCloseHandler(std::function<void()> handler) { closeHandler_ = std::move(handler); }
    // Non-owning; null removes the strip.
    void setMenuBar(LinuxMenuBar* menuBar);

    // Content size in logical points (below the menu bar).
    core::Size contentLogicalSize() const;

    // Scripted QA hooks (ShellServices::captureWindowPng / resizeWindow):
    // paints the window into its back buffer and writes that as PNG; resizes
    // so the content area becomes the given logical size (clamped to the
    // minimum size), applied immediately.
    bool writePng(const std::filesystem::path& path);
    void resizeContent(double width, double height);

    // Used by X11Connection.
    void handleEvent(_XEvent& event, bool inputAllowed);
    void requestPaint() { sink_.requestRedraw(); }
    void paintIfNeeded();

private:
    class Sink final : public ui::IRedrawSink {
    public:
        explicit Sink(X11Window& owner) : owner_(owner) {}
        void requestRedraw() override; // any thread
        bool consume() { return dirty_.exchange(false); }

    private:
        X11Window& owner_;
        std::atomic<bool> dirty_{true};
    };

    void applyRootFrame();
    void paint();
    void handleKey(_XEvent& event);
    void handlePointer(const ui::PointerEvent& event);
    void handleClientMessage(_XEvent& event);
    void handleDropData(_XEvent& event);
    void sendDndStatus(bool accept);
    void sendDndFinished(bool accepted);
    double contentTop() const;
    void updateTitle();

    X11Connection& connection_;
    Sink sink_{*this};
    unsigned long window_ = 0;
    _XIC* inputContext_ = nullptr;
    cairo_surface_t* windowSurface_ = nullptr;
    cairo_surface_t* backBuffer_ = nullptr;
    int pixelWidth_ = 0;
    int pixelHeight_ = 0;
    core::Size minLogicalSize_;
    ui::Widget* root_ = nullptr;
    LinuxMenuBar* menuBar_ = nullptr;
    std::function<bool(const ui::KeyEvent&)> keyHandler_;
    std::function<void(const std::vector<std::filesystem::path>&)> fileDropHandler_;
    std::function<void()> closeHandler_;
    bool contentPressed_ = false; // a press inside the content owns the drag
    std::string title_;
    bool edited_ = false;
    bool focusGiven_ = false;
    // XDND state.
    unsigned long dndSource_ = 0;
    bool dndAcceptable_ = false;
    int dndVersion_ = 0;
};

} // namespace rivet::platform
