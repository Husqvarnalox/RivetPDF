// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ImageDecoder.hpp"

namespace rivet::platform {

// PNG/JPEG decoding through the Windows Imaging Component (WIC), with the
// same contract as the macOS (ImageIO) and Linux (libpng/libjpeg) backends:
//   - limits checked from the frame header before any pixel buffer exists;
//   - a plainly oriented 8-bit gray/RGB JPEG passes through as Format::Jpeg
//     from decode(); everything else (and every decodeBgra() result) becomes
//     straight-alpha BGRA with the EXIF orientation applied;
//   - GIF/BMP/TIFF/WebP are recognized and reported as Unsupported (WIC could
//     read some of them; the other backends cannot, so neither does this one).
// Thread-safe: each call initializes COM (MTA) for its own scope and uses its
// own WIC factory.
class WindowsImageDecoder final : public IImageDecoder {
public:
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t> encoded) const override;
    core::Result<pdf::PdfImageData> decodeBgra(std::span<const std::uint8_t> encoded,
                                               const BgraLimits& limits) const override;

private:
    core::Result<pdf::PdfImageData> decodeImpl(std::span<const std::uint8_t> encoded, bool passThroughJpeg,
                                               const BgraLimits* limits) const;
};

} // namespace rivet::platform
