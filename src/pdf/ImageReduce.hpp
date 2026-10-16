// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Reduce file size (ADR-0028): the portable parts of "Save Reduced Copy" -
// presets, the resolution decision and the box-filter downsampler. The
// backend (src/pdf/pdfium) walks the pages, decodes the images, calls these
// helpers and re-embeds the result; JPEG encoding is injected through
// IJpegEncoder because this layer must not depend on the platform layer.

namespace rivet::pdf {

// Encodes 8-bit pixels as a complete baseline JPEG file. Implemented by the
// application over the platform codec (platform::IImageEncoder).
//
// Contract: `pixels` are tightly packed interleaved rows (stride =
// width * channels); channels is 1 (gray) or 3 (RGB); quality is 1..100.
// Thread-safe and re-entrant. Called from the assembly worker while it holds
// the PDFium call gate: an implementation must never call back into the PDF
// layer.
class IJpegEncoder {
public:
    virtual ~IJpegEncoder() = default;
    virtual core::Result<std::vector<std::uint8_t>> encode(std::span<const std::uint8_t> pixels,
                                                           std::uint32_t width, std::uint32_t height,
                                                           std::uint32_t channels, int quality) const = 0;
};

enum class ReduceQuality : std::uint8_t { High, Medium, Small };

struct ReducePreset {
    int dpi = 0;         // images above this effective resolution are downsampled
    int jpegQuality = 0; // 1..100
};

constexpr ReducePreset reducePreset(ReduceQuality quality) {
    switch (quality) {
    case ReduceQuality::High: return {200, 85};
    case ReduceQuality::Medium: return {150, 75};
    case ReduceQuality::Small: return {100, 60};
    }
    return {150, 75};
}

// Images with a side below this are left untouched.
inline constexpr std::uint32_t kMinReducibleImageSide = 64;

// What to do with one image.
struct ReducePlan {
    std::uint32_t width = 0;  // target pixel size (<= the source size)
    std::uint32_t height = 0;
    bool resample = false;    // the target is smaller than the source
};

// Decides the target size of an image of `pixelWidth` x `pixelHeight` pixels
// placed `placedWidth` x `placedHeight` points large (page user space):
// each axis is limited to placed inches * `targetDpi`, never enlarged.
// nullopt = leave the image alone (a side below kMinReducibleImageSide, a
// degenerate placement or a non-positive dpi).
std::optional<ReducePlan> planReduction(std::uint32_t pixelWidth, std::uint32_t pixelHeight, double placedWidth,
                                        double placedHeight, int targetDpi);

// Area-averaging (box filter) downsample of 8-bit interleaved pixels with
// `channels` (1..4) channels and `stride` bytes per source row to
// `dstWidth` x `dstHeight` (1 <= dst <= src per axis), tightly packed.
// Source pixels contribute with their fractional coverage, so the result of
// a constant image is constant and the mean brightness is preserved.
// Returns an empty vector for inconsistent arguments (never reads out of
// bounds).
std::vector<std::uint8_t> boxDownsample(std::span<const std::uint8_t> source, std::uint32_t width,
                                        std::uint32_t height, std::uint32_t channels, std::size_t stride,
                                        std::uint32_t dstWidth, std::uint32_t dstHeight);

// Options of one reducing assembly (PdfAssemblyRequest::reduceImages).
struct PdfImageReduction {
    int targetDpi = 150;
    int jpegQuality = 75;
    // Required; must outlive the assembly.
    const IJpegEncoder* encoder = nullptr;
    // Optional out-parameter: receives the number of images re-encoded.
    std::size_t* imagesReduced = nullptr;
};

} // namespace rivet::pdf
