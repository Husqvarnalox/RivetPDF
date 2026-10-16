// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ImageEncoder.hpp"

namespace rivet::platform {

// Baseline JPEG encoding through the Windows Imaging Component (WIC), with
// the same contract as the macOS (ImageIO) and Linux (libjpeg) encoders:
// 1-channel gray or 3-channel RGB input, quality 1..100 (clamped), limits
// from pdf/PdfContent.hpp, no alpha, no EXIF, no logging of pixel data.
// Thread-safe: each call initializes COM (MTA) for its own scope and uses
// its own WIC factory.
class WindowsImageEncoder final : public IImageEncoder {
public:
    core::Result<std::vector<std::uint8_t>> encodeJpeg(std::span<const std::uint8_t> pixels,
                                                       std::uint32_t width, std::uint32_t height,
                                                       std::uint32_t channels, int quality) const override;
};

} // namespace rivet::platform
