// SPDX-License-Identifier: MPL-2.0
#include "pdf/ImageReduce.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::pdf {

std::optional<ReducePlan> planReduction(std::uint32_t pixelWidth, std::uint32_t pixelHeight, double placedWidth,
                                        double placedHeight, int targetDpi) {
    if (pixelWidth < kMinReducibleImageSide || pixelHeight < kMinReducibleImageSide || targetDpi <= 0 ||
        !(placedWidth > 0.0) || !(placedHeight > 0.0) || !std::isfinite(placedWidth) ||
        !std::isfinite(placedHeight)) {
        return std::nullopt;
    }
    const auto limit = [targetDpi](std::uint32_t pixels, double placedPoints) -> std::uint32_t {
        const double wanted = std::ceil(placedPoints / 72.0 * static_cast<double>(targetDpi));
        if (!(wanted < static_cast<double>(pixels))) return pixels;
        return static_cast<std::uint32_t>(std::max(1.0, wanted));
    };
    ReducePlan plan;
    plan.width = limit(pixelWidth, placedWidth);
    plan.height = limit(pixelHeight, placedHeight);
    plan.resample = plan.width < pixelWidth || plan.height < pixelHeight;
    return plan;
}

std::vector<std::uint8_t> boxDownsample(std::span<const std::uint8_t> source, std::uint32_t width,
                                        std::uint32_t height, std::uint32_t channels, std::size_t stride,
                                        std::uint32_t dstWidth, std::uint32_t dstHeight) {
    if (width == 0 || height == 0 || channels == 0 || channels > 4 || dstWidth == 0 || dstHeight == 0 ||
        dstWidth > width || dstHeight > height) {
        return {};
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * channels;
    if (stride < rowBytes || source.size() < stride * (height - 1) + rowBytes) return {};

    std::vector<std::uint8_t> out(static_cast<std::size_t>(dstWidth) * dstHeight * channels);
    const double scaleX = static_cast<double>(width) / static_cast<double>(dstWidth);
    const double scaleY = static_cast<double>(height) / static_cast<double>(dstHeight);
    double sums[4];
    for (std::uint32_t dy = 0; dy < dstHeight; ++dy) {
        const double y0 = dy * scaleY;
        const double y1 = std::min(static_cast<double>(height), (dy + 1) * scaleY);
        const auto firstRow = static_cast<std::uint32_t>(y0);
        const auto lastRow = std::min(height, static_cast<std::uint32_t>(std::ceil(y1)));
        for (std::uint32_t dx = 0; dx < dstWidth; ++dx) {
            const double x0 = dx * scaleX;
            const double x1 = std::min(static_cast<double>(width), (dx + 1) * scaleX);
            const auto firstCol = static_cast<std::uint32_t>(x0);
            const auto lastCol = std::min(width, static_cast<std::uint32_t>(std::ceil(x1)));
            std::fill(sums, sums + 4, 0.0);
            double weightSum = 0.0;
            for (std::uint32_t sy = firstRow; sy < lastRow; ++sy) {
                const double wy = std::min(y1, sy + 1.0) - std::max(y0, static_cast<double>(sy));
                if (!(wy > 0.0)) continue;
                const std::uint8_t* row = source.data() + static_cast<std::size_t>(sy) * stride;
                for (std::uint32_t sx = firstCol; sx < lastCol; ++sx) {
                    const double wx = std::min(x1, sx + 1.0) - std::max(x0, static_cast<double>(sx));
                    if (!(wx > 0.0)) continue;
                    const double weight = wx * wy;
                    const std::uint8_t* pixel = row + static_cast<std::size_t>(sx) * channels;
                    for (std::uint32_t c = 0; c < channels; ++c) sums[c] += weight * pixel[c];
                    weightSum += weight;
                }
            }
            std::uint8_t* target = out.data() + (static_cast<std::size_t>(dy) * dstWidth + dx) * channels;
            for (std::uint32_t c = 0; c < channels; ++c) {
                const double value = weightSum > 0.0 ? sums[c] / weightSum : 0.0;
                target[c] = static_cast<std::uint8_t>(std::clamp(std::lround(value), 0L, 255L));
            }
        }
    }
    return out;
}

} // namespace rivet::pdf
