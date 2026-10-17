// SPDX-License-Identifier: MPL-2.0
#include "LinuxImageEncoder.hpp"

#include "pdf/PdfContent.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio> // jpeglib.h needs FILE
#include <cstdlib>
#include <new>
#include <vector>

#include <jpeglib.h>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.linux.image";

core::Error fail(core::ErrorCode code, const char* message) {
    return core::makeError(code, message, kSubsystem);
}

struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
};

extern "C" void encoderJpegErrorExit(j_common_ptr cinfo) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    std::longjmp(manager->jump, 1);
}

extern "C" void encoderJpegSilentMessage(j_common_ptr) {} // never log image content

// libjpeg compression with setjmp error handling, kept POD-only (no C++
// objects with destructors live across the setjmp). On success `*out` is a
// malloc'ed buffer of `*outSize` bytes the caller frees.
bool compress(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height, std::uint32_t channels,
              int quality, unsigned char** out, unsigned long* outSize) {
    jpeg_compress_struct cinfo{};
    JpegErrorManager errors{};
    cinfo.err = jpeg_std_error(&errors.base);
    errors.base.error_exit = encoderJpegErrorExit;
    errors.base.output_message = encoderJpegSilentMessage;
    jpeg_create_compress(&cinfo);
    if (setjmp(errors.jump) != 0) {
        jpeg_destroy_compress(&cinfo);
        if (*out != nullptr) {
            std::free(*out);
            *out = nullptr;
        }
        return false;
    }
    jpeg_mem_dest(&cinfo, out, outSize);
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = static_cast<int>(channels);
    cinfo.in_color_space = channels == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    const std::size_t rowBytes = static_cast<std::size_t>(width) * channels;
    while (cinfo.next_scanline < cinfo.image_height) {
        // libjpeg's row pointer is not const-qualified; it only reads.
        JSAMPROW row = const_cast<JSAMPROW>(pixels + static_cast<std::size_t>(cinfo.next_scanline) * rowBytes);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return true;
}

} // namespace

core::Result<std::vector<std::uint8_t>> LinuxImageEncoder::encodeJpeg(std::span<const std::uint8_t> pixels,
                                                                      std::uint32_t width, std::uint32_t height,
                                                                      std::uint32_t channels,
                                                                      int quality) const {
    try {
        if (width == 0 || height == 0 || width > pdf::kMaxImageSide || height > pdf::kMaxImageSide ||
            static_cast<std::uint64_t>(width) * height > pdf::kMaxImagePixels ||
            (channels != 1 && channels != 3)) {
            return std::unexpected(fail(core::ErrorCode::InvalidArgument, "unsupported image dimensions"));
        }
        if (pixels.size() < static_cast<std::size_t>(width) * channels * height) {
            return std::unexpected(fail(core::ErrorCode::InvalidArgument, "pixel buffer is too small"));
        }
        unsigned char* buffer = nullptr;
        unsigned long size = 0;
        if (!compress(pixels.data(), width, height, channels, std::clamp(quality, 1, 100), &buffer, &size) ||
            buffer == nullptr || size < 4) {
            if (buffer != nullptr) std::free(buffer);
            return std::unexpected(fail(core::ErrorCode::Internal, "JPEG encoding failed"));
        }
        std::vector<std::uint8_t> result(buffer, buffer + size);
        std::free(buffer);
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(fail(core::ErrorCode::OutOfMemory, "out of memory while encoding"));
    }
}

} // namespace rivet::platform
