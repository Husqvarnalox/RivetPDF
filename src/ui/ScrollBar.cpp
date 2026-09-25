// SPDX-License-Identifier: MPL-2.0
#include "ui/ScrollBar.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::ui {
namespace {

constexpr Color kTrackColor = Color::gray(0.92);
constexpr Color kThumbColor = Color::gray(0.72);
constexpr Color kThumbDragColor = Color::gray(0.62);
constexpr double kCornerRadius = 4.0;

} // namespace

ScrollBar::ScrollBar(ScrollOrientation orientation) : orientation_(orientation) {}

double ScrollBar::maxOffset() const {
    if (!std::isfinite(contentExtent_) || !std::isfinite(viewportExtent_)) return 0.0;
    return std::max(0.0, contentExtent_ - viewportExtent_);
}

bool ScrollBar::isUsable() const {
    return std::isfinite(contentExtent_) && std::isfinite(viewportExtent_) &&
           contentExtent_ > viewportExtent_;
}

void ScrollBar::setExtents(double viewportExtent, double contentExtent) {
    // Reject invalid input as a whole so a NaN/Inf/negative can never corrupt
    // the proportional thumb math.
    if (!std::isfinite(viewportExtent) || !std::isfinite(contentExtent) ||
        viewportExtent < 0.0 || contentExtent < 0.0) {
        return;
    }
    if (viewportExtent_ == viewportExtent && contentExtent_ == contentExtent) return;
    viewportExtent_ = viewportExtent;
    contentExtent_ = contentExtent;
    // Keep the offset inside the (possibly smaller) range.
    offset_ = std::clamp(offset_, 0.0, maxOffset());
    invalidate();
}

void ScrollBar::setOffset(double offset) {
    if (!std::isfinite(offset)) return; // non-finite: keep the current offset
    const double clamped = std::clamp(offset, 0.0, maxOffset());
    if (offset_ == clamped) return;
    offset_ = clamped;
    invalidate();
}

void ScrollBar::setOnScroll(std::function<void(double)> onScroll) {
    onScroll_ = std::move(onScroll);
}

core::Size ScrollBar::preferredSize(const PaintContext&) const {
    // The cross axis is fixed; the scroll axis keeps the current extent.
    if (orientation_ == ScrollOrientation::Vertical) {
        return core::Size{kThickness, frame().size.height};
    }
    return core::Size{frame().size.width, kThickness};
}

double ScrollBar::trackLength() const {
    const double length = orientation_ == ScrollOrientation::Vertical ? bounds().size.height
                                                                      : bounds().size.width;
    return std::max(0.0, length - 2.0 * kTrackPadding);
}

double ScrollBar::thumbLength() const {
    if (!isUsable()) return 0.0;
    const double track = trackLength();
    if (track <= 0.0) return 0.0;
    const double proportion = std::clamp(viewportExtent_ / contentExtent_, 0.0, 1.0);
    // At least kMinThumbLength so a huge document keeps the thumb grabbable,
    // never longer than the track.
    return std::min(track, std::max(track * proportion, kMinThumbLength));
}

double ScrollBar::trackAxisOrigin() const {
    const core::Rect track = trackRect();
    return orientation_ == ScrollOrientation::Vertical ? track.minY() : track.minX();
}

double ScrollBar::thumbStartForOffset(double offset) const {
    const double span = trackLength() - thumbLength();
    const double maximum = maxOffset();
    if (span <= 0.0 || maximum <= 0.0) return trackAxisOrigin();
    return trackAxisOrigin() + span * (offset / maximum);
}

double ScrollBar::offsetForThumbStart(double thumbStart) const {
    const double maximum = maxOffset();
    const double span = trackLength() - thumbLength();
    if (maximum <= 0.0) return 0.0;
    if (span <= 0.0) return thumbStart <= trackAxisOrigin() ? 0.0 : maximum;
    const double relative = (thumbStart - trackAxisOrigin()) / span;
    return std::clamp(relative, 0.0, 1.0) * maximum;
}

double ScrollBar::axisCoordinate(const core::Point& position) const {
    return orientation_ == ScrollOrientation::Vertical ? position.y : position.x;
}

core::Rect ScrollBar::trackRect() const {
    const core::Rect area = bounds();
    if (orientation_ == ScrollOrientation::Vertical) {
        return core::Rect{core::Point{area.minX(), area.minY() + kTrackPadding},
                          core::Size{area.size.width,
                                     std::max(0.0, area.size.height - 2.0 * kTrackPadding)}};
    }
    return core::Rect{
        core::Point{area.minX() + kTrackPadding, area.minY()},
        core::Size{std::max(0.0, area.size.width - 2.0 * kTrackPadding), area.size.height}};
}

core::Rect ScrollBar::thumbRect() const {
    const core::Rect track = trackRect();
    const double start = thumbStartForOffset(offset_);
    const double length = thumbLength();
    if (orientation_ == ScrollOrientation::Vertical) {
        return core::Rect{core::Point{track.minX(), start},
                          core::Size{track.size.width, length}};
    }
    return core::Rect{core::Point{start, track.minY()},
                      core::Size{length, track.size.height}};
}

bool ScrollBar::onMouse(const PointerEvent& event) {
    if (!isUsable()) {
        // A scrollbar that cannot scroll must swallow nothing; also drop any
        // drag state so a mid-drag extents change cannot leave it stuck.
        dragging_ = false;
        pressedInside_ = false;
        return false;
    }

    const bool inside = bounds().contains(event.position);
    switch (event.type) {
    case PointerEventType::Down: {
        if (!inside) return false;
        const double pointer = axisCoordinate(event.position);
        const double thumbStart = thumbStartForOffset(offset_);
        const double thumbEnd = thumbStart + thumbLength();
        if (pointer >= thumbStart && pointer <= thumbEnd) {
            // Grab the thumb where it was touched.
            dragging_ = true;
            grabOffset_ = pointer - thumbStart;
        } else {
            // Page jump: put the thumb center under the pointer, then keep it
            // centered while the drag continues (grab = +half thumb length,
            // so a zero-pixel move after the jump is a no-op).
            offset_ = offsetForThumbStart(pointer - thumbLength() / 2.0);
            if (onScroll_) onScroll_(offset_);
            dragging_ = true;
            grabOffset_ = thumbLength() / 2.0;
        }
        pressedInside_ = true;
        break;
    }

    case PointerEventType::Move: {
        if (!dragging_) return false;
        const double moved = offsetForThumbStart(axisCoordinate(event.position) - grabOffset_);
        if (moved != offset_) {
            offset_ = moved;
            if (onScroll_) onScroll_(offset_);
        }
        break;
    }

    case PointerEventType::Up: {
        const bool wasDragging = dragging_;
        dragging_ = false;
        const bool wasPressed = pressedInside_;
        pressedInside_ = false;
        if (!wasDragging && !wasPressed) return false;
        break;
    }

    case PointerEventType::Entered:
    case PointerEventType::Exited:
    case PointerEventType::Scroll:
        return false;
    }

    event.accepted = true;
    invalidate();
    return true;
}

void ScrollBar::paintSelf(PaintContext& context) const {
    if (!isUsable()) return;
    context.fillRoundedRect(trackRect(), kTrackColor, kCornerRadius);
    context.fillRoundedRect(thumbRect(), dragging_ ? kThumbDragColor : kThumbColor, kCornerRadius);
}

} // namespace rivet::ui
