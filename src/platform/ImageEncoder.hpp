// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace rivet::platform {

// Encodes raw 8-bit pixels as a baseline JPEG file (Reduce file size,
// ADR-0028). Implemented per platform (ImageIO on macOS, libjpeg on Linux,
// WIC on Windows); shared code never touches the native codec.
//
// Contract:
//   - `pixels`: tightly packed interleaved rows (stride = width * channels),
//     `channels` 1 (gray) or 3 (RGB, R first); `width`/`height` non-zero and
//     at most pdf::kMaxImageSide; `quality` 1..100 (clamped). Anything else
//     is ErrorCode::InvalidArgument (no pixel data in the message).
//   - The result is a complete JPEG file (SOI..EOI) with the same pixel
//     size, 8 bits per component, no EXIF orientation, no alpha.
//   - Codec failures are ErrorCode::Internal / Unsupported. No exceptions;
//     no logging of image content.
//   - Thread-safe and re-entrant: called on worker threads (the assembly
//     worker) while the PDFium call gate is held; it must not call back into
//     the PDF layer.
class IImageEncoder {
public:
    virtual ~IImageEncoder() = default;

    virtual core::Result<std::vector<std::uint8_t>> encodeJpeg(std::span<const std::uint8_t> pixels,
                                                               std::uint32_t width, std::uint32_t height,
                                                               std::uint32_t channels, int quality) const = 0;
};

} // namespace rivet::platform
