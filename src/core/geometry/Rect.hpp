#pragma once

#include "core/geometry/Insets.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Size.hpp"

#include <algorithm>

namespace rivet::core {

// An axis-aligned rectangle expressed as origin + size. The origin is the
// min-X/min-Y corner in the coordinate space in use. Display spaces in Rivet
// (logical, physical) are top-left origin / y-down, so there the origin is
// the top-left corner and maxY is the bottom edge.
struct Rect {
    Point origin;
    Size size;

    constexpr Rect() = default;
    constexpr Rect(Point o, Size s) : origin(o), size(s) {}
    constexpr Rect(double x, double y, double w, double h) : origin(x, y), size(w, h) {}

    constexpr double minX() const { return origin.x; }
    constexpr double minY() const { return origin.y; }
    constexpr double maxX() const { return origin.x + size.width; }
    constexpr double maxY() const { return origin.y + size.height; }

    constexpr Point center() const { return Point{minX() + size.width / 2.0, minY() + size.height / 2.0}; }
    constexpr double area() const { return size.area(); }
    constexpr bool isEmpty() const { return size.isEmpty(); }

    bool isFinite() const { return origin.isFinite() && size.isFinite(); }

    constexpr bool contains(Point p) const {
        return p.x >= minX() && p.x <= maxX() && p.y >= minY() && p.y <= maxY();
    }

    constexpr bool containsRect(const Rect& r) const {
        return r.isEmpty() || (r.minX() >= minX() && r.maxX() <= maxX() && r.minY() >= minY() && r.maxY() <= maxY());
    }

    constexpr bool intersects(const Rect& r) const {
        return minX() < r.maxX() && r.minX() < maxX() && minY() < r.maxY() && r.minY() < maxY();
    }

    // Overlap region; an empty rect (zero size at the clamped corner) when the
    // rectangles do not intersect.
    constexpr Rect intersection(const Rect& r) const {
        const double x0 = std::max(minX(), r.minX());
        const double y0 = std::max(minY(), r.minY());
        const double x1 = std::min(maxX(), r.maxX());
        const double y1 = std::min(maxY(), r.maxY());
        if (x1 <= x0 || y1 <= y0) return Rect{Point{x0, y0}, Size{}};
        return Rect{Point{x0, y0}, Size{x1 - x0, y1 - y0}};
    }

    // Smallest rectangle covering both inputs.
    constexpr Rect united(const Rect& r) const {
        if (isEmpty()) return r;
        if (r.isEmpty()) return *this;
        const double x0 = std::min(minX(), r.minX());
        const double y0 = std::min(minY(), r.minY());
        const double x1 = std::max(maxX(), r.maxX());
        const double y1 = std::max(maxY(), r.maxY());
        return Rect{Point{x0, y0}, Size{x1 - x0, y1 - y0}};
    }

    // Shrinks by the given insets on each side. Result may be empty.
    constexpr Rect inset(const Insets& i) const {
        const double w = size.width - i.horizontal();
        const double h = size.height - i.vertical();
        return Rect{Point{minX() + i.left, minY() + i.top}, Size{std::max(0.0, w), std::max(0.0, h)}};
    }

    constexpr Rect translated(Point delta) const { return Rect{origin + delta, size}; }

    constexpr Rect operator+(const Point& delta) const { return translated(delta); }

    constexpr bool operator==(const Rect&) const = default;

    static bool nearlyEqual(const Rect& a, const Rect& b, double epsilon = 1e-9) {
        return Point::nearlyEqual(a.origin, b.origin, epsilon) && Size::nearlyEqual(a.size, b.size, epsilon);
    }
};

} // namespace rivet::core
