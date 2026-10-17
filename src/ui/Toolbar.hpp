#pragma once

#include "ui/Widget.hpp"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::ui {

// Horizontal bar of widgets in three groups (ADR-0033 §5): leading items
// laid out left-to-right from the left padding, trailing items right-
// aligned, center items centered in the bar (pushed aside rather than
// overlapping the other groups).
//
// layout() positions children by x-advance using each child's CURRENT frame
// size; it never resizes items. Callers must set item sizes via setFrame()
// before the toolbar lays out — addItem() runs a layout pass with whatever
// sizes are current, as does any later setFrame() on the toolbar itself.
// Items are vertically centered in the bar and keep their own height.
//
// Keyboard navigation (ADR-0033 §5): beginKeyboardNavigation() focuses the
// first keyboard stop; Tab/Shift+Tab and Left/Right move between the stops
// of visible items in visual order, Space/Enter activates, Escape ends it.
class Toolbar : public Widget {
public:
    enum class Group : std::uint8_t { Leading, Center, Trailing };

    static constexpr double kDefaultHeight = 40.0;
    static constexpr double kDefaultItemSpacing = 8.0;
    static constexpr double kPadding = 10.0;

    explicit Toolbar(double height = kDefaultHeight);

    // Takes ownership of the item. spacingAfter is the gap inserted after it
    // (the gap between the last item and the right padding is not counted in
    // preferredSize's width).
    void addItem(std::unique_ptr<Widget> item, double spacingAfter = kDefaultItemSpacing,
                 Group group = Group::Leading);

    // Hides/shows an item (it keeps its size, takes no space and consumes no
    // input while hidden; a zero-size frame is the hidden state). No-op for a
    // widget that is not an item of this toolbar.
    void setItemVisible(const Widget* item, bool visible);
    bool itemVisible(const Widget* item) const { return !hiddenSizes_.contains(item); }
    // Resizes an item (keeps its visibility) and relayouts.
    void setItemSize(const Widget* item, core::Size size);

    double height() const { return height_; }
    // Hairline under the bar (off when a context bar continues the chrome).
    void setBottomHairline(bool hairline);

    // Width: padding + item widths + inter-item gaps; height: bar height.
    core::Size preferredSize(const PaintContext& context) const override;

    void layout() override;
    void paintSelf(PaintContext& context) const override;

    // Keyboard navigation.
    bool keyboardNavigating() const { return keyboardIndex_ >= 0; }
    void beginKeyboardNavigation(bool fromEnd = false);
    void endKeyboardNavigation();
    // Handles Tab/arrows/Space/Enter/Escape while navigating.
    bool handleKey(const KeyEvent& event);

private:
    struct Stop {
        Widget* item;
        int stop;
    };
    double spacingAfterItem(const Widget* item) const;
    Group groupOf(const Widget* item) const;
    std::vector<Widget*> itemsInVisualOrder() const;
    std::vector<Stop> keyboardStops() const;
    void focusStop(int index);

    double height_;
    bool bottomHairline_ = true;
    std::unordered_map<const Widget*, double> spacingAfter_;
    std::unordered_map<const Widget*, Group> groups_;
    // Hidden items and the size to restore when they come back.
    std::unordered_map<const Widget*, core::Size> hiddenSizes_;
    int keyboardIndex_ = -1;
    Widget* keyboardItem_ = nullptr;
};

} // namespace rivet::ui
