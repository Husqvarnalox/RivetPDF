#pragma once

#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"

namespace rivet::render {

// Maps between the coordinate spaces Rivet uses (see core/geometry/Point.hpp):
//   page     - PDF user space, points, origin bottom-left, y-up, unrotated
//   logical  - viewport space, points, origin top-left, y-down, current zoom
//   physical - device pixels, origin top-left, y-down
//
// The page is shown inside pageFrameLogical (its box in logical coordinates)
// after applying the display rotation. With the unrotated page size (w0, h0)
// and the rotated display box size (W, H), an unrotated page point (x, y)
// maps to display deltas (dx, dy) in points. Rotations match the PDF /Rotate
// encoding (clockwise for display):
//
//   None:         W = w0, H = h0;  dx = x,      dy = h0 - y
//   Clockwise90:  W = h0, H = w0;  dx = y,      dy = x
//   Clockwise180: W = w0, H = h0;  dx = w0 - x, dy = y
//   Clockwise270: W = h0, H = w0;  dx = h0 - y, dy = w0 - x
//
//   logical = pageFrameLogical.origin + zoom * (dx, dy)
//
// logicalToPage / physicalToLogical are the exact inverses. zoom must be > 0
// (it comes from ZoomState, which clamps to a positive range) and
// backingScale > 0.
class PageTransform {
public:
    static PageTransform make(const core::Rect& pageFrameLogical, const core::Size& pageSizePoints,
                              core::PageRotation rotation, double zoom, double backingScale);

    // PDF page coords (origin bottom-left, y-up) -> logical.
    core::Point pageToLogical(const core::Point& pagePoint) const;
    core::Point logicalToPage(const core::Point& logicalPoint) const;

    // Rect corners are mapped and bounded; a 90-degree rotation maps an
    // axis-aligned rect to an axis-aligned rect, so this is exact.
    core::Rect pageToLogical(const core::Rect& pageRect) const;
    core::Rect logicalToPage(const core::Rect& logicalRect) const;

    core::Point logicalToPhysical(const core::Point& logicalPoint) const;
    core::Point physicalToLogical(const core::Point& physicalPoint) const;
    core::Rect logicalToPhysical(const core::Rect& logicalRect) const;

    // Matrix M with M.map(p) == pageToLogical(p) for every page point p.
    core::Matrix pageToLogicalMatrix() const;

    double zoom() const { return zoom_; }
    double backingScale() const { return backingScale_; }

    double devicePixelsPerPoint() const { return zoom_ * backingScale_; }

private:
    PageTransform(core::Rect pageFrameLogical, core::Size pageSizePoints, core::PageRotation rotation,
                  double zoom, double backingScale);

    static core::Rect boundingRectOf(const core::Point& p0, const core::Point& p1, const core::Point& p2,
                                     const core::Point& p3);

    core::Rect frame_;
    core::Size pageSizePoints_;
    core::PageRotation rotation_ = core::PageRotation::None;
    double zoom_ = 1.0;
    double backingScale_ = 1.0;
};

} // namespace rivet::render
