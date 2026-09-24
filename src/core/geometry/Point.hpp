#pragma once

#include <cmath>

namespace rivet::core {

// A 2D point in whatever coordinate space the caller is working in.
// Rivet distinguishes (see docs/ARCHITECTURE.md):
//   - PDF/page coordinates  (points, origin bottom-left, y-up)
//   - logical coordinates   (points, origin top-left, y-down)
//   - physical coordinates  (device pixels, origin top-left, y-down)
// Point itself is convention-neutral; PageTransform owns the conversions.
struct Point {
    double x = 0.0;
    double y = 0.0;

    constexpr Point() = default;
    constexpr Point(double px, double py) : x(px), y(py) {}

    constexpr Point operator+(const Point& o) const { return Point{x + o.x, y + o.y}; }
    constexpr Point operator-(const Point& o) const { return Point{x - o.x, y - o.y}; }
    constexpr Point operator-() const { return Point{-x, -y}; }
    constexpr Point operator*(double s) const { return Point{x * s, y * s}; }
    constexpr Point operator/(double s) const { return Point{x / s, y / s}; }

    constexpr Point& operator+=(const Point& o) { x += o.x; y += o.y; return *this; }
    constexpr Point& operator-=(const Point& o) { x -= o.x; y -= o.y; return *this; }
    constexpr Point& operator*=(double s) { x *= s; y *= s; return *this; }

    constexpr bool operator==(const Point&) const = default;

    bool isFinite() const { return std::isfinite(x) && std::isfinite(y); }

    static bool nearlyEqual(const Point& a, const Point& b, double epsilon = 1e-9) {
        return std::fabs(a.x - b.x) <= epsilon && std::fabs(a.y - b.y) <= epsilon;
    }
};

constexpr Point operator*(double s, const Point& p) { return p * s; }

} // namespace rivet::core
