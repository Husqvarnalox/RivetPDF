// SPDX-License-Identifier: MPL-2.0
#include "WindowsImageEncoder.hpp"

#include "WinCommon.hpp"
#include "pdf/PdfContent.hpp"

#include <objbase.h>
#include <ocidl.h>
#include <propidl.h>
#include <wincodec.h>

#include <algorithm>
#include <cstddef>
#include <new>
#include <vector>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.windows.image";

core::Error fail(core::ErrorCode code, const char* message) {
    return core::makeError(code, message, kSubsystem);
}

} // namespace

core::Result<std::vector<std::uint8_t>> WindowsImageEncoder::encodeJpeg(std::span<const std::uint8_t> pixels,
                                                                        std::uint32_t width,
                                                                        std::uint32_t height,
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

        const win::ComScope com;
        win::ComPtr<IWICImagingFactory> factory;
        if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IWICImagingFactory, factory.putVoid()))) {
            return std::unexpected(fail(core::ErrorCode::Unsupported, "the imaging component is unavailable"));
        }

        // A growable in-memory stream (freed with the last reference).
        win::ComPtr<IStream> stream;
        if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, stream.put()))) {
            return std::unexpected(fail(core::ErrorCode::OutOfMemory, "could not allocate the output stream"));
        }
        win::ComPtr<IWICBitmapEncoder> encoder;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put())) ||
            FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache))) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoder setup failed"));
        }
        win::ComPtr<IWICBitmapFrameEncode> frame;
        win::ComPtr<IPropertyBag2> options;
        if (FAILED(encoder->CreateNewFrame(frame.put(), options.put()))) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG frame setup failed"));
        }
        if (options) {
            PROPBAG2 name{};
            wchar_t key[] = L"ImageQuality";
            name.pstrName = key;
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = static_cast<float>(std::clamp(quality, 1, 100)) / 100.0f;
            // A failed quality write leaves the codec default; not fatal.
            (void)options->Write(1, &name, &value);
        }
        WICPixelFormatGUID format = channels == 1 ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR;
        const WICPixelFormatGUID requested = format;
        if (FAILED(frame->Initialize(options.get())) || FAILED(frame->SetSize(width, height)) ||
            FAILED(frame->SetPixelFormat(&format)) || !::IsEqualGUID(format, requested)) {
            return std::unexpected(fail(core::ErrorCode::Unsupported, "JPEG pixel format unsupported"));
        }

        if (channels == 1) {
            if (FAILED(frame->WritePixels(height, static_cast<UINT>(rowBytes),
                                          static_cast<UINT>(rowBytes * height),
                                          const_cast<BYTE*>(pixels.data())))) {
                return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
            }
        } else {
            // WIC wants BGR; the contract is RGB (R first).
            std::vector<std::uint8_t> row(rowBytes);
            for (std::uint32_t y = 0; y < height; ++y) {
                const std::uint8_t* src = pixels.data() + static_cast<std::size_t>(y) * rowBytes;
                for (std::size_t x = 0; x < rowBytes; x += 3) {
                    row[x] = src[x + 2];
                    row[x + 1] = src[x + 1];
                    row[x + 2] = src[x];
                }
                if (FAILED(frame->WritePixels(1, static_cast<UINT>(rowBytes), static_cast<UINT>(rowBytes),
                                              row.data()))) {
                    return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
                }
            }
        }
        if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
        }

        STATSTG stats{};
        if (FAILED(stream->Stat(&stats, STATFLAG_NONAME)) || stats.cbSize.QuadPart < 4 ||
            stats.cbSize.QuadPart > (1ll << 31)) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
        }
        HGLOBAL memory = nullptr;
        if (FAILED(::GetHGlobalFromStream(stream.get(), &memory)) || memory == nullptr) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
        }
        const void* data = ::GlobalLock(memory);
        if (data == nullptr) {
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
        }
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::vector<std::uint8_t> result(bytes, bytes + static_cast<std::size_t>(stats.cbSize.QuadPart));
        ::GlobalUnlock(memory);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(fail(core::ErrorCode::OutOfMemory, "out of memory while encoding"));
    }
}

} // namespace rivet::platform
