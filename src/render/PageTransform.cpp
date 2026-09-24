#include "render/PageTransform.hpp"

#include <algorithm>

namespace rivet::render {

namespace {

// Display-box deltas (dx, dy) in points for an unrotated page point (x, y).
// The mapping per rotation is documented in PageTransform.hpp. Rotations
// follow the PDF /Rotate encoding: Clockwise90 turns the page content 90
// degrees clockwise for display (page top edge lands on the display right).
core::Point displayDelta(core::PageRotation rotation, double x, double y, double w0, double h0) {
    switch (rotation) {
        case core::PageRotation::None: return {x, h0 - y};
        case core::PageRotation::Clockwise90: return {y, x};
        case core::PageRotation::Clockwise180: return {w0 - x, y};
        case core::PageRotation::Clockwise270: return {h0 - y, w0 - x};
    }
    return {x, h0 - y}; // unreachable for valid rotations
}

// Inverse of displayDelta.
core::Point displayDeltaInverse(core::PageRotation rotation, double dx, double dy, double w0, double h0) {
    switch (rotation) {
        case core::PageRotation::None: return {dx, h0 - dy};
        case core::PageRotation::Clockwise90: return {dy, dx};
        case core::PageRotation::Clockwise180: return {w0 - dx, dy};
        case core::PageRotation::Clockwise270: return {w0 - dy, h0 - dx};
    }
    return {dx, h0 - dy};
}

} // namespace

PageTransform PageTransform::make(const core::Rect& pageFrameLogical, const core::Size& pageSizePoints,
                                  core::PageRotation rotation, double zoom, double backingScale) {
    return PageTransform{pageFrameLogical, pageSizePoints, rotation, zoom, backingScale};
}

PageTransform::PageTransform(core::Rect pageFrameLogical, core::Size pageSizePoints,
                             core::PageRotation rotation, double zoom, double backingScale)
    : frame_(pageFrameLogical),
      pageSizePoints_(pageSizePoints),
      rotation_(rotation),
      zoom_(zoom),
      backingScale_(backingScale) {}

core::Point PageTransform::pageToLogical(const core::Point& pagePoint) const {
    const core::Point delta =
        displayDelta(rotation_, pagePoint.x, pagePoint.y, pageSizePoints_.width, pageSizePoints_.height);
    return frame_.origin + zoom_ * delta;
}

core::Point PageTransform::logicalToPage(const core::Point& logicalPoint) const {
    const double dx = (logicalPoint.x - frame_.origin.x) / zoom_;
    const double dy = (logicalPoint.y - frame_.origin.y) / zoom_;
    return displayDeltaInverse(rotation_, dx, dy, pageSizePoints_.width, pageSizePoints_.height);
}

core::Rect PageTransform::pageToLogical(const core::Rect& pageRect) const {
    return boundingRectOf(pageToLogical(core::Point{pageRect.minX(), pageRect.minY()}),
                          pageToLogical(core::Point{pageRect.maxX(), pageRect.minY()}),
                          pageToLogical(core::Point{pageRect.minX(), pageRect.maxY()}),
                          pageToLogical(core::Point{pageRect.maxX(), pageRect.maxY()}));
}

core::Rect PageTransform::logicalToPage(const core::Rect& logicalRect) const {
    return boundingRectOf(logicalToPage(core::Point{logicalRect.minX(), logicalRect.minY()}),
                          logicalToPage(core::Point{logicalRect.maxX(), logicalRect.minY()}),
                          logicalToPage(core::Point{logicalRect.minX(), logicalRect.maxY()}),
                          logicalToPage(core::Point{logicalRect.maxX(), logicalRect.maxY()}));
}

core::Point PageTransform::logicalToPhysical(const core::Point& logicalPoint) const {
    return logicalPoint * backingScale_;
}

core::Point PageTransform::physicalToLogical(const core::Point& physicalPoint) const {
    return physicalPoint / backingScale_;
}

core::Rect PageTransform::logicalToPhysical(const core::Rect& logicalRect) const {
    return core::Rect{logicalRect.origin * backingScale_, logicalRect.size * backingScale_};
}

core::Matrix PageTransform::pageToLogicalMatrix() const {
    const double z = zoom_;
    const double fx = frame_.origin.x;
    const double fy = frame_.origin.y;
    const double w0 = pageSizePoints_.width;
    const double h0 = pageSizePoints_.height;
    switch (rotation_) {
        case core::PageRotation::None:
            return core::Matrix{z, 0.0, 0.0, -z, fx, fy + z * h0};
        case core::PageRotation::Clockwise90:
            return core::Matrix{0.0, z, z, 0.0, fx, fy};
        case core::PageRotation::Clockwise180:
            return core::Matrix{-z, 0.0, 0.0, z, fx + z * w0, fy};
        case core::PageRotation::Clockwise270:
            return core::Matrix{0.0, -z, -z, 0.0, fx + z * h0, fy + z * w0};
    }
    return core::Matrix::identity();
}

core::Rect PageTransform::boundingRectOf(const core::Point& p0, const core::Point& p1, const core::Point& p2,
                                         const core::Point& p3) {
    const double minX = std::min({p0.x, p1.x, p2.x, p3.x});
    const double minY = std::min({p0.y, p1.y, p2.y, p3.y});
    const double maxX = std::max({p0.x, p1.x, p2.x, p3.x});
    const double maxY = std::max({p0.y, p1.y, p2.y, p3.y});
    return core::Rect{core::Point{minX, minY}, core::Size{maxX - minX, maxY - minY}};
}

} // namespace rivet::render
