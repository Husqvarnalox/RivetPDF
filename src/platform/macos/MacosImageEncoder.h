// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ImageEncoder.hpp"

namespace rivet::platform {

// ImageIO/CoreGraphics-backed IImageEncoder (baseline JPEG). Stateless, so
// encodeJpeg() is safe to call concurrently from worker threads.
class MacosImageEncoder final : public IImageEncoder {
public:
    core::Result<std::vector<std::uint8_t>> encodeJpeg(std::span<const std::uint8_t> pixels, std::uint32_t width,
                                                       std::uint32_t height, std::uint32_t channels,
                                                       int quality) const override;
};

} // namespace rivet::platform
