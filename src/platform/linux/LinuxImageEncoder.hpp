// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/ImageEncoder.hpp"

namespace rivet::platform {

// libjpeg / libjpeg-turbo baseline JPEG encoder with exactly the contract of
// IImageEncoder (and of MacosImageEncoder). Stateless; thread-safe and
// re-entrant (each call owns its codec state).
class LinuxImageEncoder final : public IImageEncoder {
public:
    core::Result<std::vector<std::uint8_t>> encodeJpeg(std::span<const std::uint8_t> pixels, std::uint32_t width,
                                                       std::uint32_t height, std::uint32_t channels,
                                                       int quality) const override;
};

} // namespace rivet::platform
