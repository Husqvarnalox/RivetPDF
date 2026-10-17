// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rivet::platform {

// Key chord of a menu item. `primary` is Ctrl on Linux (Command on macOS);
// `key` is one lowercase character.
struct MenuBarShortcut {
    std::string key;
    bool primary = true;
    bool shift = false;
    bool alt = false;
};

struct MenuBarItem {
    std::string title;
    int id = 0; // handed back to the callbacks
    std::optional<MenuBarShortcut> shortcut;
    bool separatorBefore = false;
    bool checkable = false;
};

struct MenuBarMenu {
    std::string title;
    std::vector<MenuBarItem> items;
};

// In-window menu bar for the X11 shell (X11 has no global menu): a strip
// across the top of the window with drop-down menus painted over the
// content. It knows nothing about the application: the composition root
// translates app::buildMenuBar() into MenuBarMenu values and answers the
// enable/check/activate callbacks through ShellController.
//
// Geometry is in window-logical coordinates (the strip occupies
// [0, kHeight) at the top). Main thread only.
class LinuxMenuBar {
public:
    struct Callbacks {
        std::function<bool(int id)> isEnabled;
        std::function<std::optional<bool>(int id)> isChecked;
        std::function<void(int id)> activate;
    };

    static constexpr double kHeight = 26.0;

    LinuxMenuBar(std::vector<MenuBarMenu> menus, Callbacks callbacks);

    // Replaces (or, with {}, drops) the callbacks; teardown clears them
    // before the shell they capture is destroyed.
    void setCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

    double height() const { return kHeight; }
    bool isOpen() const { return openMenu_ >= 0; }

    void paintBar(ui::PaintContext& context, double width) const;
    // Drop-down of the open menu, painted after (over) the content.
    void paintOverlay(ui::PaintContext& context) const;

    // Pointer input in window-logical coordinates. Returns true when the
    // menu consumed the event (always while a menu is open).
    bool onPointer(const ui::PointerEvent& event);
    // Keyboard navigation while open (consumes every key).
    bool onKey(const ui::KeyEvent& event);
    // Runs the enabled item whose shortcut matches. Disabled or unknown
    // chords are left to the content (returns false).
    bool handleShortcut(const ui::KeyEvent& event);

    void openMenu(std::size_t index);
    void close();

    // "Ctrl+Shift+S" style label.
    static std::string shortcutLabel(const MenuBarShortcut& shortcut);

private:
    struct ItemSlot {
        core::Rect rect; // window-logical
        int itemIndex = -1;
    };

    core::Rect titleRect(std::size_t menuIndex) const;
    core::Rect dropdownRect(std::size_t menuIndex) const;
    std::vector<ItemSlot> itemSlots(std::size_t menuIndex) const;
    int menuAt(core::Point point) const;
    int itemAt(core::Point point) const;
    bool enabled(const MenuBarItem& item) const;
    void activate(const MenuBarItem& item);
    void moveHover(int delta);

    std::vector<MenuBarMenu> menus_;
    Callbacks callbacks_;
    int openMenu_ = -1;
    int hoverItem_ = -1;
};

} // namespace rivet::platform
