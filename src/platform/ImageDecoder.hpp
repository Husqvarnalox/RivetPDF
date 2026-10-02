// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "pdf/PdfContent.hpp"

#include <cstdint>
#include <span>

namespace rivet::platform {

// Decodes an image file (Replace Image, ADR-0014/0015) into the portable
// pixel description the content editor embeds. Implemented per platform
// (ImageIO on macOS); shared code never touches the native decoder.
//
// Contract:
//   - The limits of pdf/PdfContent.hpp are enforced BEFORE any pixel buffer
//     is allocated: `encoded.size() <= kMaxImageEncodedBytes`, each side
//     `<= kMaxImageSide`, `width * height <= kMaxImagePixels` (computed
//     overflow-safe), both sides non-zero. A violation is
//     ErrorCode::InvalidArgument (message says which limit; no file data).
//   - A valid, plainly oriented 8-bit RGB/gray JPEG within the limits is
//     passed through as PdfImageData::Format::Jpeg (`bytes` = the input,
//     width/height set); everything else decodes to Format::Bgra: 8-bit
//     STRAIGHT-alpha BGRA rows, `stride == width * 4`.
//   - Unreadable or unsupported data is ErrorCode::InvalidDocument /
//     Unsupported. No exceptions; no logging of file content.
//   - Thread-safe and re-entrant: called on worker threads.
class IImageDecoder {
public:
    virtual ~IImageDecoder() = default;

    virtual core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t> encoded) const = 0;
};

} // namespace rivet::platform
