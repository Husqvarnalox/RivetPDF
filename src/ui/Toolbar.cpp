#include "ui/Toolbar.hpp"

#include "ui/Drawing.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {

Toolbar::Toolbar(double height) : height_(std::max(0.0, height)) {}

void Toolbar::addItem(std::unique_ptr<Widget> item, double spacingAfter, Group group) {
    Widget* raw = item.get();
    addChild(std::move(item));
    if (raw != nullptr) {
        spacingAfter_[raw] = spacingAfter;
        groups_[raw] = group;
    }
    layout();
}

double Toolbar::spacingAfterItem(const Widget* item) const {
    const auto it = spacingAfter_.find(item);
    return it == spacingAfter_.end() ? kDefaultItemSpacing : it->second;
}

Toolbar::Group Toolbar::groupOf(const Widget* item) const {
    const auto it = groups_.find(item);
    return it == groups_.end() ? Group::Leading : it->second;
}

void Toolbar::setItemVisible(const Widget* item, bool visible) {
    for (const auto& child : children()) {
        if (child.get() != item) continue;
        if (visible == itemVisible(item)) return;
        if (!visible) {
            if (keyboardItem_ == child.get()) endKeyboardNavigation();
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

void Toolbar::setItemSize(const Widget* item, core::Size size) {
    if (!itemVisible(item)) {
        hiddenSizes_[item] = size;
        return;
    }
    for (const auto& child : children()) {
        if (child.get() != item) continue;
        child->setFrame(core::Rect{child->frame().origin, size});
        layout();
        invalidate();
        return;
    }
}

void Toolbar::setBottomHairline(bool hairline) {
    if (bottomHairline_ == hairline) return;
    bottomHairline_ = hairline;
    invalidate();
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
    const auto place = [this](Widget& child, double x) {
        const core::Rect itemFrame = child.frame();
        const double y = std::max(0.0, (height_ - itemFrame.size.height) / 2.0);
        child.setFrame(core::Rect{x, y, itemFrame.size.width, itemFrame.size.height});
        return itemFrame.size.width + spacingAfterItem(&child);
    };
    const auto groupWidth = [this](Group group) {
        double width = 0.0;
        Widget* last = nullptr;
        for (const auto& child : children()) {
            if (groupOf(child.get()) != group || !itemVisible(child.get())) continue;
            width += child->frame().size.width + spacingAfterItem(child.get());
            last = child.get();
        }
        if (last != nullptr) width -= spacingAfterItem(last);
        return width;
    };

    double leadingEnd = kPadding;
    for (const auto& child : children()) {
        if (groupOf(child.get()) != Group::Leading || !itemVisible(child.get())) continue;
        leadingEnd += place(*child, leadingEnd);
    }
    const double trailingWidth = groupWidth(Group::Trailing);
    const double trailingStart = frame().size.width - kPadding - trailingWidth;
    double x = trailingStart;
    for (const auto& child : children()) {
        if (groupOf(child.get()) != Group::Trailing || !itemVisible(child.get())) continue;
        x += place(*child, x);
    }
    const double centerWidth = groupWidth(Group::Center);
    double centerX = (frame().size.width - centerWidth) / 2.0;
    // Pushed aside instead of overlapping the leading / trailing groups.
    centerX = std::min(centerX, trailingStart - kDefaultItemSpacing * 2.0 - centerWidth);
    centerX = std::max(centerX, leadingEnd + kDefaultItemSpacing);
    for (const auto& child : children()) {
        if (groupOf(child.get()) != Group::Center || !itemVisible(child.get())) continue;
        centerX += place(*child, centerX);
    }
}

void Toolbar::paintSelf(PaintContext& context) const {
    const core::Rect rect = bounds();
    context.fillRect(rect, colors().chromeBackground);
    if (bottomHairline_) drawBottomHairline(context, rect, colors().chromeSeparator);
}

std::vector<Widget*> Toolbar::itemsInVisualOrder() const {
    std::vector<Widget*> items;
    for (const auto& child : children()) {
        if (itemVisible(child.get()) && !child->frame().isEmpty()) items.push_back(child.get());
    }
    std::stable_sort(items.begin(), items.end(),
                     [](const Widget* a, const Widget* b) { return a->frame().minX() < b->frame().minX(); });
    return items;
}

std::vector<Toolbar::Stop> Toolbar::keyboardStops() const {
    std::vector<Stop> stops;
    for (Widget* item : itemsInVisualOrder()) {
        for (int s = 0; s < item->keyboardStopCount(); ++s) stops.push_back(Stop{item, s});
    }
    return stops;
}

void Toolbar::focusStop(int index) {
    const std::vector<Stop> stops = keyboardStops();
    if (keyboardItem_ != nullptr) keyboardItem_->setKeyboardStop(-1);
    keyboardItem_ = nullptr;
    keyboardIndex_ = -1;
    if (stops.empty()) return;
    const int count = static_cast<int>(stops.size());
    keyboardIndex_ = ((index % count) + count) % count;
    keyboardItem_ = stops[static_cast<std::size_t>(keyboardIndex_)].item;
    keyboardItem_->setKeyboardStop(stops[static_cast<std::size_t>(keyboardIndex_)].stop);
    invalidate();
}

void Toolbar::beginKeyboardNavigation(bool fromEnd) {
    focusStop(fromEnd ? -1 : 0);
}

void Toolbar::endKeyboardNavigation() {
    if (keyboardItem_ != nullptr) keyboardItem_->setKeyboardStop(-1);
    keyboardItem_ = nullptr;
    keyboardIndex_ = -1;
    invalidate();
}

bool Toolbar::handleKey(const KeyEvent& event) {
    if (!keyboardNavigating()) return false;
    switch (event.key) {
    case Key::Tab:
        if (event.modifiers.command || event.modifiers.control) return false;
        focusStop(keyboardIndex_ + (event.modifiers.shift ? -1 : 1));
        return true;
    case Key::Right: focusStop(keyboardIndex_ + 1); return true;
    case Key::Left: focusStop(keyboardIndex_ - 1); return true;
    case Key::Space:
    case Key::Enter: {
        const std::vector<Stop> stops = keyboardStops();
        if (keyboardIndex_ < static_cast<int>(stops.size())) {
            const Stop stop = stops[static_cast<std::size_t>(keyboardIndex_)];
            stop.item->activateKeyboardStop(stop.stop);
        }
        return true;
    }
    case Key::Escape: endKeyboardNavigation(); return true;
    default: return false;
    }
}

} // namespace rivet::ui
