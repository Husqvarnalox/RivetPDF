#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

namespace rivet::render {

// Quantized PHYSICAL rendering density used as rendering/cache identity: device
// pixels per point, i.e. the quantized zoom (RenderScaleKey) TIMES the display
// backing scale. Two configurations that would rasterize a tile at different
// pixel dimensions must never share a cache entry: 100% zoom on a 1x and a 2x
// display produce different keys here.
//
// Quantization rounds UP, as one policy with RenderScaleKey (which contributes
// the zoom factor): the raster is never produced at a lower pixel density than
// the device would display. The PRODUCT is quantized as a whole, so densities
// landing in the same 1/64 bucket share keys (and therefore cache entries).
struct PhysicalRenderScaleKey {
    std::uint32_t value = 64; // effective density is value / kDenominator

    static constexpr double kDenominator = 64.0;
    // Sane density range: ~0.1 device px per point (tiny viewports) up to
    // 512 (extreme zoom on a high-density display). Outside the range the
    // product clamps before quantization.
    static constexpr double kMinDensity = 0.10;
    static constexpr double kMaxDensity = 512.0;

    // quantizedZoomScale: an already-quantized RenderScaleKey::scale() (the
    // zoom contribution); backingScale: the display backing scale. The product
    // is quantized UP to the next 1/64 multiple and clamped. A non-finite
    // product maps to density 1.0 before clamping, mirroring
    // RenderScaleKey::fromZoom's handling of bad input.
    static PhysicalRenderScaleKey fromDensities(double quantizedZoomScale, double backingScale) {
        double product = quantizedZoomScale * backingScale;
        if (!std::isfinite(product)) product = 1.0;
        product = std::clamp(product, kMinDensity, kMaxDensity);
        PhysicalRenderScaleKey key;
        key.value = static_cast<std::uint32_t>(std::ceil(product * kDenominator));
        if (key.value < 1) key.value = 1;
        return key;
    }

    double scale() const { return static_cast<double>(value) / kDenominator; }

    constexpr bool operator==(const PhysicalRenderScaleKey&) const = default;
    constexpr auto operator<=>(const PhysicalRenderScaleKey&) const = default;
};

} // namespace rivet::render

template <>
struct std::hash<rivet::render::PhysicalRenderScaleKey> {
    std::size_t operator()(const rivet::render::PhysicalRenderScaleKey& key) const noexcept {
        return std::hash<std::uint32_t>{}(key.value);
    }
};
