#pragma once

namespace rivet::core {

// Distances between a rectangle edge and the surrounding box, in the same
// coordinate orientation as Rect (top = smaller y in display spaces).
struct Insets {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;

    constexpr Insets() = default;
    constexpr Insets(double l, double t, double r, double b) : left(l), top(t), right(r), bottom(b) {}

    static constexpr Insets uniform(double v) { return Insets{v, v, v, v}; }
    static constexpr Insets horizontal(double v) { return Insets{v, 0.0, v, 0.0}; }
    static constexpr Insets vertical(double v) { return Insets{0.0, v, 0.0, v}; }

    constexpr double horizontal() const { return left + right; }
    constexpr double vertical() const { return top + bottom; }

    constexpr bool operator==(const Insets&) const = default;
};

} // namespace rivet::core
