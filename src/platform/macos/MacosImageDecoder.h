// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ImageDecoder.hpp"

namespace rivet::platform {

// ImageIO/CoreGraphics-backed IImageDecoder (PNG and JPEG). Stateless, so
// decode() is safe to call concurrently from worker threads.
class MacosImageDecoder final : public IImageDecoder {
public:
    core::Result<pdf::PdfImageData> decode(std::span<const std::uint8_t> encoded) const override;
};

} // namespace rivet::platform
