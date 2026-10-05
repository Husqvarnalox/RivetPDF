// SPDX-License-Identifier: MPL-2.0
#import "MacosImageDecoder.h"

#import <CoreFoundation/CoreFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.macos.image";

// Owning wrapper for Core Foundation / CoreGraphics objects (ARC does not
// manage CF types). Released exactly once.
template <typename T>
class CfRef {
public:
    CfRef() = default;
    explicit CfRef(T ref) : ref_(ref) {}
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
    CfRef(CfRef&& other) noexcept : ref_(std::exchange(other.ref_, nullptr)) {}
    CfRef& operator=(CfRef&& other) noexcept {
        if (this != &other) {
            reset();
            ref_ = std::exchange(other.ref_, nullptr);
        }
        return *this;
    }
    ~CfRef() { reset(); }

    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    void reset() {
        if (ref_ != nullptr) CFRelease(ref_);
        ref_ = nullptr;
    }
    T ref_ = nullptr;
};

core::Error fail(core::ErrorCode code, const char* message) {
    return core::makeError(code, message, kSubsystem);
}

bool numberFrom(CFDictionaryRef dict, CFStringRef key, std::int64_t& out) {
    if (dict == nullptr) return false;
    const void* value = CFDictionaryGetValue(dict, key);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return false;
    long long number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &number)) {
        return false;
    }
    out = number;
    return true;
}

bool stringEquals(const void* value, CFStringRef expected) {
    return value != nullptr && CFGetTypeID(value) == CFStringGetTypeID() &&
           CFStringCompare(static_cast<CFStringRef>(value), expected, 0) == kCFCompareEqualTo;
}

// Overflow-safe check of the pdf/PdfContent.hpp pixel limits.
std::optional<core::Error> checkDimensions(std::int64_t width, std::int64_t height) {
    if (width <= 0 || height <= 0) {
        return fail(core::ErrorCode::InvalidDocument, "image has a zero dimension");
    }
    if (width > static_cast<std::int64_t>(pdf::kMaxImageSide) ||
        height > static_cast<std::int64_t>(pdf::kMaxImageSide)) {
        return fail(core::ErrorCode::InvalidArgument, "image side exceeds the maximum image side");
    }
    // Both sides <= kMaxImageSide, so the 64-bit product cannot overflow.
    const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (pixels > pdf::kMaxImagePixels) {
        return fail(core::ErrorCode::InvalidArgument, "image exceeds the maximum pixel count");
    }
    return std::nullopt;
}

// Display limits of decodeBgra() (null = none), from header dimensions only.
std::optional<core::Error> checkDisplayLimits(std::int64_t width, std::int64_t height,
                                              const IImageDecoder::BgraLimits* limits) {
    if (limits == nullptr) return std::nullopt;
    if (width > static_cast<std::int64_t>(limits->maxSide) ||
        height > static_cast<std::int64_t>(limits->maxSide)) {
        return fail(core::ErrorCode::InvalidArgument, "image side exceeds the display limit");
    }
    const auto bytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4u;
    if (bytes > limits->maxDecodedBytes) {
        return fail(core::ErrorCode::InvalidArgument, "decoded image exceeds the display byte limit");
    }
    return std::nullopt;
}

// Premultiplied BGRA -> straight alpha, in place.
void unpremultiply(std::vector<std::uint8_t>& bytes) {
    for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
        const std::uint32_t a = bytes[i + 3];
        if (a == 255) continue;
        if (a == 0) {
            bytes[i] = bytes[i + 1] = bytes[i + 2] = 0;
            continue;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const std::uint32_t v = (static_cast<std::uint32_t>(bytes[i + c]) * 255u + a / 2u) / a;
            bytes[i + c] = static_cast<std::uint8_t>(std::min<std::uint32_t>(v, 255u));
        }
    }
}

} // namespace

core::Result<pdf::PdfImageData> MacosImageDecoder::decode(
    std::span<const std::uint8_t> encoded) const {
    return decodeImpl(encoded, true, nullptr);
}

core::Result<pdf::PdfImageData> MacosImageDecoder::decodeBgra(std::span<const std::uint8_t> encoded,
                                                              const BgraLimits& limits) const {
    return decodeImpl(encoded, false, &limits);
}

core::Result<pdf::PdfImageData> MacosImageDecoder::decodeImpl(std::span<const std::uint8_t> encoded,
                                                              bool passThroughJpeg,
                                                              const BgraLimits* limits) const {
    try {
        if (encoded.empty()) {
            return std::unexpected(fail(core::ErrorCode::InvalidArgument, "image data is empty"));
        }
        if (encoded.size() > pdf::kMaxImageEncodedBytes) {
            return std::unexpected(fail(core::ErrorCode::InvalidArgument,
                                        "image file exceeds the maximum encoded size"));
        }

        CfRef<CFDataRef> data{CFDataCreate(kCFAllocatorDefault, encoded.data(),
                                           static_cast<CFIndex>(encoded.size()))};
        if (!data) {
            return std::unexpected(fail(core::ErrorCode::OutOfMemory, "cannot buffer image data"));
        }

        CfRef<CGImageSourceRef> source{CGImageSourceCreateWithData(data.get(), nullptr)};
        if (!source) {
            return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
        }

        // Format gate first: only PNG and JPEG. A null type means ImageIO
        // does not recognize the data at all.
        CFStringRef type = CGImageSourceGetType(source.get());
        if (type == nullptr) {
            return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
        }
        const bool isJpeg = CFStringCompare(type, CFSTR("public.jpeg"), 0) == kCFCompareEqualTo;
        const bool isPng = CFStringCompare(type, CFSTR("public.png"), 0) == kCFCompareEqualTo;
        if (!isJpeg && !isPng) {
            return std::unexpected(
                fail(core::ErrorCode::Unsupported, "only PNG and JPEG images are supported"));
        }
        if (CGImageSourceGetCount(source.get()) < 1) {
            return std::unexpected(
                fail(core::ErrorCode::InvalidDocument, "image contains no readable frame"));
        }

        // Dimensions from the header properties BEFORE any pixel decode.
        CfRef<CFDictionaryRef> props{CGImageSourceCopyPropertiesAtIndex(source.get(), 0, nullptr)};
        std::int64_t width = 0, height = 0;
        if (!props || !numberFrom(props.get(), kCGImagePropertyPixelWidth, width) ||
            !numberFrom(props.get(), kCGImagePropertyPixelHeight, height)) {
            return std::unexpected(
                fail(core::ErrorCode::InvalidDocument, "image has no readable dimensions"));
        }
        if (auto limit = checkDimensions(width, height)) return std::unexpected(*limit);
        if (auto limit = checkDisplayLimits(width, height, limits)) return std::unexpected(*limit);

        std::int64_t orientation = 1;
        if (!numberFrom(props.get(), kCGImagePropertyOrientation, orientation)) orientation = 1;
        const bool plainlyOriented = orientation == 1;

        // JPEG pass-through: 8-bit RGB or gray, unrotated, and decodable.
        if (isJpeg && plainlyOriented && passThroughJpeg) {
            std::int64_t depth = 0;
            const bool haveDepth = numberFrom(props.get(), kCGImagePropertyDepth, depth);
            const void* model = CFDictionaryGetValue(props.get(), kCGImagePropertyColorModel);
            const bool modelOk = stringEquals(model, kCGImagePropertyColorModelRGB) ||
                                 stringEquals(model, kCGImagePropertyColorModelGray);
            if (haveDepth && depth == 8 && modelOk) {
                CfRef<CGImageRef> probe{CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr)};
                if (!probe) {
                    return std::unexpected(
                        fail(core::ErrorCode::InvalidDocument, "image cannot be decoded"));
                }
                pdf::PdfImageData out;
                out.format = pdf::PdfImageData::Format::Jpeg;
                out.width = static_cast<std::uint32_t>(width);
                out.height = static_cast<std::uint32_t>(height);
                out.stride = 0;
                out.bytes.assign(encoded.begin(), encoded.end());
                return out;
            }
        }

        // Decode to an upright CGImage.
        CfRef<CGImageRef> image;
        if (plainlyOriented) {
            image = CfRef<CGImageRef>{CGImageSourceCreateImageAtIndex(source.get(), 0, nullptr)};
        } else {
            const std::int64_t maxSide = std::max(width, height);
            CfRef<CFNumberRef> maxPixels{
                CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt64Type, &maxSide)};
            const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways,
                                  kCGImageSourceCreateThumbnailWithTransform,
                                  kCGImageSourceThumbnailMaxPixelSize};
            const void* values[] = {kCFBooleanTrue, kCFBooleanTrue, maxPixels.get()};
            CfRef<CFDictionaryRef> options{
                CFDictionaryCreate(kCFAllocatorDefault, keys, values, 3,
                                   &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
            if (!maxPixels || !options) {
                return std::unexpected(
                    fail(core::ErrorCode::OutOfMemory, "cannot prepare image decode"));
            }
            image = CfRef<CGImageRef>{
                CGImageSourceCreateThumbnailAtIndex(source.get(), 0, options.get())};
        }
        if (!image) {
            return std::unexpected(fail(core::ErrorCode::InvalidDocument, "image cannot be decoded"));
        }

        const auto outWidth = static_cast<std::int64_t>(CGImageGetWidth(image.get()));
        const auto outHeight = static_cast<std::int64_t>(CGImageGetHeight(image.get()));
        if (auto limit = checkDimensions(outWidth, outHeight)) return std::unexpected(*limit);
        if (auto limit = checkDisplayLimits(outWidth, outHeight, limits)) return std::unexpected(*limit);

        const std::size_t stride = static_cast<std::size_t>(outWidth) * 4;
        pdf::PdfImageData out;
        out.format = pdf::PdfImageData::Format::Bgra;
        out.width = static_cast<std::uint32_t>(outWidth);
        out.height = static_cast<std::uint32_t>(outHeight);
        out.stride = static_cast<std::uint32_t>(stride);
        out.bytes.assign(stride * static_cast<std::size_t>(outHeight), 0); // may throw bad_alloc

        CfRef<CGColorSpaceRef> colorSpace{CGColorSpaceCreateWithName(kCGColorSpaceSRGB)};
        if (!colorSpace) {
            return std::unexpected(fail(core::ErrorCode::Internal, "no sRGB color space"));
        }
        CfRef<CGContextRef> context{CGBitmapContextCreate(
            out.bytes.data(), static_cast<size_t>(outWidth), static_cast<size_t>(outHeight), 8,
            stride, colorSpace.get(),
            static_cast<CGBitmapInfo>(static_cast<unsigned>(kCGImageAlphaPremultipliedFirst) |
                                      static_cast<unsigned>(kCGBitmapByteOrder32Little)))};
        if (!context) {
            return std::unexpected(fail(core::ErrorCode::Internal, "cannot create bitmap context"));
        }

        CGContextSetBlendMode(context.get(), kCGBlendModeCopy);
        CGContextDrawImage(context.get(),
                           CGRectMake(0, 0, static_cast<CGFloat>(outWidth),
                                      static_cast<CGFloat>(outHeight)),
                           image.get());
        unpremultiply(out.bytes);
        return out;
    } catch (const std::bad_alloc&) {
        return std::unexpected(
            fail(core::ErrorCode::OutOfMemory, "out of memory while decoding image"));
    } catch (...) {
        return std::unexpected(
            fail(core::ErrorCode::Internal, "unexpected failure while decoding image"));
    }
}

} // namespace rivet::platform
