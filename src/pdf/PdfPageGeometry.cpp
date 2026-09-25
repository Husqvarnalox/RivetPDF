// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfPageGeometry.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::pdf {

bool PdfBox::isValid() const {
    return std::isfinite(left) && std::isfinite(bottom) && std::isfinite(right) &&
           std::isfinite(top) && right > left && top > bottom;
}

bool PdfBox::contains(const PdfBox& inner, double epsilon) const {
    return inner.left >= left - epsilon && inner.bottom >= bottom - epsilon &&
           inner.right <= right + epsilon && inner.top <= top + epsilon;
}

PdfBox PdfBox::intersection(const PdfBox& other) const {
    const double l = std::max(left, other.left);
    const double b = std::max(bottom, other.bottom);
    const double r = std::min(right, other.right);
    const double t = std::min(top, other.top);
    if (r <= l || t <= b) {
        return PdfBox{l, b, l, b};
    }
    return PdfBox{l, b, r, t};
}

core::Size displaySize(const PdfPageView& view) {
    const double w0 = view.cropBox.width();
    const double h0 = view.cropBox.height();
    switch (view.rotation) {
        case core::PageRotation::Clockwise90:
        case core::PageRotation::Clockwise270:
            return core::Size{h0, w0};
        case core::PageRotation::None:
        case core::PageRotation::Clockwise180:
            break;
    }
    return core::Size{w0, h0};
}

core::Point userToDisplay(const PdfPageView& view, double x, double y) {
    const double dx0 = x - view.cropBox.left;
    const double dy0 = y - view.cropBox.bottom;
    const double w0 = view.cropBox.width();
    const double h0 = view.cropBox.height();
    switch (view.rotation) {
        case core::PageRotation::Clockwise90:
            return {dy0, dx0};
        case core::PageRotation::Clockwise180:
            return {w0 - dx0, dy0};
        case core::PageRotation::Clockwise270:
            return {h0 - dy0, w0 - dx0};
        case core::PageRotation::None:
            break;
    }
    return {dx0, h0 - dy0};
}

std::pair<double, double> displayToUser(const PdfPageView& view, core::Point displayPoint) {
    const double left = view.cropBox.left;
    const double bottom = view.cropBox.bottom;
    const double w0 = view.cropBox.width();
    const double h0 = view.cropBox.height();
    const double dx = displayPoint.x;
    const double dy = displayPoint.y;
    switch (view.rotation) {
        case core::PageRotation::Clockwise90:
            return {left + dy, bottom + dx};
        case core::PageRotation::Clockwise180:
            return {left + (w0 - dx), bottom + dy};
        case core::PageRotation::Clockwise270:
            return {left + (w0 - dy), bottom + (h0 - dx)};
        case core::PageRotation::None:
            break;
    }
    return {left + dx, bottom + (h0 - dy)};
}

core::Matrix userToDisplayMatrix(const PdfPageView& view) {
    const PdfBox& box = view.cropBox;
    // Each case is the affine form of the matching userToDisplay branch.
    switch (view.rotation) {
        case core::PageRotation::Clockwise90: // (y - bottom, x - left)
            return core::Matrix{0.0, 1.0, 1.0, 0.0, -box.bottom, -box.left};
        case core::PageRotation::Clockwise180: // (right - x, y - bottom)
            return core::Matrix{-1.0, 0.0, 0.0, 1.0, box.right, -box.bottom};
        case core::PageRotation::Clockwise270: // (top - y, right - x)
            return core::Matrix{0.0, -1.0, -1.0, 0.0, box.top, box.right};
        case core::PageRotation::None:
            break;
    }
    return core::Matrix{1.0, 0.0, 0.0, -1.0, -box.left, box.top}; // (x - left, top - y)
}

core::Rect userBoxToDisplayRect(const PdfPageView& view, const PdfBox& box) {
    if (!box.isValid()) {
        return {};
    }
    const core::Size size = displaySize(view);
    const core::Point a = userToDisplay(view, box.left, box.bottom);
    const core::Point b = userToDisplay(view, box.right, box.top);
    const double minX = std::clamp(std::min(a.x, b.x), 0.0, size.width);
    const double maxX = std::clamp(std::max(a.x, b.x), 0.0, size.width);
    const double minY = std::clamp(std::min(a.y, b.y), 0.0, size.height);
    const double maxY = std::clamp(std::max(a.y, b.y), 0.0, size.height);
    return core::Rect{core::Point{minX, minY}, core::Size{maxX - minX, maxY - minY}};
}

PdfBox displayRectToUserBox(const PdfPageView& view, const core::Rect& rect) {
    const auto [ax, ay] = displayToUser(view, rect.origin);
    const auto [bx, by] = displayToUser(view, core::Point{rect.maxX(), rect.maxY()});
    return PdfBox{std::min(ax, bx), std::min(ay, by), std::max(ax, bx), std::max(ay, by)};
}

} // namespace rivet::pdf
