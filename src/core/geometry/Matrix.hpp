#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace rivet::core {

// A 2x3 affine matrix:
//
//   x' = a*x + c*y + tx
//   y' = b*x + d*y + ty
//
// Composition convention: (A * B).map(p) == A.map(B.map(p)), i.e. B is
// applied to the point first, then A.
struct Matrix {
    double a = 1.0;
    double b = 0.0;
    double c = 0.0;
    double d = 1.0;
    double tx = 0.0;
    double ty = 0.0;

    static constexpr Matrix identity() { return Matrix{}; }

    static constexpr Matrix translation(double dx, double dy) {
        return Matrix{1.0, 0.0, 0.0, 1.0, dx, dy};
    }

    static constexpr Matrix scaling(double sx, double sy) {
        return Matrix{sx, 0.0, 0.0, sy, 0.0, 0.0};
    }

    static Matrix rotation(double radians) {
        const double s = std::sin(radians);
        const double c = std::cos(radians);
        return Matrix{c, s, -s, c, 0.0, 0.0};
    }

    constexpr Point map(Point p) const {
        return Point{a * p.x + c * p.y + tx, b * p.x + d * p.y + ty};
    }

    // Bounding box of the four transformed corners.
    Rect mapRect(const Rect& r) const {
        const Point p0 = map(r.origin);
        const Point p1 = map(Point{r.maxX(), r.minY()});
        const Point p2 = map(Point{r.minX(), r.maxY()});
        const Point p3 = map(Point{r.maxX(), r.maxY()});
        const double x0 = std::min(std::min(p0.x, p1.x), std::min(p2.x, p3.x));
        const double y0 = std::min(std::min(p0.y, p1.y), std::min(p2.y, p3.y));
        const double x1 = std::max(std::max(p0.x, p1.x), std::max(p2.x, p3.x));
        const double y1 = std::max(std::max(p0.y, p1.y), std::max(p2.y, p3.y));
        return Rect{Point{x0, y0}, Size{x1 - x0, y1 - y0}};
    }

    constexpr Matrix operator*(const Matrix& o) const {
        return Matrix{
            a * o.a + c * o.b,
            b * o.a + d * o.b,
            a * o.c + c * o.d,
            b * o.c + d * o.d,
            a * o.tx + c * o.ty + tx,
            b * o.tx + d * o.ty + ty};
    }

    // Returns std::nullopt for singular (non-invertible) matrices.
    std::optional<Matrix> inverted() const {
        const double det = a * d - b * c;
        if (det == 0.0 || !std::isfinite(det)) return std::nullopt;
        const double invDet = 1.0 / det;
        Matrix result;
        result.a = d * invDet;
        result.b = -b * invDet;
        result.c = -c * invDet;
        result.d = a * invDet;
        result.tx = (c * ty - d * tx) * invDet;
        result.ty = (b * tx - a * ty) * invDet;
        return result;
    }

    constexpr bool operator==(const Matrix&) const = default;
};

} // namespace rivet::core
