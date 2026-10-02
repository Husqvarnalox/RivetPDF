// SPDX-License-Identifier: MPL-2.0
#include "editor/ContentGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace rivet::editor::geometry {
namespace {

double cross(core::Point a, core::Point b, core::Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

double distanceToSegment(core::Point p, core::Point a, core::Point b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0.0) return std::hypot(p.x - a.x, p.y - a.y);
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Sorts the four vertices clockwise on screen (y down = increasing atan2).
void sortClockwise(std::array<core::Point, 4>& points) {
    core::Point centroid;
    for (const core::Point& p : points) centroid += p;
    centroid = centroid / 4.0;
    std::sort(points.begin(), points.end(), [&](const core::Point& a, const core::Point& b) {
        const double angleA = std::atan2(a.y - centroid.y, a.x - centroid.x);
        const double angleB = std::atan2(b.y - centroid.y, b.x - centroid.x);
        if (angleA != angleB) return angleA < angleB;
        return std::hypot(a.x - centroid.x, a.y - centroid.y) < std::hypot(b.x - centroid.x, b.y - centroid.y);
    });
}

} // namespace

core::Matrix objectToDisplay(const pdf::PdfPageView& view, const core::Matrix& objectMatrix) {
    return pdf::userToDisplayMatrix(view) * objectMatrix;
}

std::array<core::Point, 4> quadToDisplay(const pdf::PdfPageView& view, const std::array<pdf::PdfPoint, 4>& quad) {
    std::array<core::Point, 4> mapped;
    for (std::size_t i = 0; i < 4; ++i) mapped[i] = pdf::userToDisplay(view, quad[i].x, quad[i].y);
    sortClockwise(mapped);
    // Start at the top-most (then left-most) vertex.
    std::size_t first = 0;
    for (std::size_t i = 1; i < 4; ++i) {
        if (mapped[i].y < mapped[first].y || (mapped[i].y == mapped[first].y && mapped[i].x < mapped[first].x)) {
            first = i;
        }
    }
    std::rotate(mapped.begin(), mapped.begin() + static_cast<std::ptrdiff_t>(first), mapped.end());
    return mapped;
}

core::Rect boxToDisplay(const pdf::PdfPageView& view, const pdf::PdfBox& box) {
    const core::Point a = pdf::userToDisplay(view, box.left, box.bottom);
    const core::Point b = pdf::userToDisplay(view, box.right, box.top);
    const double x0 = std::min(a.x, b.x);
    const double y0 = std::min(a.y, b.y);
    return core::Rect{core::Point{x0, y0}, core::Size{std::max(a.x, b.x) - x0, std::max(a.y, b.y) - y0}};
}

std::optional<core::Matrix> userTranslationForDisplayDelta(const pdf::PdfPageView& view, core::Point displayDelta) {
    if (!displayDelta.isFinite()) return std::nullopt;
    core::Matrix linear = pdf::userToDisplayMatrix(view);
    linear.tx = 0.0;
    linear.ty = 0.0;
    const auto inverse = linear.inverted();
    if (!inverse.has_value()) return std::nullopt;
    const core::Point user = inverse->map(displayDelta);
    return core::Matrix::translation(user.x, user.y);
}

std::optional<core::Matrix> userTransformForDisplayRects(const pdf::PdfPageView& view, const core::Rect& from,
                                                         const core::Rect& to) {
    if (!from.isFinite() || !to.isFinite()) return std::nullopt;
    if (from.size.width <= 0.0 || from.size.height <= 0.0 || to.size.width <= 0.0 || to.size.height <= 0.0) {
        return std::nullopt;
    }
    const double sx = to.size.width / from.size.width;
    const double sy = to.size.height / from.size.height;
    // S: x' = sx * (x - from.x) + to.x, y' = sy * (y - from.y) + to.y
    const core::Matrix scale{sx, 0.0, 0.0, sy, to.minX() - sx * from.minX(), to.minY() - sy * from.minY()};
    const core::Matrix v = pdf::userToDisplayMatrix(view);
    const auto inverse = v.inverted();
    if (!inverse.has_value()) return std::nullopt;
    return *inverse * scale * v;
}

double displayAngleDegrees(const pdf::PdfPageView& view, const core::Matrix& userMatrix) {
    core::Matrix linear = pdf::userToDisplayMatrix(view);
    linear.tx = 0.0;
    linear.ty = 0.0;
    const core::Point direction = linear.map(core::Point{userMatrix.a, userMatrix.b});
    // y is down on screen: atan2 grows clockwise.
    double degrees = std::atan2(direction.y, direction.x) * 180.0 / std::numbers::pi;
    if (degrees <= -180.0) degrees += 360.0;
    // Snap float noise (cos/sin of exact quarter turns) to clean values.
    const double rounded = std::round(degrees * 1e6) / 1e6;
    return rounded == 0.0 ? 0.0 : rounded;
}

double userAngleRadians(const core::Matrix& userMatrix) { return std::atan2(userMatrix.b, userMatrix.a); }

core::Matrix uprightPlacement(const pdf::PdfPageView& view, core::Point displayOrigin) {
    // Block frame (x right, y up) -> display (x right, y down) at the origin.
    const core::Matrix frameToDisplay{1.0, 0.0, 0.0, -1.0, displayOrigin.x, displayOrigin.y};
    const auto inverse = pdf::userToDisplayMatrix(view).inverted();
    // V is a reflection composed with quarter turns, always invertible.
    return inverse.has_value() ? (*inverse * frameToDisplay) : core::Matrix::identity();
}

bool isRigid(const core::Matrix& m, double epsilon) {
    if (!std::isfinite(m.a) || !std::isfinite(m.b) || !std::isfinite(m.c) || !std::isfinite(m.d) ||
        !std::isfinite(m.tx) || !std::isfinite(m.ty)) {
        return false;
    }
    return std::fabs(m.a - m.d) <= epsilon && std::fabs(m.b + m.c) <= epsilon &&
           std::fabs(m.a * m.a + m.b * m.b - 1.0) <= epsilon;
}

bool isSimilarity(const core::Matrix& m, double epsilon) {
    if (!std::isfinite(m.a) || !std::isfinite(m.b) || !std::isfinite(m.c) || !std::isfinite(m.d) ||
        !std::isfinite(m.tx) || !std::isfinite(m.ty)) {
        return false;
    }
    const double scale = std::hypot(m.a, m.b);
    if (scale <= 0.0) return false;
    const double tolerance = epsilon * scale;
    return std::fabs(m.a - m.d) <= tolerance && std::fabs(m.b + m.c) <= tolerance;
}

bool pointInConvexQuad(core::Point p, const std::array<core::Point, 4>& quad, double tolerance) {
    std::array<core::Point, 4> poly = quad;
    sortClockwise(poly);
    bool allNonNegative = true;
    bool allNonPositive = true;
    for (std::size_t i = 0; i < 4; ++i) {
        const double side = cross(poly[i], poly[(i + 1) % 4], p);
        if (side < 0.0) allNonNegative = false;
        if (side > 0.0) allNonPositive = false;
    }
    if (allNonNegative || allNonPositive) return true;
    for (std::size_t i = 0; i < 4; ++i) {
        if (distanceToSegment(p, poly[i], poly[(i + 1) % 4]) <= tolerance) return true;
    }
    return false;
}

} // namespace rivet::editor::geometry
