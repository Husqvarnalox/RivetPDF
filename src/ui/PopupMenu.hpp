// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::ui {

// In-window popover menu (the zoom presets, the toolbar's overflow "more"
// menu): a themed list with optional check marks, shortcuts, separators and
// one level of cascading submenus. While open it covers its parent (its
// frame is the parent's bounds) so a click outside closes it; closed it has
// an empty frame and takes no input. Keyboard: Up/Down, Right/Left for
// submenus, Enter/Space activates, Escape closes. The chosen item's action
// runs after the menu closed. Main thread only.
class PopupMenu final : public Widget {
public:
    struct Item {
        std::string title;
        std::string shortcut; // display text ("⌘S"), may be empty
        bool enabled = true;
        bool checked = false;
        bool separatorBefore = false;
        std::vector<Item> submenu; // non-empty: a cascading submenu
        std::function<void()> action;
    };

    static constexpr double kItemHeight = 24.0;
    static constexpr double kSeparatorHeight = 9.0;
    static constexpr double kPadding = 5.0;
    static constexpr double kMinWidth = 160.0;

    // Opens the menu under `anchor` (parent coordinates) covering `area`
    // (normally the parent's bounds). Replaces an open menu.
    void open(std::vector<Item> items, const core::Rect& anchor, const core::Rect& area);
    void close();
    bool isOpen() const { return open_; }
    void setOnClosed(std::function<void()> onClosed) { onClosed_ = std::move(onClosed); }

    // Test/diagnostic access.
    const std::vector<Item>& items() const { return items_; }
    core::Rect menuRect() const { return levels_.empty() ? core::Rect{} : levels_.front().rect; }

    bool onMouse(const PointerEvent& event) override;
    bool onKey(const KeyEvent& event) override;
    void paint(PaintContext& context) const override { paintSelf(context); }
    void paintSelf(PaintContext& context) const override;

private:
    struct Level {
        const std::vector<Item>* items = nullptr;
        core::Rect rect; // local coordinates
        int hover = -1;
    };

    static double itemsHeight(const std::vector<Item>& items);
    static double itemsWidth(const std::vector<Item>& items);
    core::Rect placeLevel(const std::vector<Item>& items, core::Point topLeft, double avoidLeftX) const;
    int itemAt(const Level& level, core::Point point) const;
    core::Rect itemRect(const Level& level, std::size_t index) const;
    void openSubmenu(std::size_t levelIndex, std::size_t itemIndex);
    void activate(const Item& item);
    void moveHover(int delta);

    std::vector<Item> items_;
    std::vector<Level> levels_;
    bool open_ = false;
    std::function<void()> onClosed_;
};

} // namespace rivet::ui
