// SPDX-License-Identifier: MPL-2.0

#include "PdfiumImageReduce.h"

#include "PdfiumWriting.h"
#include "pdf/PdfContent.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fpdf_edit.h"
#include "fpdfview.h"

namespace rivet::pdf::internal {
namespace {

core::Error internalError(std::string message) {
    return core::makeError(core::ErrorCode::Internal, std::move(message), "pdf");
}

class ScopedBitmap {
public:
    explicit ScopedBitmap(FPDF_BITMAP bitmap) : bitmap_(bitmap) {}
    ~ScopedBitmap() {
        if (bitmap_ != nullptr) FPDFBitmap_Destroy(bitmap_);
    }
    ScopedBitmap(const ScopedBitmap&) = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;
    FPDF_BITMAP get() const { return bitmap_; }

private:
    FPDF_BITMAP bitmap_;
};

std::vector<std::uint8_t> rawData(FPDF_PAGEOBJECT image) {
    const unsigned long size = FPDFImageObj_GetImageDataRaw(image, nullptr, 0);
    std::vector<std::uint8_t> data(size);
    if (size > 0) FPDFImageObj_GetImageDataRaw(image, data.data(), size);
    return data;
}

bool isJpeg(FPDF_PAGEOBJECT image) {
    const int filters = FPDFImageObj_GetImageFilterCount(image);
    if (filters <= 0) return false;
    char name[16] = {};
    const unsigned long length = FPDFImageObj_GetImageFilter(image, filters - 1, name, sizeof(name));
    // The length includes the terminating NUL.
    return length == 10 && std::string(name) == "DCTDecode";
}

// PDFium replaces an image in place on its shared CPDF_Image: another object
// showing the same XObject would change with it (and may be placed larger).
bool sharesImage(const std::vector<FPDF_PAGEOBJECT>& images, FPDF_PAGEOBJECT image) {
    unsigned int width = 0;
    unsigned int height = 0;
    if (FPDFImageObj_GetImagePixelSize(image, &width, &height) == 0) return true;
    std::optional<std::vector<std::uint8_t>> mine;
    for (const FPDF_PAGEOBJECT other : images) {
        if (other == image) continue;
        unsigned int otherWidth = 0;
        unsigned int otherHeight = 0;
        if (FPDFImageObj_GetImagePixelSize(other, &otherWidth, &otherHeight) == 0 || otherWidth != width ||
            otherHeight != height) {
            continue;
        }
        if (!mine.has_value()) mine = rawData(image);
        if (mine->empty() || rawData(other) == *mine) return true;
    }
    return false;
}

// True when the image, as drawn, has translucent pixels (an /SMask, /Mask,
// stencil or alpha). The image is rendered with its mask at placement size;
// the outermost pixel ring is ignored (anti-aliased edges). A rendering that
// cannot be obtained counts as masked: the image stays as it is.
bool hasTransparency(FPDF_DOCUMENT document, FPDF_PAGE page, FPDF_PAGEOBJECT image) {
    ScopedBitmap rendered(FPDFImageObj_GetRenderedBitmap(document, page, image));
    if (rendered.get() == nullptr) return true;
    const int width = FPDFBitmap_GetWidth(rendered.get());
    const int height = FPDFBitmap_GetHeight(rendered.get());
    const int stride = FPDFBitmap_GetStride(rendered.get());
    const auto* buffer = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(rendered.get()));
    if (buffer == nullptr || width <= 0 || height <= 0) return true;
    if (FPDFBitmap_GetFormat(rendered.get()) != FPDFBitmap_BGRA) {
        return false; // no alpha channel at all
    }
    const int margin = (width > 4 && height > 4) ? 1 : 0;
    for (int y = margin; y < height - margin; ++y) {
        const std::uint8_t* row = buffer + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = margin; x < width - margin; ++x) {
            if (row[x * 4 + 3] != 0xFF) return true;
        }
    }
    return false;
}

struct Pixels {
    std::vector<std::uint8_t> data; // tightly packed
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 0; // 1 or 3
};

// The decoded image as tightly packed gray or RGB; nullopt when it cannot be
// read faithfully (alpha channel, unknown format, inconsistent buffer).
std::optional<Pixels> readPixels(FPDF_PAGEOBJECT image) {
    ScopedBitmap bitmap(FPDFImageObj_GetBitmap(image));
    if (bitmap.get() == nullptr) return std::nullopt;
    const int width = FPDFBitmap_GetWidth(bitmap.get());
    const int height = FPDFBitmap_GetHeight(bitmap.get());
    const int stride = FPDFBitmap_GetStride(bitmap.get());
    const auto* buffer = static_cast<const std::uint8_t*>(FPDFBitmap_GetBuffer(bitmap.get()));
    int bytesPerPixel = 0;
    switch (FPDFBitmap_GetFormat(bitmap.get())) {
    case FPDFBitmap_Gray: bytesPerPixel = 1; break;
    case FPDFBitmap_BGR: bytesPerPixel = 3; break;
    case FPDFBitmap_BGRx:
    case FPDFBitmap_BGRA: bytesPerPixel = 4; break;
    default: return std::nullopt;
    }
    if (width <= 0 || height <= 0 || buffer == nullptr || stride < width * bytesPerPixel ||
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) > kMaxImagePixels) {
        return std::nullopt;
    }
    Pixels pixels;
    pixels.width = static_cast<std::uint32_t>(width);
    pixels.height = static_cast<std::uint32_t>(height);
    pixels.channels = bytesPerPixel == 1 ? 1u : 3u;
    pixels.data.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * pixels.channels);
    const bool hasAlpha = FPDFBitmap_GetFormat(bitmap.get()) == FPDFBitmap_BGRA;
    std::size_t out = 0;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = buffer + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * bytesPerPixel;
            if (bytesPerPixel == 1) {
                pixels.data[out++] = pixel[0];
                continue;
            }
            if (hasAlpha && pixel[3] != 0xFF) return std::nullopt;
            pixels.data[out++] = pixel[2];
            pixels.data[out++] = pixel[1];
            pixels.data[out++] = pixel[0];
        }
    }
    return pixels;
}

enum class Outcome : std::uint8_t { Skipped, Replaced };

core::Result<Outcome> reduceOne(FPDF_DOCUMENT document, FPDF_PAGE page, FPDF_PAGEOBJECT image,
                                const std::vector<FPDF_PAGEOBJECT>& images, const PdfImageReduction& options,
                                bool& pageChecked, bool& pageSafe,
                                const std::function<bool()>& regenerationSafe) {
    unsigned int width = 0;
    unsigned int height = 0;
    if (FPDFImageObj_GetImagePixelSize(image, &width, &height) == 0 || width > kMaxImageSide ||
        height > kMaxImageSide || static_cast<std::uint64_t>(width) * height > kMaxImagePixels) {
        return Outcome::Skipped;
    }
    FS_MATRIX matrix{};
    if (FPDFPageObj_GetMatrix(image, &matrix) == 0) return Outcome::Skipped;
    const auto plan = planReduction(width, height, static_cast<double>(std::hypot(matrix.a, matrix.b)), static_cast<double>(std::hypot(matrix.c, matrix.d)),
                                    options.targetDpi);
    if (!plan.has_value()) return Outcome::Skipped;
    const bool jpeg = isJpeg(image);
    if (jpeg && !plan->resample) return Outcome::Skipped; // already compact enough

    FPDF_IMAGEOBJ_METADATA metadata{};
    if (FPDFImageObj_GetImageMetadata(image, page, &metadata) == 0 || metadata.bits_per_pixel == 1) {
        return Outcome::Skipped;
    }
    if (sharesImage(images, image) || hasTransparency(document, page, image)) return Outcome::Skipped;

    auto pixels = readPixels(image);
    if (!pixels.has_value()) return Outcome::Skipped;
    std::span<const std::uint8_t> source(pixels->data);
    std::vector<std::uint8_t> reduced;
    std::uint32_t outWidth = pixels->width;
    std::uint32_t outHeight = pixels->height;
    if (plan->resample) {
        reduced = boxDownsample(source, pixels->width, pixels->height, pixels->channels,
                                static_cast<std::size_t>(pixels->width) * pixels->channels, plan->width,
                                plan->height);
        if (reduced.empty()) return Outcome::Skipped;
        source = reduced;
        outWidth = plan->width;
        outHeight = plan->height;
    }
    auto encoded = options.encoder->encode(source, outWidth, outHeight, pixels->channels, options.jpegQuality);
    if (!encoded.has_value() || encoded->empty()) return Outcome::Skipped;

    const std::size_t stored = rawData(image).size();
    const std::size_t allowed = (plan->resample || jpeg) ? stored : stored / 4 * 3;
    if (stored == 0 || encoded->size() >= allowed) return Outcome::Skipped;

    if (!pageChecked) {
        pageChecked = true;
        pageSafe = !regenerationSafe || regenerationSafe();
    }
    if (!pageSafe) return Outcome::Skipped;

    JpegSource jpegSource{encoded->data(), encoded->size()};
    FPDF_FILEACCESS access{};
    access.m_FileLen = static_cast<unsigned long>(encoded->size());
    access.m_GetBlock = &readJpegBlock;
    access.m_Param = &jpegSource;
    FPDF_PAGE pages[1] = {page};
    if (FPDFImageObj_LoadJpegFileInline(pages, 1, image, &access) == 0) {
        return std::unexpected(internalError("PDFium could not embed a reduced image"));
    }
    if (FPDFPageObj_SetMatrix(image, &matrix) == 0) {
        return std::unexpected(internalError("PDFium could not restore an image placement"));
    }
    return Outcome::Replaced;
}

} // namespace

core::Result<std::size_t> reduceImagesOnPage(FPDF_DOCUMENT document, FPDF_PAGE page,
                                             const PdfImageReduction& options,
                                             const std::function<bool()>& regenerationSafe) {
    if (options.encoder == nullptr) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                               "image reduction needs a JPEG encoder", "pdf"));
    }
    const int count = FPDFPage_CountObjects(page);
    if (count < 0 || static_cast<std::size_t>(count) > kMaxContentObjectsPerPage) return std::size_t{0};
    std::vector<FPDF_PAGEOBJECT> images;
    for (int i = 0; i < count; ++i) {
        const FPDF_PAGEOBJECT object = FPDFPage_GetObject(page, i);
        if (object != nullptr && FPDFPageObj_GetType(object) == FPDF_PAGEOBJ_IMAGE) images.push_back(object);
    }
    std::size_t replaced = 0;
    bool pageChecked = false;
    bool pageSafe = true;
    for (const FPDF_PAGEOBJECT image : images) {
        auto outcome = reduceOne(document, page, image, images, options, pageChecked, pageSafe, regenerationSafe);
        if (!outcome.has_value()) return std::unexpected(outcome.error());
        if (*outcome == Outcome::Replaced) ++replaced;
        if (pageChecked && !pageSafe) break;
    }
    if (replaced > 0 && FPDFPage_GenerateContent(page) == 0) {
        return std::unexpected(internalError("PDFium could not regenerate the page content"));
    }
    return replaced;
}

} // namespace rivet::pdf::internal
