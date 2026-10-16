// SPDX-License-Identifier: MPL-2.0
#import "MacosImageEncoder.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "pdf/PdfContent.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <vector>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.macos.image";

core::Error fail(core::ErrorCode code, const char* message) {
    return core::makeError(code, message, kSubsystem);
}

template <typename T>
class CfRef {
public:
    explicit CfRef(T ref = nullptr) : ref_(ref) {}
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    ~CfRef() {
        if (ref_ != nullptr) CFRelease(ref_);
    }
    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    T ref_;
};

} // namespace

core::Result<std::vector<std::uint8_t>> MacosImageEncoder::encodeJpeg(std::span<const std::uint8_t> pixels,
                                                                      std::uint32_t width, std::uint32_t height,
                                                                      std::uint32_t channels,
                                                                      int quality) const {
    try {
        if (width == 0 || height == 0 || width > pdf::kMaxImageSide || height > pdf::kMaxImageSide ||
            static_cast<std::uint64_t>(width) * height > pdf::kMaxImagePixels ||
            (channels != 1 && channels != 3)) {
            return std::unexpected(fail(core::ErrorCode::InvalidArgument, "unsupported image dimensions"));
        }
        const std::size_t rowBytes = static_cast<std::size_t>(width) * channels;
        if (pixels.size() < rowBytes * height) {
            return std::unexpected(fail(core::ErrorCode::InvalidArgument, "pixel buffer is too small"));
        }
        const double lossy = static_cast<double>(std::clamp(quality, 1, 100)) / 100.0;

        CfRef<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, pixels.data(),
                                           static_cast<CFIndex>(rowBytes * height)));
        if (!data) return std::unexpected(fail(core::ErrorCode::OutOfMemory, "could not copy the pixels"));
        CfRef<CGDataProviderRef> provider(CGDataProviderCreateWithCFData(data.get()));
        CfRef<CGColorSpaceRef> space(channels == 1 ? CGColorSpaceCreateDeviceGray()
                                                   : CGColorSpaceCreateDeviceRGB());
        if (!provider || !space) return std::unexpected(fail(core::ErrorCode::Internal, "no color space"));
        CfRef<CGImageRef> image(CGImageCreate(width, height, 8, 8 * channels, rowBytes, space.get(),
                                              static_cast<CGBitmapInfo>(kCGImageAlphaNone), provider.get(),
                                              nullptr, false, kCGRenderingIntentDefault));
        if (!image) return std::unexpected(fail(core::ErrorCode::Internal, "could not create the image"));

        CfRef<CFMutableDataRef> output(CFDataCreateMutable(kCFAllocatorDefault, 0));
        if (!output) return std::unexpected(fail(core::ErrorCode::OutOfMemory, "could not allocate output"));
        CfRef<CGImageDestinationRef> destination(
            CGImageDestinationCreateWithData(output.get(), CFSTR("public.jpeg"), 1, nullptr));
        if (!destination) return std::unexpected(fail(core::ErrorCode::Unsupported, "no JPEG encoder"));
        CfRef<CFNumberRef> qualityNumber(CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &lossy));
        const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
        const void* values[] = {qualityNumber.get()};
        CfRef<CFDictionaryRef> options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                          &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks));
        CGImageDestinationAddImage(destination.get(), image.get(), options.get());
        if (!CGImageDestinationFinalize(destination.get())) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
        }
        const auto length = static_cast<std::size_t>(CFDataGetLength(output.get()));
        const UInt8* bytes = CFDataGetBytePtr(output.get());
        if (length < 4 || bytes == nullptr) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding produced no data"));
        }
        return std::vector<std::uint8_t>(bytes, bytes + length);
    } catch (const std::bad_alloc&) {
        return std::unexpected(fail(core::ErrorCode::OutOfMemory, "out of memory while encoding"));
    } catch (...) {
        return std::unexpected(fail(core::ErrorCode::Internal, "unexpected encoder failure"));
    }
}

} // namespace rivet::platform
