// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pdf/ImageReduce.hpp"
#include "platform/ImageEncoder.hpp"

namespace rivet::app {

// Adapts the platform JPEG encoder to the PDF layer's IJpegEncoder (the PDF
// layer must not depend on the platform layer; ADR-0028). Stateless: thread
// safety is the platform encoder's. `encoder` must outlive the adapter.
class JpegEncoderAdapter final : public pdf::IJpegEncoder {
public:
    explicit JpegEncoderAdapter(const platform::IImageEncoder& encoder) : encoder_(encoder) {}

    core::Result<std::vector<std::uint8_t>> encode(std::span<const std::uint8_t> pixels, std::uint32_t width,
                                                   std::uint32_t height, std::uint32_t channels,
                                                   int quality) const override {
        return encoder_.encodeJpeg(pixels, width, height, channels, quality);
    }

private:
    const platform::IImageEncoder& encoder_;
};

} // namespace rivet::app
