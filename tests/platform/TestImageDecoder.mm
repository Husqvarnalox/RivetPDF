// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#import "platform/macos/MacosImageDecoder.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

// MacosImageDecoder against inputs encoded in-process with ImageIO. Nothing
// is read from disk.

namespace {

using rivet::core::ErrorCode;
using rivet::pdf::PdfImageData;

struct CfDeleter {
    void operator()(const void* ref) const {
        if (ref != nullptr) CFRelease(ref);
    }
};
template <typename T>
using Cf = std::unique_ptr<std::remove_pointer_t<T>, CfDeleter>;

// Builds an 8-bit CGImage over a copy of `pixels`.
Cf<CGImageRef> makeImage(std::size_t width, std::size_t height, CGColorSpaceRef space,
                         std::size_t components, CGBitmapInfo info,
                         const std::vector<std::uint8_t>& pixels) {
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, pixels.data(),
                                  static_cast<CFIndex>(pixels.size()));
    Cf<CFDataRef> dataOwner{data};
    Cf<CGDataProviderRef> provider{CGDataProviderCreateWithCFData(data)};
    return Cf<CGImageRef>{CGImageCreate(width, height, 8, 8 * components, width * components,
                                        space, info, provider.get(), nullptr, false,
                                        kCGRenderingIntentDefault)};
}

// Encodes `image` with `type` (UTI) and optional EXIF-style orientation.
std::vector<std::uint8_t> encode(CGImageRef image, CFStringRef type, int orientation = 0) {
    Cf<CFMutableDataRef> out{CFDataCreateMutable(kCFAllocatorDefault, 0)};
    Cf<CGImageDestinationRef> dest{CGImageDestinationCreateWithData(out.get(), type, 1, nullptr)};
    CFDictionaryRef props = nullptr;
    Cf<CFNumberRef> orientationNumber;
    Cf<CFDictionaryRef> propsOwner;
    if (orientation != 0) {
        orientationNumber.reset(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &orientation));
        const void* keys[] = {kCGImagePropertyOrientation};
        const void* values[] = {orientationNumber.get()};
        props = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                   &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        propsOwner.reset(props);
    }
    CGImageDestinationAddImage(dest.get(), image, props);
    CHECK(CGImageDestinationFinalize(dest.get()));
    const auto* bytes = CFDataGetBytePtr(out.get());
    return std::vector<std::uint8_t>(bytes, bytes + CFDataGetLength(out.get()));
}

std::vector<std::uint8_t> rgbGradientPixels(std::size_t w, std::size_t h) {
    std::vector<std::uint8_t> px(w * h * 4);
    for (std::size_t y = 0; y < h; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            auto* p = &px[(y * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1));
            p[2] = 128;
            p[3] = 255;
        }
    }
    return px;
}

std::vector<std::uint8_t> rgbJpeg(std::size_t w, std::size_t h, int orientation = 0) {
    Cf<CGColorSpaceRef> space{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
    auto image = makeImage(w, h, space.get(), 4, kCGImageAlphaNoneSkipLast, rgbGradientPixels(w, h));
    return encode(image.get(), CFSTR("public.jpeg"), orientation);
}

std::vector<std::uint8_t> grayPng(std::size_t w, std::size_t h) {
    Cf<CGColorSpaceRef> space{CGColorSpaceCreateWithName(kCGColorSpaceGenericGrayGamma2_2)};
    auto image = makeImage(w, h, space.get(), 1, kCGImageAlphaNone,
                           std::vector<std::uint8_t>(w * h, 0x40));
    return encode(image.get(), CFSTR("public.png"));
}

rivet::platform::MacosImageDecoder decoder;

} // namespace

RIVET_TEST(imageDecoderPngWithAlphaYieldsStraightAlphaBgra) {
    // 2x2 straight-alpha RGBA: half-transparent red, opaque green, blue,
    // fully transparent white.
    const std::vector<std::uint8_t> rgba = {
        255, 0,   0,   128, //
        0,   255, 0,   255, //
        0,   0,   255, 255, //
        255, 255, 255, 0,
    };
    Cf<CGColorSpaceRef> space{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
    auto image = makeImage(2, 2, space.get(), 4, kCGImageAlphaLast, rgba);
    const auto png = encode(image.get(), CFSTR("public.png"));

    auto result = decoder.decode(png);
    CHECK(result.has_value());
    CHECK(result->format == PdfImageData::Format::Bgra);
    CHECK_EQ(result->width, 2u);
    CHECK_EQ(result->height, 2u);
    CHECK_EQ(result->stride, 8u);
    CHECK_EQ(result->bytes.size(), std::size_t{16});

    const auto& b = result->bytes;
    // Pixel 0: half-transparent red, STRAIGHT (not premultiplied).
    CHECK_EQ(b[0], 0);
    CHECK_EQ(b[1], 0);
    CHECK_EQ(b[2], 255);
    CHECK_EQ(b[3], 128);
    // Pixel 1: opaque green.
    CHECK_EQ(b[4], 0);
    CHECK_EQ(b[5], 255);
    CHECK_EQ(b[6], 0);
    CHECK_EQ(b[7], 255);
    // Pixel 2: opaque blue (BGRA order).
    CHECK_EQ(b[8], 255);
    CHECK_EQ(b[9], 0);
    CHECK_EQ(b[10], 0);
    CHECK_EQ(b[11], 255);
    // Pixel 3: fully transparent -> all zero.
    CHECK_EQ(b[12], 0);
    CHECK_EQ(b[13], 0);
    CHECK_EQ(b[14], 0);
    CHECK_EQ(b[15], 0);
}

RIVET_TEST(imageDecoderPlainJpegPassesThroughUnchanged) {
    const auto jpeg = rgbJpeg(16, 8);
    auto result = decoder.decode(jpeg);
    CHECK(result.has_value());
    CHECK(result->format == PdfImageData::Format::Jpeg);
    CHECK_EQ(result->width, 16u);
    CHECK_EQ(result->height, 8u);
    CHECK_EQ(result->stride, 0u);
    CHECK(result->bytes == jpeg);
}

RIVET_TEST(imageDecoderDecodeBgraNeverPassesJpegThrough) {
    const auto jpeg = rgbJpeg(16, 8);
    auto result = decoder.decodeBgra(jpeg, {});
    CHECK(result.has_value());
    CHECK(result->format == PdfImageData::Format::Bgra);
    CHECK_EQ(result->width, 16u);
    CHECK_EQ(result->height, 8u);
    CHECK_EQ(result->stride, 64u);
    CHECK_EQ(result->bytes.size(), std::size_t{16 * 8 * 4});
}

RIVET_TEST(imageDecoderDecodeBgraEnforcesDisplayLimits) {
    const auto png = grayPng(64, 64);
    rivet::platform::IImageDecoder::BgraLimits side;
    side.maxSide = 32;
    auto bySide = decoder.decodeBgra(png, side);
    CHECK(!bySide.has_value());
    CHECK(bySide.error().code == ErrorCode::InvalidArgument);
    rivet::platform::IImageDecoder::BgraLimits bytes;
    bytes.maxDecodedBytes = 64 * 64 * 4 - 1;
    auto byBytes = decoder.decodeBgra(png, bytes);
    CHECK(!byBytes.has_value());
    CHECK(byBytes.error().code == ErrorCode::InvalidArgument);
    CHECK(decoder.decodeBgra(png, {}).has_value());
}

RIVET_TEST(imageDecoderGrayJpegPassesThrough) {
    Cf<CGColorSpaceRef> space{CGColorSpaceCreateWithName(kCGColorSpaceGenericGrayGamma2_2)};
    auto image = makeImage(8, 8, space.get(), 1, kCGImageAlphaNone,
                           std::vector<std::uint8_t>(64, 0x80));
    const auto jpeg = encode(image.get(), CFSTR("public.jpeg"));
    auto result = decoder.decode(jpeg);
    CHECK(result.has_value());
    CHECK(result->format == PdfImageData::Format::Jpeg);
    CHECK(result->bytes == jpeg);
}

RIVET_TEST(imageDecoderRotatedJpegDecodesUprightToBgra) {
    // Orientation 6 (rotate 90 degrees CW to display): a stored 4x2 image
    // is shown as 2x4.
    const auto jpeg = rgbJpeg(4, 2, 6);
    auto result = decoder.decode(jpeg);
    CHECK(result.has_value());
    CHECK(result->format == PdfImageData::Format::Bgra);
    CHECK_EQ(result->width, 2u);
    CHECK_EQ(result->height, 4u);
    CHECK_EQ(result->stride, 8u);
    CHECK_EQ(result->bytes.size(), std::size_t{32});
}

RIVET_TEST(imageDecoderCmykJpegDecodesToBgra) {
    Cf<CGColorSpaceRef> space{CGColorSpaceCreateDeviceCMYK()};
    auto image = makeImage(4, 4, space.get(), 4, kCGImageAlphaNone,
                           std::vector<std::uint8_t>(4 * 4 * 4, 0x30));
    const auto jpeg = encode(image.get(), CFSTR("public.jpeg"));
    auto result = decoder.decode(jpeg);
    CHECK(result.has_value());
    CHECK(result->format == PdfImageData::Format::Bgra);
    CHECK_EQ(result->width, 4u);
    CHECK_EQ(result->height, 4u);
    CHECK_EQ(result->stride, 16u);
    CHECK_EQ(result->bytes.size(), std::size_t{64});
}

RIVET_TEST(imageDecoderRejectsSideBeyondTheLimit) {
    // 10001 x 1: a few dozen bytes, but one side over kMaxImageSide.
    const auto png = grayPng(rivet::pdf::kMaxImageSide + 1, 1);
    auto result = decoder.decode(png);
    CHECK(!result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
}

RIVET_TEST(imageDecoderRejectsPixelCountBeyondTheLimit) {
    // Both sides within kMaxImageSide, product just over kMaxImagePixels.
    const std::size_t w = rivet::pdf::kMaxImageSide;
    const std::size_t h = rivet::pdf::kMaxImagePixels / w + 1;
    const auto png = grayPng(w, h);
    auto result = decoder.decode(png);
    CHECK(!result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
}

RIVET_TEST(imageDecoderRejectsOversizeEncodedInput) {
    std::vector<std::uint8_t> huge(rivet::pdf::kMaxImageEncodedBytes + 1, 0x42);
    auto result = decoder.decode(huge);
    CHECK(!result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
}

RIVET_TEST(imageDecoderRejectsEmptyInput) {
    auto result = decoder.decode(std::span<const std::uint8_t>{});
    CHECK(!result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidArgument);
}

RIVET_TEST(imageDecoderRejectsGarbage) {
    std::vector<std::uint8_t> garbage(300);
    for (std::size_t i = 0; i < garbage.size(); ++i) garbage[i] = static_cast<std::uint8_t>(i * 7 + 3);
    auto result = decoder.decode(garbage);
    CHECK(!result.has_value());
    CHECK(result.error().code == ErrorCode::InvalidDocument);
}

RIVET_TEST(imageDecoderRejectsTruncatedPng) {
    auto png = grayPng(32, 32);
    png.resize(12); // signature + a fragment of IHDR
    auto result = decoder.decode(png);
    CHECK(!result.has_value());
}

RIVET_TEST(imageDecoderRejectsGifAndTiffAsUnsupported) {
    Cf<CGColorSpaceRef> space{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
    auto image = makeImage(4, 4, space.get(), 4, kCGImageAlphaNoneSkipLast, rgbGradientPixels(4, 4));
    const auto tiff = encode(image.get(), CFSTR("public.tiff"));
    auto tiffResult = decoder.decode(tiff);
    CHECK(!tiffResult.has_value());
    CHECK(tiffResult.error().code == ErrorCode::Unsupported);

    const auto gif = encode(image.get(), CFSTR("com.compuserve.gif"));
    auto gifResult = decoder.decode(gif);
    CHECK(!gifResult.has_value());
    CHECK(gifResult.error().code == ErrorCode::Unsupported);
}
