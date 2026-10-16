// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#import "platform/macos/MacosImageDecoder.h"
#import "platform/macos/MacosImageEncoder.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// MacosImageEncoder (ImageIO) round trips through the macOS decoder.

namespace {

using rivet::core::ErrorCode;

RIVET_TEST(macosEncoderProducesDecodableRgbJpeg) {
    rivet::platform::MacosImageEncoder encoder;
    rivet::platform::MacosImageDecoder decoder;
    const std::uint32_t width = 40;
    const std::uint32_t height = 24;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0; i < pixels.size(); i += 3) {
        pixels[i] = 220; // solid red keeps the lossy check simple
        pixels[i + 1] = 20;
        pixels[i + 2] = 20;
    }
    const auto jpeg = encoder.encodeJpeg(pixels, width, height, 3, 80);
    CHECK(jpeg.has_value());
    if (!jpeg.has_value()) return;
    CHECK(jpeg->size() > 4 && (*jpeg)[0] == 0xFF && (*jpeg)[1] == 0xD8);

    const auto decoded = decoder.decodeBgra(*jpeg, {});
    CHECK(decoded.has_value());
    if (!decoded.has_value()) return;
    CHECK_EQ(decoded->width, width);
    CHECK_EQ(decoded->height, height);
    const std::uint8_t* px = decoded->bytes.data(); // BGRA
    CHECK(px[2] > 190 && px[1] < 60 && px[0] < 60);
}

RIVET_TEST(macosEncoderEncodesGrayAndRejectsBadInput) {
    rivet::platform::MacosImageEncoder encoder;
    const std::vector<std::uint8_t> gray(16 * 16, 128);
    const auto jpeg = encoder.encodeJpeg(gray, 16, 16, 1, 60);
    CHECK(jpeg.has_value());
    CHECK(!encoder.encodeJpeg(gray, 16, 16, 2, 60).has_value());   // channels
    CHECK(!encoder.encodeJpeg(gray, 0, 16, 1, 60).has_value());    // empty
    CHECK(!encoder.encodeJpeg(gray, 32, 32, 1, 60).has_value());   // short buffer
}

} // namespace
