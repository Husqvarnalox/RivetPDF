#pragma once

#include <cmath>

namespace rivet::core {

// A 2D size. Width or height <= 0 means "empty".
struct Size {
    double width = 0.0;
    double height = 0.0;

    constexpr Size() = default;
    constexpr Size(double w, double h) : width(w), height(h) {}

    constexpr double area() const { return width * height; }
    constexpr bool isEmpty() const { return width <= 0.0 || height <= 0.0; }

    bool isFinite() const { return std::isfinite(width) && std::isfinite(height); }

    constexpr Size operator+(const Size& o) const { return Size{width + o.width, height + o.height}; }
    constexpr Size operator-(const Size& o) const { return Size{width - o.width, height - o.height}; }
    constexpr Size operator*(double s) const { return Size{width * s, height * s}; }
    constexpr Size operator/(double s) const { return Size{width / s, height / s}; }

    constexpr bool operator==(const Size&) const = default;

    static bool nearlyEqual(const Size& a, const Size& b, double epsilon = 1e-9) {
        return std::fabs(a.width - b.width) <= epsilon && std::fabs(a.height - b.height) <= epsilon;
    }
};

constexpr Size operator*(double s, const Size& sz) { return sz * s; }

} // namespace rivet::core
