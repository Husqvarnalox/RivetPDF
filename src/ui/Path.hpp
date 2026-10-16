// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace rivet::ui {

struct PathSegment {
    enum class Op : std::uint8_t { MoveTo, LineTo, CubicTo, Close };

    Op op = Op::MoveTo;
    core::Point p;  // MoveTo/LineTo: the point; CubicTo: the end point
    core::Point c1; // CubicTo only: first control point
    core::Point c2; // CubicTo only: second control point
};

// A vector path in LOGICAL coordinates (see the PaintContext coordinate
// contract). Plain data: building one never touches a platform API.
struct Path {
    std::vector<PathSegment> segments;

    bool empty() const { return segments.empty(); }

    void moveTo(core::Point p) {
        segments.push_back(PathSegment{PathSegment::Op::MoveTo, p, {}, {}});
    }
    void lineTo(core::Point p) {
        segments.push_back(PathSegment{PathSegment::Op::LineTo, p, {}, {}});
    }
    void cubicTo(core::Point c1, core::Point c2, core::Point end) {
        segments.push_back(PathSegment{PathSegment::Op::CubicTo, end, c1, c2});
    }
    void close() { segments.push_back(PathSegment{PathSegment::Op::Close, {}, {}, {}}); }

    // Closed rectangle subpath (clockwise in the y-down logical space).
    void addRect(const core::Rect& rect) {
        moveTo({rect.minX(), rect.minY()});
        lineTo({rect.maxX(), rect.minY()});
        lineTo({rect.maxX(), rect.maxY()});
        lineTo({rect.minX(), rect.maxY()});
        close();
    }

    // Closed rounded-rectangle subpath (corner arcs as cubic Beziers); the
    // radius is clamped to half the shorter side.
    void addRoundedRect(const core::Rect& rect, double radius) {
        constexpr double kKappa = 0.5522847498307936;
        const double r = std::max(0.0, std::min(radius, std::min(rect.size.width, rect.size.height) / 2.0));
        const double k = r * (1.0 - kKappa);
        const double x0 = rect.minX();
        const double y0 = rect.minY();
        const double x1 = rect.maxX();
        const double y1 = rect.maxY();
        moveTo({x0 + r, y0});
        lineTo({x1 - r, y0});
        cubicTo({x1 - k, y0}, {x1, y0 + k}, {x1, y0 + r});
        lineTo({x1, y1 - r});
        cubicTo({x1, y1 - k}, {x1 - k, y1}, {x1 - r, y1});
        lineTo({x0 + r, y1});
        cubicTo({x0 + k, y1}, {x0, y1 - k}, {x0, y1 - r});
        lineTo({x0, y0 + r});
        cubicTo({x0, y0 + k}, {x0 + k, y0}, {x0 + r, y0});
        close();
    }

    // Closed ellipse inscribed in `rect`, four cubic Bezier arcs (the usual
    // kappa approximation), starting at the right-most point.
    void addEllipse(const core::Rect& rect) {
        constexpr double kKappa = 0.5522847498307936;
        const core::Point center = rect.center();
        const double rx = rect.size.width / 2.0;
        const double ry = rect.size.height / 2.0;
        const double kx = rx * kKappa;
        const double ky = ry * kKappa;
        moveTo({center.x + rx, center.y});
        cubicTo({center.x + rx, center.y + ky}, {center.x + kx, center.y + ry},
                {center.x, center.y + ry});
        cubicTo({center.x - kx, center.y + ry}, {center.x - rx, center.y + ky},
                {center.x - rx, center.y});
        cubicTo({center.x - rx, center.y - ky}, {center.x - kx, center.y - ry},
                {center.x, center.y - ry});
        cubicTo({center.x + kx, center.y - ry}, {center.x + rx, center.y - ky},
                {center.x + rx, center.y});
        close();
    }
};

enum class LineCap : std::uint8_t { Butt, Round };
enum class LineJoin : std::uint8_t { Miter, Round };

} // namespace rivet::ui
