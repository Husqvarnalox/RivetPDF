// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ImageDecoder.hpp"

namespace rivet::platform {

// PNG (libpng simplified API) and JPEG (libjpeg / libjpeg-turbo) decoder
// with exactly the contract of MacosImageDecoder:
//   - pdf/PdfContent.hpp limits and decodeBgra's display limits are checked
//     from the header before any pixel buffer is allocated;
//   - decode(): an 8-bit RGB/YCbCr/gray JPEG without an EXIF rotation is
//     passed through as Format::Jpeg; everything else becomes straight-alpha
//     BGRA (stride = width * 4), EXIF orientation applied;
//   - decodeBgra(): always BGRA, upright.
// Stateless; thread-safe and re-entrant (each call owns its codec state).
class LinuxImageDecoder final : public IImageDecoder {
public:
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t> encoded) const override;
    core::Result<pdf::PdfImageData> decodeBgra(std::span<const std::uint8_t> encoded,
                                               const BgraLimits& limits) const override;

private:
    core::Result<pdf::PdfImageData> decodeImpl(std::span<const std::uint8_t> encoded, bool passThroughJpeg,
                                               const BgraLimits* limits) const;
};

} // namespace rivet::platform
