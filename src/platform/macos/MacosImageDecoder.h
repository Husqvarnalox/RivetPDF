// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ImageDecoder.hpp"

namespace rivet::platform {

// ImageIO/CoreGraphics-backed IImageDecoder (PNG and JPEG). Stateless, so
// decode() is safe to call concurrently from worker threads.
class MacosImageDecoder final : public IImageDecoder {
public:
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t> encoded) const override;
    core::Result<pdf::PdfImageData> decodeBgra(std::span<const std::uint8_t> encoded,
                                               const BgraLimits& limits) const override;

private:
    // `passThroughJpeg` false forces the BGRA path; `limits` (optional) adds
    // the display limits to the PdfContent.hpp ones.
    core::Result<pdf::PdfImageData> decodeImpl(std::span<const std::uint8_t> encoded, bool passThroughJpeg,
                                               const BgraLimits* limits) const;
};

} // namespace rivet::platform
