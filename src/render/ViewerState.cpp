// SPDX-License-Identifier: MPL-2.0
#include "render/ViewerState.hpp"

#include "core/geometry/Rect.hpp"

namespace rivet::render {

ViewerState::ViewerState() {
    // Single funnel for zoom changes: forward to the one ViewerState callback
    // so the bound viewport sees zoom and scroll changes through the same hook.
    zoom_.setCallback([this](double) {
        if (onChanged_) onChanged_();
    });
}

void ViewerState::setScrollOffsetPoints(const core::Point& offset) {
    if (core::Point::nearlyEqual(scrollOffset_, offset)) return;
    scrollOffset_ = offset;
    if (onChanged_) onChanged_();
}

void ViewerState::setCallback(std::function<void()> onChanged) {
    onChanged_ = std::move(onChanged);
}

} // namespace rivet::render
