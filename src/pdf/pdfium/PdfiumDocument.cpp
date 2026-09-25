#include "PdfiumDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "PdfiumCallGate.hpp"
#include "fpdf_doc.h"  // FPDF_GetMetaText
#include "fpdf_edit.h" // FPDFPage_GetRotation

namespace rivet::pdf {
namespace {

// RAII wrapper for FPDF_PAGE handles (FPDF_LoadPage / FPDF_ClosePage).
class ScopedPage {
public:
    explicit ScopedPage(FPDF_PAGE page) : page_(page) {}
    ~ScopedPage() {
        if (page_ != nullptr) {
            FPDF_ClosePage(page_);
        }
    }
    ScopedPage(const ScopedPage&) = delete;
    ScopedPage& operator=(const ScopedPage&) = delete;

    FPDF_PAGE get() const { return page_; }

private:
    FPDF_PAGE page_;
};

// RAII wrapper for FPDF_BITMAP handles (FPDFBitmap_CreateEx with an external
// buffer / FPDFBitmap_Destroy). The pixel buffer itself is owned by a
// core::Bitmap and is NOT freed by FPDFBitmap_Destroy: CreateEx with a
// non-null first_scan makes PDFium wrap the caller's buffer.
class ScopedBitmap {
public:
    explicit ScopedBitmap(FPDF_BITMAP bitmap) : bitmap_(bitmap) {}
    ~ScopedBitmap() {
        if (bitmap_ != nullptr) {
            FPDFBitmap_Destroy(bitmap_);
        }
    }
    ScopedBitmap(const ScopedBitmap&) = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;

    FPDF_BITMAP get() const { return bitmap_; }

private:
    FPDF_BITMAP bitmap_;
};

// Upper bound for a single metadata string (UTF-16LE bytes, including the
// terminator). Titles are short; anything larger is treated as missing.
constexpr std::size_t kMaxMetaTextBytes = 1u << 16;

// Upper bound for devicePixelsPerPoint. A legitimate physical scale is the
// quantized zoom (up to 64x) times the display backing scale (up to 8x on
// high-DPI outputs), i.e. up to 512; larger values are caller bugs. The real
// safety bound is core::kMaxBitmapDimension, checked below against the scaled
// tile size.
constexpr double kMaxRenderScale = 512.0;

// Tolerance when checking that the requested tile lies inside the page: page
// dimensions come back as floats and callers compute from those values.
constexpr double kContainmentEpsilon = 1e-4;

// Converts UTF-16LE bytes (as returned by FPDF_GetMetaText) to UTF-8.
// Stops at the terminating NUL code unit, skips a leading BOM, decodes
// surrogate pairs and emits U+FFFD for malformed sequences. No <codecvt>.
std::string utf16leToUtf8(const std::uint8_t* bytes, std::size_t byteLength) {
    std::string out;
    out.reserve(byteLength); // usually an over-estimate; avoids reallocation

    const std::size_t units = byteLength / 2;
    auto unitAt = [&](std::size_t index) -> std::uint16_t {
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[index * 2]) |
                                          (static_cast<std::uint16_t>(bytes[index * 2 + 1]) << 8));
    };

    auto appendCodePoint = [&out](std::uint32_t codePoint) {
        if (codePoint <= 0x7Fu) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else if (codePoint <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
        }
    };

    std::size_t i = 0;
    if (units > 0 && unitAt(0) == 0xFEFFu) {
        i = 1; // byte-order mark
    }

    while (i < units) {
        const std::uint16_t unit = unitAt(i);
        if (unit == 0) {
            break; // terminator
        }
        if (unit >= 0xD800u && unit <= 0xDBFFu) { // high surrogate
            if (i + 1 < units && unitAt(i + 1) >= 0xDC00u && unitAt(i + 1) <= 0xDFFFu) {
                appendCodePoint(0x10000u + ((static_cast<std::uint32_t>(unit) - 0xD800u) << 10) +
                                (static_cast<std::uint32_t>(unitAt(i + 1)) - 0xDC00u));
                i += 2;
            } else {
                appendCodePoint(0xFFFDu); // unpaired high surrogate
                i += 1;
            }
            continue;
        }
        if (unit >= 0xDC00u && unit <= 0xDFFFu) { // unpaired low surrogate
            appendCodePoint(0xFFFDu);
            i += 1;
            continue;
        }
        appendCodePoint(unit);
        i += 1;
    }
    return out;
}

} // namespace

// Caller must hold the PDFium gate: this runs FPDF_GetPageCount and
// FPDF_GetMetaText. It is the tail of PdfiumEngine::openDocument's single
// gate acquisition and must not acquire itself.
PdfiumDocument::PdfiumDocument(FPDF_DOCUMENT document, bool isEncrypted) : document_(document) {
    info_.isEncrypted = isEncrypted;
    info_.pageCount = static_cast<std::size_t>(std::max(0, FPDF_GetPageCount(document_)));
    info_.title = metaText("Title");
}

PdfiumDocument::~PdfiumDocument() {
    if (document_ == nullptr) {
        return;
    }
    const FPDF_DOCUMENT handle = document_;
    document_ = nullptr;
    // Public entry operation: one gate acquisition for the close. The gate is
    // a leaked singleton, so this remains valid during static teardown.
    globalPdfiumCallGate().invoke([handle] { FPDF_CloseDocument(handle); });
}

const PdfDocumentInfo& PdfiumDocument::info() const {
    return info_;
}

std::string PdfiumDocument::metaText(const char* tag) const {
    // Caller must hold the PDFium gate; this helper never acquires it (see
    // PdfiumCallGate.hpp).
    //
    // FPDF_GetMetaText returns the required buffer size in bytes (UTF-16LE,
    // including the NUL terminator) when buffer is null.
    const unsigned long needed = FPDF_GetMetaText(document_, tag, nullptr, 0);
    if (needed < 2 || needed > kMaxMetaTextBytes) {
        return {};
    }

    std::vector<std::uint8_t> bytes(needed);
    const unsigned long written = FPDF_GetMetaText(document_, tag, bytes.data(), needed);
    if (written < 2) {
        return {};
    }

    // Round down to a whole number of UTF-16 code units before decoding.
    const std::size_t byteCount = std::min<std::size_t>(written, needed) & ~std::size_t{1};
    return utf16leToUtf8(bytes.data(), byteCount);
}

core::Result<PdfPageInfo> PdfiumDocument::pageInfo(std::size_t pageIndex) const {
    // Public entry operation: one gate acquisition for the whole body
    // (FPDF_LoadPage, dimension queries, FPDFPage_GetRotation all race with
    // any other FPDF_* call).
    return globalPdfiumCallGate().invoke([&]() -> core::Result<PdfPageInfo> {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "page index " + std::to_string(pageIndex) +
                                                       " out of range (document has " +
                                                       std::to_string(info_.pageCount) + " pages)",
                                                   "pdf"));
        }

        ScopedPage page(FPDF_LoadPage(document_, static_cast<int>(pageIndex)));
        if (page.get() == nullptr) {
            const int lastError = static_cast<int>(FPDF_GetLastError());
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                                   "PDFium failed to load page " +
                                                       std::to_string(pageIndex) +
                                                       " (FPDF error " + std::to_string(lastError) + ")",
                                                   "pdf"));
        }

        PdfPageInfo result;
        result.index = pageIndex;
        // FPDF_GetPageWidthF/FPDF_GetPageHeightF report the displayed page
        // size, i.e. already accounting for /Rotate ("Changing the rotation
        // of |page| affects the return value", fpdfview.h).
        result.sizePoints = core::Size{static_cast<double>(FPDF_GetPageWidthF(page.get())),
                                       static_cast<double>(FPDF_GetPageHeightF(page.get()))};
        result.rotation = core::rotationFromQuarterTurns(FPDFPage_GetRotation(page.get()));
        return result;
    });
}

core::Result<core::Bitmap> PdfiumDocument::renderPage(std::size_t pageIndex,
                                                      const core::Rect& pageRectPoints,
                                                      double devicePixelsPerPoint) {
    // Public entry operation: one gate acquisition for the whole body. Every
    // step below (page load, dimension queries, bitmap fill, render) issues
    // FPDF_* calls that must not overlap with any other PDFium call.
    return globalPdfiumCallGate().invoke([&]() -> core::Result<core::Bitmap> {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "page index " + std::to_string(pageIndex) +
                                                       " out of range (document has " +
                                                       std::to_string(info_.pageCount) + " pages)",
                                                   "pdf"));
        }
        if (!std::isfinite(devicePixelsPerPoint) || devicePixelsPerPoint <= 0.0 ||
            devicePixelsPerPoint > kMaxRenderScale) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "device pixels per point must be finite and in (0, 512]",
                                                   "pdf"));
        }
        if (!pageRectPoints.isFinite() || pageRectPoints.isEmpty()) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "page rectangle must be finite and non-empty",
                                                   "pdf"));
        }

        ScopedPage page(FPDF_LoadPage(document_, static_cast<int>(pageIndex)));
        if (page.get() == nullptr) {
            const int lastError = static_cast<int>(FPDF_GetLastError());
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                                   "PDFium failed to load page " +
                                                       std::to_string(pageIndex) +
                                                       " (FPDF error " + std::to_string(lastError) + ")",
                                                   "pdf"));
        }

        const double pageWidth = static_cast<double>(FPDF_GetPageWidthF(page.get()));
        const double pageHeight = static_cast<double>(FPDF_GetPageHeightF(page.get()));
        if (!std::isfinite(pageWidth) || !std::isfinite(pageHeight) || pageWidth <= 0.0 ||
            pageHeight <= 0.0) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                                   "page " + std::to_string(pageIndex) +
                                                       " has invalid display dimensions",
                                                   "pdf"));
        }

        // pageRectPoints is in displayed-page coordinates (top-left origin,
        // y-down) and must be contained in the page bounds.
        if (pageRectPoints.minX() < -kContainmentEpsilon ||
            pageRectPoints.minY() < -kContainmentEpsilon ||
            pageRectPoints.maxX() > pageWidth + kContainmentEpsilon ||
            pageRectPoints.maxY() > pageHeight + kContainmentEpsilon) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "page rectangle exceeds the page bounds", "pdf"));
        }

        const double pixelWidth = std::ceil(pageRectPoints.size.width * devicePixelsPerPoint);
        const double pixelHeight = std::ceil(pageRectPoints.size.height * devicePixelsPerPoint);
        if (pixelWidth < 1.0 || pixelHeight < 1.0 ||
            pixelWidth > static_cast<double>(core::kMaxBitmapDimension) ||
            pixelHeight > static_cast<double>(core::kMaxBitmapDimension)) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "rendered tile size exceeds the supported maximum",
                                                   "pdf"));
        }

        auto created = core::Bitmap::create(static_cast<std::uint32_t>(pixelWidth),
                                            static_cast<std::uint32_t>(pixelHeight));
        if (!created.has_value()) {
            return std::unexpected(created.error());
        }
        core::Bitmap bitmap = std::move(created.value());

        // Format note: fpdfview.h documents FPDFBitmap_BGRA as straight
        // (non-premultiplied) alpha - "Pixel components are independent of
        // alpha" - which is exactly core::Bitmap's BGRA8888Straight contract.
        // The opaque background fill below leaves every pixel at alpha 255;
        // a future render mode producing semi-transparent page content can
        // hand straight-alpha pixels to the compositor without conversion.
        ScopedBitmap fpdfBitmap(FPDFBitmap_CreateEx(static_cast<int>(bitmap.width()),
                                                    static_cast<int>(bitmap.height()),
                                                    FPDFBitmap_BGRA, bitmap.data(),
                                                    static_cast<int>(bitmap.stride())));
        if (fpdfBitmap.get() == nullptr) {
            return std::unexpected(core::makeError(core::ErrorCode::OutOfMemory,
                                                   "PDFium could not create the rendering surface",
                                                   "pdf"));
        }

        // White background: PDFium only writes where content covers the
        // surface, so a transparent page region must come out white, not
        // zero-alpha. FPDFBitmap_FillRect takes ONE packed color in 8888 ARGB
        // form (0xAARRGGBB): 0xFFFFFFFFu is opaque white.
        if (FPDFBitmap_FillRect(fpdfBitmap.get(), 0, 0, static_cast<int>(bitmap.width()),
                                static_cast<int>(bitmap.height()), 0xFFFFFFFFu) == 0) {
            return std::unexpected(core::makeError(core::ErrorCode::Internal,
                                                   "PDFium could not initialize the rendering surface",
                                                   "pdf"));
        }

        // Build the matrix mapping displayed-page space to bitmap pixels.
        //
        // Composition model (verified against this PDFium revision's source):
        // FPDF_RenderPageBitmapWithMatrix computes
        //     transform = page->GetDisplayMatrix() * callerMatrix
        // (row-vector convention, fpdfsdk/fpdf_view.cpp around the
        // CFX_Matrix multiply in core/fxcrt/fx_coordinates.h), i.e. the
        // caller matrix is applied ON TOP of the page display matrix, which
        // already folds in /Rotate, the CropBox offset and the user-space
        // y-flip. The caller matrix therefore operates in DISPLAYED-PAGE
        // space (points, origin at the top-left of the displayed page,
        // y-down): a plain scale + tile offset, NO extra flip. FS_MATRIX
        // follows the CFX_Matrix convention:
        //     x' = a*x + c*y + e
        //     y' = b*x + d*y + f
        //
        // pageRectPoints is exactly such a displayed-page rectangle, so
        // mapping its top-left corner to bitmap pixel (0, 0) at
        // devicePixelsPerPoint puts the tile's top edge in row 0 and its
        // bottom edge in the last row, for tiles and full pages alike, with
        // displayed-page orientation preserved (a tile above another tile
        // also renders above it in the bitmap).
        const double matrixE = -pageRectPoints.minX() * devicePixelsPerPoint;
        const double matrixF = -pageRectPoints.minY() * devicePixelsPerPoint;
        if (!std::isfinite(matrixE) || !std::isfinite(matrixF) || std::fabs(matrixE) > 1e9 ||
            std::fabs(matrixF) > 1e9) {
            // Possible only for pathological page geometry (origin far
            // outside any real page); PDFium consumes the matrix as floats.
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "page rectangle is too far from the page origin",
                                                   "pdf"));
        }

        FS_MATRIX matrix{};
        matrix.a = static_cast<float>(devicePixelsPerPoint);
        matrix.b = 0.0f;
        matrix.c = 0.0f;
        matrix.d = static_cast<float>(devicePixelsPerPoint);
        matrix.e = static_cast<float>(matrixE);
        matrix.f = static_cast<float>(matrixF);

        // The clipping rect must be non-null: a null clip degenerates to the
        // empty device clip FX_RECT(0, 0, 0, 0) (CFX_FloatRect's zero default
        // via CFX_FloatRect::ToFxRect) and the call renders nothing at all,
        // for any matrix. Clip to the whole bitmap - the bitmap already
        // covers exactly the requested tile - using the embedder-test
        // convention {left, top, right, bottom} = {0, 0, w, h}.
        const FS_RECTF bitmapClip{0.0f, 0.0f, static_cast<float>(bitmap.width()),
                                  static_cast<float>(bitmap.height())};

        FPDF_RenderPageBitmapWithMatrix(fpdfBitmap.get(), page.get(), &matrix, &bitmapClip,
                                        FPDF_ANNOT | FPDF_LCD_TEXT);

        return bitmap;
    });
}

} // namespace rivet::pdf
