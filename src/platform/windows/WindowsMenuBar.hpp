// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/UiTypes.hpp"

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct HMENU__;

namespace rivet::platform {

// Portable description of the menu bar (filled from app::buildMenuBar() by
// the composition root; the backend never depends on rivet::app).
struct WinMenuShortcut {
    std::string key; // single lowercase character, e.g. "o", "1", "="
    bool primary = true; // Ctrl
    bool shift = false;
    bool alt = false;
};

struct WinMenuItem {
    std::string title;
    int id = 0; // flat index; the WM_COMMAND id is kCommandBase + id
    bool separatorBefore = false;
    bool checkable = false;
    std::optional<WinMenuShortcut> shortcut;
};

struct WinMenu {
    std::string title;
    std::vector<WinMenuItem> items;
};

// Native Win32 menu bar (HMENU) built from the portable model. Shortcuts are
// shown right-aligned ("Ctrl+Shift+S") and matched by handleShortcut() from
// the window's key handling - there is no accelerator table, so the menu's
// enable state decides like NSMenu key equivalents do. Enable/check state is
// refreshed from the callbacks on WM_INITMENUPOPUP. Top-level titles get an
// Alt mnemonic on their first letter. Main thread only.
class WindowsMenuBar {
public:
    static constexpr unsigned kCommandBase = 1000;

    struct Callbacks {
        std::function<bool(int id)> isEnabled;
        std::function<std::optional<bool>(int id)> isChecked;
        std::function<void(int id)> activate;
    };

    explicit WindowsMenuBar(std::vector<WinMenu> menus);
    ~WindowsMenuBar();
    WindowsMenuBar(const WindowsMenuBar&) = delete;
    WindowsMenuBar& operator=(const WindowsMenuBar&) = delete;

    HMENU__* handle() const { return bar_; }
    void setCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

    // WM_INITMENUPOPUP for one of our submenus.
    void refresh(HMENU__* popup);
    // WM_COMMAND from a menu item. Returns true when the id was ours.
    bool command(unsigned commandId);
    // Ctrl/Alt chord matching a shortcut of an ENABLED item: activates it and
    // returns true. A chord of a disabled item returns false (the content may
    // still use it).
    bool handleShortcut(const ui::KeyEvent& event);

    // Releases the HMENU from the window before destruction (DestroyWindow
    // destroys an attached menu).
    void detached() { attached_ = false; }
    void attached() { attached_ = true; }

private:
    std::vector<WinMenu> menus_;
    std::vector<HMENU__*> popups_;
    HMENU__* bar_ = nullptr;
    Callbacks callbacks_;
    bool attached_ = false;
};

} // namespace rivet::platform
