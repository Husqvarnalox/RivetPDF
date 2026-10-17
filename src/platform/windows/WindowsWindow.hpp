// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Size.hpp"
#include "ui/UiTypes.hpp"
#include "ui/Widget.hpp"

#include "WinCommon.hpp" // after Rivet headers: <windows.h> defines macros

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct ID2D1Factory;
struct ID2D1HwndRenderTarget;

namespace rivet::platform {

class WindowsMenuBar;

// Top-level Win32 window hosting a Rivet widget tree: the Windows counterpart
// of RivetContentView / X11Window.
//   - Direct2D HWND render target whose DPI is 96 * scale, so widget code
//     paints in logical points; per-monitor-v2 DPI (WM_DPICHANGED resizes to
//     the suggested rect and relayouts).
//   - Input: Ctrl is the primary ("command") modifier, Alt is option, the
//     Windows key stands in for the macOS Control key. Chords resolve to the
//     character the key types WITHOUT Ctrl/Alt (like -[NSEvent characters]);
//     plain text arrives through WM_CHAR (dead keys, IME, surrogate pairs).
//     Menu shortcuts are matched before the key handler, as NSMenu key
//     equivalents are. Alt+character stays a menu mnemonic (it never types
//     text or reaches the key handler) unless it is an Alt menu shortcut.
//   - Thread-safe redraw sink: coalesces into one InvalidateRect per burst;
//     painting happens on WM_PAINT.
//   - Native HMENU menu bar (WM_INITMENUPOPUP refresh, WM_COMMAND routing),
//     WM_DROPFILES file drops, WM_CLOSE -> close handler, dark title bar when
//     the system app theme is dark.
// Main thread only, except the redraw sink.
class WindowsWindow {
public:
    struct Options {
        std::string title = "Rivet";
        core::Size logicalSize{800.0, 600.0};
        core::Size minLogicalSize{0.0, 0.0};
    };

    explicit WindowsWindow(const Options& options);
    ~WindowsWindow();
    WindowsWindow(const WindowsWindow&) = delete;
    WindowsWindow& operator=(const WindowsWindow&) = delete;

    bool valid() const { return hwnd_ != nullptr; }
    HWND hwnd() const { return hwnd_; }

    void show(int showCommand);
    void setTitle(const std::string& title);
    void setDocumentEdited(bool edited);

    // Non-owning; installing propagates the redraw sink and applies the
    // current client bounds as the root frame (runs the shell layout).
    void setRootWidget(ui::Widget* widget);
    ui::IRedrawSink* redrawSink() { return &sink_; }

    void setKeyHandler(std::function<bool(const ui::KeyEvent&)> handler) { keyHandler_ = std::move(handler); }
    void setFileDropHandler(std::function<void(const std::vector<std::filesystem::path>&)> handler);
    // WM_CLOSE (title-bar close button, Alt+F4).
    void setCloseHandler(std::function<void()> handler) { closeHandler_ = std::move(handler); }
    // WM_SETTINGCHANGE / WM_THEMECHANGED / WM_DWMCOLORIZATIONCOLORCHANGED
    // (the appearance service re-reads the theme and accent).
    void setAppearanceChangedHandler(std::function<void()> handler) { appearanceChangedHandler_ = std::move(handler); }
    // Non-owning; null detaches the native menu.
    void setMenuBar(WindowsMenuBar* menuBar);

    double scale() const { return scale_; }
    core::Size contentLogicalSize() const;

private:
    class Sink final : public ui::IRedrawSink {
    public:
        explicit Sink(WindowsWindow& owner) : owner_(owner) {}
        void requestRedraw() override; // any thread
        void clearPending() { pending_.store(false); }

    private:
        WindowsWindow& owner_;
        std::atomic<bool> pending_{false};
    };

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void applyRootFrame();
    void paint();
    void releaseTarget();
    void updateTitle();
    void applyDarkTitleBar();
    void dispatchPointer(const ui::PointerEvent& event);
    bool handleKeyDown(WPARAM virtualKey, bool system);
    void handleChar(wchar_t unit);
    bool dispatchKey(const ui::KeyEvent& event);
    void removeQueuedChars();

    Sink sink_{*this};
    HWND hwnd_ = nullptr;
    bool trackingMouseLeave_ = false; // TrackMouseEvent(TME_LEAVE) armed
    ID2D1Factory* factory_ = nullptr;
    ID2D1HwndRenderTarget* target_ = nullptr;
    double scale_ = 1.0;
    int pixelWidth_ = 0;
    int pixelHeight_ = 0;
    core::Size minLogicalSize_;
    core::Size initialLogicalSize_;
    ui::Widget* root_ = nullptr;
    WindowsMenuBar* menuBar_ = nullptr;
    std::function<bool(const ui::KeyEvent&)> keyHandler_;
    std::function<void(const std::vector<std::filesystem::path>&)> fileDropHandler_;
    std::function<void()> closeHandler_;
    std::function<void()> appearanceChangedHandler_;
    std::string title_;
    bool edited_ = false;
    wchar_t highSurrogate_ = 0;
    int buttonsDown_ = 0;
};

} // namespace rivet::platform
