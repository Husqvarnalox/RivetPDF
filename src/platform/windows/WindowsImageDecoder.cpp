// SPDX-License-Identifier: MPL-2.0
#include "WindowsImageDecoder.hpp"

#include "WinCommon.hpp"

#include <objbase.h>
#include <propidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <new>
#include <optional>
#include <vector>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.windows.image";

core::Error fail(core::ErrorCode code, const char* message) {
    return core::makeError(code, message, kSubsystem);
}

// Overflow-safe check of the pdf/PdfContent.hpp pixel limits.
std::optional<core::Error> checkDimensions(std::uint64_t width, std::uint64_t height) {
    if (width == 0 || height == 0) return fail(core::ErrorCode::InvalidDocument, "image has a zero dimension");
    if (width > pdf::kMaxImageSide || height > pdf::kMaxImageSide) {
        return fail(core::ErrorCode::InvalidArgument, "image side exceeds the maximum image side");
    }
    if (width * height > pdf::kMaxImagePixels) {
        return fail(core::ErrorCode::InvalidArgument, "image exceeds the maximum pixel count");
    }
    return std::nullopt;
}

std::optional<core::Error> checkDisplayLimits(std::uint64_t width, std::uint64_t height,
                                              const IImageDecoder::BgraLimits* limits) {
    if (limits == nullptr) return std::nullopt;
    if (width > limits->maxSide || height > limits->maxSide) {
        return fail(core::ErrorCode::InvalidArgument, "image side exceeds the display limit");
    }
    if (width * height * 4u > limits->maxDecodedBytes) {
        return fail(core::ErrorCode::InvalidArgument, "decoded image exceeds the display byte limit");
    }
    return std::nullopt;
}

bool startsWith(std::span<const std::uint8_t> data, std::initializer_list<std::uint8_t> magic) {
    if (data.size() < magic.size()) return false;
    return std::equal(magic.begin(), magic.end(), data.begin());
}

// EXIF orientation (1..8) of a JPEG frame; 1 when absent or invalid.
int jpegOrientation(IWICBitmapFrameDecode* frame) {
    win::ComPtr<IWICMetadataQueryReader> reader;
    if (FAILED(frame->GetMetadataQueryReader(reader.put())) || !reader) return 1;
    PROPVARIANT value;
    PropVariantInit(&value);
    int orientation = 1;
    if (SUCCEEDED(reader->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value))) {
        if (value.vt == VT_UI2 && value.uiVal >= 1 && value.uiVal <= 8) orientation = value.uiVal;
    }
    PropVariantClear(&value);
    return orientation;
}

// Copies upright BGRA `src` (width x height) into `out` applying the EXIF
// orientation (the same mapping as the Linux decoder).
void orient(const std::vector<std::uint8_t>& src, std::size_t width, std::size_t height, int orientation,
            pdf::PdfImageData& out) {
    const std::size_t outWidth = out.width;
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            std::size_t dx = x;
            std::size_t dy = y;
            switch (orientation) {
            case 2: dx = width - 1 - x; break;
            case 3:
                dx = width - 1 - x;
                dy = height - 1 - y;
                break;
            case 4: dy = height - 1 - y; break;
            case 5:
                dx = y;
                dy = x;
                break;
            case 6:
                dx = height - 1 - y;
                dy = x;
                break;
            case 7:
                dx = height - 1 - y;
                dy = width - 1 - x;
                break;
            case 8:
                dx = y;
                dy = width - 1 - x;
                break;
            default: break;
            }
            std::memcpy(out.bytes.data() + (dy * outWidth + dx) * 4, src.data() + (y * width + x) * 4, 4);
        }
    }
}

} // namespace

core::Result<pdf::PdfImageData> WindowsImageDecoder::decode(std::span<const std::uint8_t> encoded) const {
    return decodeImpl(encoded, true, nullptr);
}

core::Result<pdf::PdfImageData> WindowsImageDecoder::decodeBgra(std::span<const std::uint8_t> encoded,
                                                                const BgraLimits& limits) const {
    return decodeImpl(encoded, false, &limits);
}

core::Result<pdf::PdfImageData> WindowsImageDecoder::decodeImpl(std::span<const std::uint8_t> encoded,
                                                                bool passThroughJpeg,
                                                                const BgraLimits* limits) const {
    try {
        if (encoded.empty()) return std::unexpected(fail(core::ErrorCode::InvalidArgument, "image data is empty"));
        if (encoded.size() > pdf::kMaxImageEncodedBytes) {
            return std::unexpected(
                fail(core::ErrorCode::InvalidArgument, "image file exceeds the maximum encoded size"));
        }
        const bool png = startsWith(encoded, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
        const bool jpeg = startsWith(encoded, {0xFF, 0xD8, 0xFF});
        if (!png && !jpeg) {
            if (startsWith(encoded, {'G', 'I', 'F', '8'}) || startsWith(encoded, {'B', 'M'}) ||
                startsWith(encoded, {'I', 'I', 0x2A, 0x00}) || startsWith(encoded, {'M', 'M', 0x00, 0x2A}) ||
                (startsWith(encoded, {'R', 'I', 'F', 'F'}) && encoded.size() >= 12 &&
                 std::memcmp(encoded.data() + 8, "WEBP", 4) == 0)) {
                return std::unexpected(
                    fail(core::ErrorCode::Unsupported, "only PNG and JPEG images are supported"));
            }
            return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
        }

        const win::ComScope com(COINIT_MULTITHREADED);
        win::ComPtr<IWICImagingFactory> factory;
        if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWICImagingFactory, factory.putVoid()))) {
            return std::unexpected(fail(core::ErrorCode::NotAvailable, "the Windows image codecs are unavailable"));
        }
        win::ComPtr<IWICStream> stream;
        if (FAILED(factory->CreateStream(stream.put())) ||
            FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),
                                                static_cast<DWORD>(encoded.size())))) {
            return std::unexpected(fail(core::ErrorCode::OutOfMemory, "cannot buffer the image"));
        }
        win::ComPtr<IWICBitmapDecoder> decoder;
        win::ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand,
                                                    decoder.put())) ||
            FAILED(decoder->GetFrame(0, frame.put()))) {
            return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
        }
        UINT width = 0;
        UINT height = 0;
        if (FAILED(frame->GetSize(&width, &height))) {
            return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
        }
        if (auto limit = checkDimensions(width, height)) return std::unexpected(*limit);
        if (auto limit = checkDisplayLimits(width, height, limits)) return std::unexpected(*limit);

        const int orientation = jpeg ? jpegOrientation(frame.get()) : 1;
        if (jpeg && passThroughJpeg && orientation == 1) {
            WICPixelFormatGUID format{};
            if (SUCCEEDED(frame->GetPixelFormat(&format)) &&
                (IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) || IsEqualGUID(format, GUID_WICPixelFormat8bppGray))) {
                pdf::PdfImageData out;
                out.format = pdf::PdfImageData::Format::Jpeg;
                out.width = width;
                out.height = height;
                out.stride = 0;
                out.bytes.assign(encoded.begin(), encoded.end());
                return out;
            }
        }

        // Straight (non-premultiplied) 8-bit BGRA: exactly the contract.
        win::ComPtr<IWICFormatConverter> converter;
        if (FAILED(factory->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr,
                                         0.0, WICBitmapPaletteTypeCustom))) {
            return std::unexpected(fail(core::ErrorCode::Unsupported, "unsupported image pixel format"));
        }
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
        std::vector<std::uint8_t> pixels(rowBytes * height);
        if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(rowBytes), static_cast<UINT>(pixels.size()),
                                         pixels.data()))) {
            return std::unexpected(fail(core::ErrorCode::InvalidDocument, "image cannot be decoded"));
        }

        const bool swapped = orientation >= 5;
        pdf::PdfImageData out;
        out.format = pdf::PdfImageData::Format::Bgra;
        out.width = swapped ? height : width;
        out.height = swapped ? width : height;
        out.stride = out.width * 4u;
        if (orientation == 1) {
            out.bytes = std::move(pixels);
        } else {
            out.bytes.assign(static_cast<std::size_t>(out.stride) * out.height, 0);
            orient(pixels, width, height, orientation, out);
        }
        return out;
    } catch (const std::bad_alloc&) {
        return std::unexpected(fail(core::ErrorCode::OutOfMemory, "out of memory while decoding image"));
    } catch (...) {
        return std::unexpected(fail(core::ErrorCode::Internal, "unexpected failure while decoding image"));
    }
}

} // namespace rivet::platform
