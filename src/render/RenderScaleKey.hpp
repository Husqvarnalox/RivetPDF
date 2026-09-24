#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace rivet::render {

// Quantized zoom used as rendering/cache identity. Two zoom levels that
// quantize to the same key share rendered tiles.
//
// Quantization rounds UP: the raster is never produced at a lower resolution
// than the requested zoom (it may be very slightly higher, which the painter
// then scales down by less than 1/64).
struct RenderScaleKey {
    std::uint32_t value = 1; // effective scale is value / kDenominator

    static constexpr double kDenominator = 64.0;
    static constexpr double kMinZoom = 0.10;
    static constexpr double kMaxZoom = 64.0;

    static RenderScaleKey fromZoom(double zoom) {
        double z = zoom;
        if (!std::isfinite(z)) z = 1.0;
        z = std::clamp(z, kMinZoom, kMaxZoom);
        RenderScaleKey key;
        key.value = static_cast<std::uint32_t>(std::ceil(z * kDenominator));
        if (key.value < 1) key.value = 1;
        return key;
    }

    double scale() const { return static_cast<double>(value) / kDenominator; }

    constexpr bool operator==(const RenderScaleKey&) const = default;
    constexpr auto operator<=>(const RenderScaleKey&) const = default;
};

} // namespace rivet::render

template <>
struct std::hash<rivet::render::RenderScaleKey> {
    std::size_t operator()(const rivet::render::RenderScaleKey& key) const noexcept {
        return std::hash<std::uint32_t>{}(key.value);
    }
};
