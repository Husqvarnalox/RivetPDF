// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Widget.hpp"

#include <cstdint>
#include <functional>

namespace rivet::ui {

enum class ScrollOrientation : std::uint8_t { Vertical, Horizontal };

// Lightweight reusable scrollbar: proportional thumb, draggable thumb, track
// click. The owner (e.g. PdfViewport) supplies extents and reacts to
// onScroll; the scrollbar owns no document state.
//
// Geometry model: the owner sets (viewportExtent, contentExtent) in the same
// units as `offset` (logical points of scrolled content). The thumb is
// proportional: thumbLength = trackLength * viewport/content with a minimum
// of kMinThumbLength, and offset/maxOffset positions it along the track.
// When content <= viewport the scrollbar is not usable: paint paints nothing
// and mouse events are not consumed.
//
// All coordinates are widget-local. For Vertical the scroll axis is y; for
// Horizontal it is x.
class ScrollBar final : public Widget {
public:
    static constexpr double kThickness = 12.0;   // cross-axis size hint
    static constexpr double kTrackPadding = 2.0; // track inset on both scroll-axis ends
    static constexpr double kMinThumbLength = 24.0;

    explicit ScrollBar(ScrollOrientation orientation);

    ScrollOrientation orientation() const { return orientation_; }

    // Both extents must be finite and >= 0; calls carrying any other value
    // are ignored (state unchanged). The offset is re-clamped to the new
    // range (no onScroll fired — the owner drives the change).
    void setExtents(double viewportExtent, double contentExtent);
    double viewportExtent() const { return viewportExtent_; }
    double contentExtent() const { return contentExtent_; }

    // Sets the scroll offset, clamped to [0, maxOffset()]. Non-finite values
    // are ignored (the current offset is kept) so a NaN/Inf from a caller
    // cannot corrupt the thumb geometry.
    void setOffset(double offset);
    double offset() const { return offset_; }
    double maxOffset() const; // max(0, contentExtent - viewportExtent)

    bool isUsable() const; // contentExtent > viewportExtent and both finite

    // Fired with the new already-clamped offset when the user drags the thumb
    // or clicks the track. NOT fired for setOffset()/setExtents().
    void setOnScroll(std::function<void(double)> onScroll);

    core::Size preferredSize(const PaintContext&) const override;

    bool onMouse(const PointerEvent& event) override;
    void paintSelf(PaintContext& context) const override;

    // Thumb rect in local coordinates along the scroll axis (test/diagnostic).
    core::Rect thumbRect() const;
    // Track rect in local coordinates (test/diagnostic).
    core::Rect trackRect() const;

private:
    // Track length along the scroll axis (bounds minus both paddings).
    double trackLength() const;
    // Proportional thumb length, at least kMinThumbLength, never longer than
    // the track; 0 when not usable.
    double thumbLength() const;
    // The thumb start position on the scroll axis for a given offset.
    double thumbStartForOffset(double offset) const;
    // The scroll offset for a given absolute thumb start position (clamped).
    double offsetForThumbStart(double thumbStart) const;
    // Track origin along the scroll axis.
    double trackAxisOrigin() const;
    // The event position projected onto the scroll axis.
    double axisCoordinate(const core::Point& position) const;

    ScrollOrientation orientation_;
    double viewportExtent_ = 0.0;
    double contentExtent_ = 0.0;
    double offset_ = 0.0;
    bool dragging_ = false;      // thumb drag in progress
    double grabOffset_ = 0.0;    // axisCoordinate - thumbStart while dragging
    bool pressedInside_ = false; // the active press started in this scrollbar
    std::function<void(double)> onScroll_;
};

} // namespace rivet::ui
