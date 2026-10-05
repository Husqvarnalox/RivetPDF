#include "ui/Toolbar.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {

Toolbar::Toolbar(double height) : height_(std::max(0.0, height)) {}

void Toolbar::addItem(std::unique_ptr<Widget> item, double spacingAfter) {
    Widget* raw = item.get();
    addChild(std::move(item));
    if (raw != nullptr) spacingAfter_[raw] = spacingAfter;
    layout();
}

double Toolbar::spacingAfterItem(const Widget* item) const {
    const auto it = spacingAfter_.find(item);
    return it == spacingAfter_.end() ? kDefaultItemSpacing : it->second;
}

void Toolbar::setItemVisible(const Widget* item, bool visible) {
    for (const auto& child : children()) {
        if (child.get() != item) continue;
        if (visible == itemVisible(item)) return;
        if (!visible) {
            hiddenSizes_[item] = child->frame().size;
            child->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
        } else {
            const core::Size size = hiddenSizes_[item];
            hiddenSizes_.erase(item);
            child->setFrame(core::Rect{0.0, 0.0, size.width, size.height});
        }
        layout();
        invalidate();
        return;
    }
}

core::Size Toolbar::preferredSize(const PaintContext&) const {
    double width = 2.0 * kPadding;
    const auto& items = children();
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!itemVisible(items[i].get())) continue;
        width += items[i]->frame().size.width;
        if (i + 1 < items.size()) width += spacingAfterItem(items[i].get());
    }
    return core::Size{width, height_};
}

void Toolbar::layout() {
    // X-advance uses each item's current frame size; items keep their sizes
    // and are vertically centered in the bar (see the class comment).
    double x = kPadding;
    for (const auto& child : children()) {
        if (!itemVisible(child.get())) continue; // keeps its hidden frame
        const core::Rect itemFrame = child->frame();
        const double y = std::max(0.0, (height_ - itemFrame.size.height) / 2.0);
        child->setFrame(core::Rect{x, y, itemFrame.size.width, itemFrame.size.height});
        x += itemFrame.size.width + spacingAfterItem(child.get());
    }
}

void Toolbar::paintSelf(PaintContext& context) const {
    const core::Rect rect = bounds();
    context.fillRect(rect, Color::gray(0.93));
    // Subtle bottom hairline separating the bar from the content below.
    if (rect.size.height > 0.0) {
        const double y = rect.maxY() - 0.5;
        context.drawLine(core::Point{rect.minX(), y}, core::Point{rect.maxX(), y},
                         Color::rgba(0.0, 0.0, 0.0, 0.12), 1.0);
    }
}

} // namespace rivet::ui
