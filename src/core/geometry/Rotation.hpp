#pragma once

#include <cstdint>

namespace rivet::core {

// Page rotation in 90-degree steps, clockwise for display. The values match
// the PDF /Rotate integer encoding (0, 1, 2, 3 quarters).
enum class PageRotation : std::uint8_t {
    None = 0,
    Clockwise90 = 1,
    Clockwise180 = 2,
    Clockwise270 = 3,
};

constexpr int rotationDegrees(PageRotation r) {
    return static_cast<int>(r) * 90;
}

// Composition of two rotations, wrapping modulo a full turn.
constexpr PageRotation addRotation(PageRotation a, PageRotation b) {
    return static_cast<PageRotation>((static_cast<int>(a) + static_cast<int>(b)) % 4);
}

// Clamps an arbitrary integer (e.g. from document metadata) into a valid
// rotation, mapping anything else to None.
constexpr PageRotation rotationFromQuarterTurns(int quarterTurns) {
    const int m = quarterTurns % 4;
    const int normalized = (m < 0) ? m + 4 : m;
    return static_cast<PageRotation>(static_cast<std::uint8_t>(normalized));
}

} // namespace rivet::core
