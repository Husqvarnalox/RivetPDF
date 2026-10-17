// SPDX-License-Identifier: MPL-2.0
#include "LinuxImageDecoder.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstddef>
#include <cstdint>
#include <cstdio> // jpeglib.h needs FILE
#include <cstring>
#include <new>
#include <optional>
#include <vector>

#include <jpeglib.h>
#include <png.h>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.linux.image";

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

// ---- EXIF orientation -------------------------------------------------------

// Orientation (1..8) from an APP1 "Exif" payload; 1 when absent/invalid.
int exifOrientation(const std::uint8_t* data, std::size_t size) {
    if (size < 14 || std::memcmp(data, "Exif\0\0", 6) != 0) return 1;
    const std::uint8_t* tiff = data + 6;
    const std::size_t length = size - 6;
    const bool little = tiff[0] == 'I' && tiff[1] == 'I';
    const bool big = tiff[0] == 'M' && tiff[1] == 'M';
    if (!little && !big) return 1;
    const auto read16 = [&](std::size_t at) -> std::uint32_t {
        return little ? (tiff[at] | (tiff[at + 1] << 8)) : ((tiff[at] << 8) | tiff[at + 1]);
    };
    const auto read32 = [&](std::size_t at) -> std::uint32_t {
        return little ? (read16(at) | (read16(at + 2) << 16)) : ((read16(at) << 16) | read16(at + 2));
    };
    if (read16(2) != 42) return 1;
    const std::uint32_t ifd = read32(4);
    if (ifd > length || length - ifd < 2) return 1;
    const std::uint32_t entries = read16(ifd);
    for (std::uint32_t i = 0; i < entries; ++i) {
        const std::size_t entry = ifd + 2 + static_cast<std::size_t>(i) * 12;
        if (entry + 12 > length) return 1;
        if (read16(entry) == 0x0112) {
            const std::uint32_t value = read16(entry + 8);
            return value >= 1 && value <= 8 ? static_cast<int>(value) : 1;
        }
    }
    return 1;
}

// ---- libjpeg (setjmp error handling kept in small POD-only functions) ------

struct JpegErrorManager {
    jpeg_error_mgr base;
    std::jmp_buf jump;
};

extern "C" void jpegErrorExit(j_common_ptr cinfo) {
    auto* manager = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    std::longjmp(manager->jump, 1);
}

extern "C" void jpegSilentMessage(j_common_ptr) {} // never log file content

struct JpegHeader {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int components = 0;
    J_COLOR_SPACE colorSpace = JCS_UNKNOWN;
    int precision = 0;
    int orientation = 1;
};

struct JpegJob {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
    JpegHeader header;
    // Decode output (pre-sized by the caller before decodeJpegPixels).
    std::uint8_t* pixels = nullptr;
    int outComponents = 0;
    bool adobeInverted = false;
};

void setupJpeg(jpeg_decompress_struct* cinfo, JpegErrorManager* errors, JpegJob* job) {
    cinfo->err = jpeg_std_error(&errors->base);
    errors->base.error_exit = jpegErrorExit;
    errors->base.output_message = jpegSilentMessage;
    jpeg_create_decompress(cinfo);
    jpeg_mem_src(cinfo, const_cast<unsigned char*>(job->data), static_cast<unsigned long>(job->size));
    jpeg_save_markers(cinfo, JPEG_APP0 + 1, 0xFFFF);
}

void captureHeader(jpeg_decompress_struct* cinfo, JpegJob* job) {
    job->header.width = cinfo->image_width;
    job->header.height = cinfo->image_height;
    job->header.components = cinfo->num_components;
    job->header.colorSpace = cinfo->jpeg_color_space;
    job->header.precision = cinfo->data_precision;
    job->adobeInverted = cinfo->saw_Adobe_marker != 0;
    for (jpeg_saved_marker_ptr marker = cinfo->marker_list; marker != nullptr; marker = marker->next) {
        if (marker->marker == JPEG_APP0 + 1) {
            job->header.orientation = exifOrientation(marker->data, marker->data_length);
            break;
        }
    }
}

bool readJpegHeader(JpegJob* job) {
    jpeg_decompress_struct cinfo{};
    JpegErrorManager errors{};
    if (setjmp(errors.jump) != 0) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    setupJpeg(&cinfo, &errors, job);
    jpeg_read_header(&cinfo, TRUE);
    captureHeader(&cinfo, job);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

J_COLOR_SPACE outputSpace(J_COLOR_SPACE in) {
    if (in == JCS_GRAYSCALE) return JCS_GRAYSCALE;
    if (in == JCS_CMYK || in == JCS_YCCK) return JCS_CMYK;
    return JCS_RGB;
}

void readScanlines(jpeg_decompress_struct* cinfo, JpegJob* job) {
    const std::size_t rowBytes = static_cast<std::size_t>(cinfo->output_width) * job->outComponents;
    while (cinfo->output_scanline < cinfo->output_height) {
        JSAMPROW row = job->pixels + static_cast<std::size_t>(cinfo->output_scanline) * rowBytes;
        jpeg_read_scanlines(cinfo, &row, 1);
    }
}

bool decodeJpegPixels(JpegJob* job) {
    jpeg_decompress_struct cinfo{};
    JpegErrorManager errors{};
    if (setjmp(errors.jump) != 0) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    setupJpeg(&cinfo, &errors, job);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = outputSpace(cinfo.jpeg_color_space);
    jpeg_start_decompress(&cinfo);
    if (cinfo.output_width != job->header.width || cinfo.output_height != job->header.height ||
        cinfo.output_components != job->outComponents) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    readScanlines(&cinfo, job);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

// Writes `raw` (gray/RGB/CMYK rows, width x height) as upright BGRA.
void toBgra(const std::vector<std::uint8_t>& raw, const JpegJob& job, pdf::PdfImageData& out) {
    const std::size_t width = job.header.width;
    const std::size_t height = job.header.height;
    const int orientation = job.header.orientation;
    const std::size_t outWidth = out.width;
    const int components = job.outComponents;
    for (std::size_t y = 0; y < height; ++y) {
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t* p = raw.data() + (y * width + x) * static_cast<std::size_t>(components);
            std::uint8_t r = 0;
            std::uint8_t g = 0;
            std::uint8_t b = 0;
            if (components == 1) {
                r = g = b = p[0];
            } else if (components == 3) {
                r = p[0];
                g = p[1];
                b = p[2];
            } else {
                // Adobe CMYK JPEGs store inverted values.
                const unsigned c = job.adobeInverted ? p[0] : 255u - p[0];
                const unsigned m = job.adobeInverted ? p[1] : 255u - p[1];
                const unsigned ye = job.adobeInverted ? p[2] : 255u - p[2];
                const unsigned k = job.adobeInverted ? p[3] : 255u - p[3];
                r = static_cast<std::uint8_t>(c * k / 255u);
                g = static_cast<std::uint8_t>(m * k / 255u);
                b = static_cast<std::uint8_t>(ye * k / 255u);
            }
            std::size_t dx = x;
            std::size_t dy = y;
            switch (orientation) {
            case 2: dx = width - 1 - x; break;
            case 3: dx = width - 1 - x; dy = height - 1 - y; break;
            case 4: dy = height - 1 - y; break;
            case 5: dx = y; dy = x; break;
            case 6: dx = height - 1 - y; dy = x; break;
            case 7: dx = height - 1 - y; dy = width - 1 - x; break;
            case 8: dx = y; dy = width - 1 - x; break;
            default: break;
            }
            std::uint8_t* d = out.bytes.data() + (dy * outWidth + dx) * 4;
            d[0] = b;
            d[1] = g;
            d[2] = r;
            d[3] = 255;
        }
    }
}

core::Result<pdf::PdfImageData> decodeJpeg(std::span<const std::uint8_t> encoded, bool passThrough,
                                           const IImageDecoder::BgraLimits* limits) {
    JpegJob job;
    job.data = encoded.data();
    job.size = encoded.size();
    if (!readJpegHeader(&job)) {
        return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
    }
    const JpegHeader& header = job.header;
    if (auto limit = checkDimensions(header.width, header.height)) return std::unexpected(*limit);
    if (auto limit = checkDisplayLimits(header.width, header.height, limits)) return std::unexpected(*limit);

    const bool plainSpace = (header.colorSpace == JCS_GRAYSCALE && header.components == 1) ||
                            ((header.colorSpace == JCS_YCbCr || header.colorSpace == JCS_RGB) && header.components == 3);
    if (passThrough && header.orientation == 1 && header.precision == 8 && plainSpace) {
        pdf::PdfImageData out;
        out.format = pdf::PdfImageData::Format::Jpeg;
        out.width = header.width;
        out.height = header.height;
        out.stride = 0;
        out.bytes.assign(encoded.begin(), encoded.end());
        return out;
    }

    const J_COLOR_SPACE space = outputSpace(header.colorSpace);
    job.outComponents = space == JCS_GRAYSCALE ? 1 : (space == JCS_CMYK ? 4 : 3);
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(header.width) * header.height *
                                  static_cast<std::size_t>(job.outComponents));
    job.pixels = raw.data();
    if (!decodeJpegPixels(&job)) {
        return std::unexpected(fail(core::ErrorCode::InvalidDocument, "image cannot be decoded"));
    }

    const bool swapped = header.orientation >= 5;
    pdf::PdfImageData out;
    out.format = pdf::PdfImageData::Format::Bgra;
    out.width = swapped ? header.height : header.width;
    out.height = swapped ? header.width : header.height;
    out.stride = out.width * 4u;
    out.bytes.assign(static_cast<std::size_t>(out.stride) * out.height, 0);
    toBgra(raw, job, out);
    return out;
}

core::Result<pdf::PdfImageData> decodePng(std::span<const std::uint8_t> encoded,
                                          const IImageDecoder::BgraLimits* limits) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_memory(&image, encoded.data(), encoded.size()) == 0) {
        png_image_free(&image);
        return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
    }
    const auto failWith = [&image](core::Error error) {
        png_image_free(&image);
        return std::unexpected(std::move(error));
    };
    if (auto limit = checkDimensions(image.width, image.height)) return failWith(*limit);
    if (auto limit = checkDisplayLimits(image.width, image.height, limits)) return failWith(*limit);

    // 8-bit sRGB BGRA output of the simplified API is NOT premultiplied:
    // exactly the straight-alpha contract.
    image.format = PNG_FORMAT_BGRA;
    pdf::PdfImageData out;
    out.format = pdf::PdfImageData::Format::Bgra;
    out.width = image.width;
    out.height = image.height;
    out.stride = image.width * 4u;
    out.bytes.assign(static_cast<std::size_t>(out.stride) * out.height, 0);
    if (png_image_finish_read(&image, nullptr, out.bytes.data(), static_cast<png_int_32>(out.stride), nullptr) ==
        0) {
        png_image_free(&image);
        return std::unexpected(fail(core::ErrorCode::InvalidDocument, "image cannot be decoded"));
    }
    return out;
}

} // namespace

core::Result<pdf::PdfImageData> LinuxImageDecoder::decode(std::span<const std::uint8_t> encoded) const {
    return decodeImpl(encoded, true, nullptr);
}

core::Result<pdf::PdfImageData> LinuxImageDecoder::decodeBgra(std::span<const std::uint8_t> encoded,
                                                              const BgraLimits& limits) const {
    return decodeImpl(encoded, false, &limits);
}

core::Result<pdf::PdfImageData> LinuxImageDecoder::decodeImpl(std::span<const std::uint8_t> encoded,
                                                              bool passThroughJpeg, const BgraLimits* limits) const {
    try {
        if (encoded.empty()) {
            return std::unexpected(fail(core::ErrorCode::InvalidArgument, "image data is empty"));
        }
        if (encoded.size() > pdf::kMaxImageEncodedBytes) {
            return std::unexpected(
                fail(core::ErrorCode::InvalidArgument, "image file exceeds the maximum encoded size"));
        }
        if (startsWith(encoded, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A})) return decodePng(encoded, limits);
        if (startsWith(encoded, {0xFF, 0xD8, 0xFF})) return decodeJpeg(encoded, passThroughJpeg, limits);
        // Recognizable but unsupported formats (GIF, BMP, TIFF, WebP).
        if (startsWith(encoded, {'G', 'I', 'F', '8'}) || startsWith(encoded, {'B', 'M'}) ||
            startsWith(encoded, {'I', 'I', 0x2A, 0x00}) || startsWith(encoded, {'M', 'M', 0x00, 0x2A}) ||
            (startsWith(encoded, {'R', 'I', 'F', 'F'}) && encoded.size() >= 12 &&
             std::memcmp(encoded.data() + 8, "WEBP", 4) == 0)) {
            return std::unexpected(fail(core::ErrorCode::Unsupported, "only PNG and JPEG images are supported"));
        }
        return std::unexpected(fail(core::ErrorCode::InvalidDocument, "unreadable image data"));
    } catch (const std::bad_alloc&) {
        return std::unexpected(fail(core::ErrorCode::OutOfMemory, "out of memory while decoding image"));
    } catch (...) {
        return std::unexpected(fail(core::ErrorCode::Internal, "unexpected failure while decoding image"));
    }
}

} // namespace rivet::platform
