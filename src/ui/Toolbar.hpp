#pragma once

#include "ui/Widget.hpp"

#include <memory>
#include <unordered_map>

namespace rivet::ui {

// Horizontal bar of widgets, laid out left-to-right with a uniform height.
//
// layout() positions children by x-advance using each child's CURRENT frame
// size; it never resizes items. Callers must set item sizes via setFrame()
// (or use preferredSize() of each item) before the toolbar lays out —
// addItem() runs a layout pass with whatever sizes are current, as does any
// later setFrame() on the toolbar itself. Items are vertically centered in
// the bar and keep their own height.
class Toolbar : public Widget {
public:
    static constexpr double kDefaultHeight = 40.0;
    static constexpr double kDefaultItemSpacing = 8.0;
    static constexpr double kPadding = 10.0;

    explicit Toolbar(double height = kDefaultHeight);

    // Takes ownership of the item. spacingAfter is the gap inserted after it
    // (the gap between the last item and the right padding is not counted in
    // preferredSize's width).
    void addItem(std::unique_ptr<Widget> item, double spacingAfter = kDefaultItemSpacing);

    // Hides/shows an item (it keeps its size, takes no space and consumes no
    // input while hidden; a zero-size frame is the hidden state). No-op for a
    // widget that is not an item of this toolbar.
    void setItemVisible(const Widget* item, bool visible);
    bool itemVisible(const Widget* item) const { return !hiddenSizes_.contains(item); }

    double height() const { return height_; }

    // Width: padding + item widths + inter-item gaps; height: bar height.
    core::Size preferredSize(const PaintContext& context) const override;

    void layout() override;
    void paintSelf(PaintContext& context) const override;

private:
    double spacingAfterItem(const Widget* item) const;

    double height_;
    std::unordered_map<const Widget*, double> spacingAfter_;
    // Hidden items and the size to restore when they come back.
    std::unordered_map<const Widget*, core::Size> hiddenSizes_;
};

} // namespace rivet::ui
