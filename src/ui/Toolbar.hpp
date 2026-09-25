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

    double height() const { return height_; }

    // Width: padding + item widths + inter-item gaps; height: bar height.
    core::Size preferredSize(const PaintContext& context) const override;

    void layout() override;
    void paintSelf(PaintContext& context) const override;

private:
    double spacingAfterItem(const Widget* item) const;

    double height_;
    std::unordered_map<const Widget*, double> spacingAfter_;
};

} // namespace rivet::ui
