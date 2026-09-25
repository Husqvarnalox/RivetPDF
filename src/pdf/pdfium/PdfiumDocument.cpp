#include "PdfiumDocument.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "PdfiumCallGate.hpp"
#include "PdfiumFileSource.h"
#include "PdfiumTextPage.h"
#include "core/geometry/Matrix.hpp"
#include "fpdf_doc.h"  // FPDF_GetMetaText
#include "PdfiumDisplayTransform.h"
#include "fpdf_doc.h"    // bookmarks, dests, actions, links, page labels
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
PdfiumDocument::PdfiumDocument(FPDF_DOCUMENT document,
                               bool isEncrypted,
                               std::shared_ptr<PdfiumFileSource> source,
                               std::string password,
                               const PdfiumEngine* owner)
    : document_(document), source_(std::move(source)), password_(std::move(password)), owner_(owner) {
    info_.isEncrypted = isEncrypted;
    info_.pageCount = static_cast<std::size_t>(std::max(0, FPDF_GetPageCount(document_)));
    info_.title = metaText("Title");
}

PdfiumDocument::~PdfiumDocument() {
    // Best-effort wipe of the retained password (volatile writes are not
    // elided as dead stores).
    volatile char* secret = password_.data();
    for (std::size_t i = 0; i < password_.size(); ++i) {
        secret[i] = '\0';
    }

    if (document_ == nullptr) {
        return;
    }
    const FPDF_DOCUMENT handle = document_;
    document_ = nullptr;
    // Public entry operation: one gate acquisition for the close. The gate is
    // a leaked singleton, so this remains valid during static teardown. The
    // file source (a member) is released only after this body, i.e. after
    // PDFium is done with it.
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

        // The native view: effective /Rotate plus the effective crop box
        // (FPDF_GetPageBoundingBox = CropBox ∩ MediaBox). Its display size
        // is exactly what FPDF_GetPageWidthF/FPDF_GetPageHeightF report
        // (both derive from CPDF_Page's bbox and rotation), i.e. the
        // displayed, rotation-aware page size.
        const std::optional<PdfPageView> view = internal::nativePageView(page.get());
        if (!view.has_value()) {
            // Degenerate page geometry: report what PDFium reports (as before
            // views existed) with zero-origin boxes; rendering such a page
            // fails with InvalidDocument later.
            return PdfPageInfo(pageIndex,
                               core::Size{static_cast<double>(FPDF_GetPageWidthF(page.get())),
                                          static_cast<double>(FPDF_GetPageHeightF(page.get()))},
                               core::rotationFromQuarterTurns(FPDFPage_GetRotation(page.get())));
        }

        PdfPageInfo result;
        result.index = pageIndex;
        result.view = *view;
        result.mediaBox = internal::pageMediaBox(page.get(), *view);
        result.sizePoints = displaySize(*view);
        result.rotation = view->rotation;
        return result;
    });
}

core::Result<core::Bitmap> PdfiumDocument::renderPage(std::size_t pageIndex,
                                                      const core::Rect& pageRectPoints,
                                                      double devicePixelsPerPoint) {
    return renderPageImpl(pageIndex, nullptr, pageRectPoints, devicePixelsPerPoint);
}

core::Result<core::Bitmap> PdfiumDocument::renderPageInView(std::size_t pageIndex,
                                                            const PdfPageView& view,
                                                            const core::Rect& pageRectPoints,
                                                            double devicePixelsPerPoint) {
    return renderPageImpl(pageIndex, &view, pageRectPoints, devicePixelsPerPoint);
}

core::Result<core::Bitmap> PdfiumDocument::renderPageImpl(std::size_t pageIndex,
                                                          const PdfPageView* view,
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

        // Native geometry plus the requested view's (validated: quarter turn,
        // crop box within the media box).
        const auto geometry = internal::resolvePageGeometry(page.get(), pageIndex, view);
        if (!geometry.has_value()) {
            return std::unexpected(geometry.error());
        }
        const double pageWidth = geometry->display.displaySize.width;
        const double pageHeight = geometry->display.displaySize.height;

        // pageRectPoints is in displayed-page coordinates of the VIEW
        // (top-left origin, y-down) and must be contained in its bounds.
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
        //
        // A non-native VIEW: pageRectPoints is in the view's display space,
        // but PDFium always applies the page's NATIVE display matrix first.
        // The caller matrix therefore undoes it and applies the view's
        // instead (core::Matrix composition: rightmost applies first):
        //     caller = tile * viewDisplay * inverse(nativeDisplay)
        // i.e. native display -> user space -> view display -> bitmap. All
        // three are exact quarter-turn/translation matrices, composed in
        // doubles and validated before the float conversion. For the native
        // view the middle terms cancel and the plain tile matrix is used
        // (bit-identical to the pre-view code path). Clipping is purely the
        // bitmap clip rect (CPDFSDK_RenderPage sets it as the device clip;
        // no crop-box clip is applied), and the bitmap covers only the
        // requested tile of the view, so content outside the view's crop box
        // never lands in it - while content outside the NATIVE crop box but
        // inside the view (still within the media box) does render.
        const core::Matrix tile =
            core::Matrix::scaling(devicePixelsPerPoint, devicePixelsPerPoint) *
            core::Matrix::translation(-pageRectPoints.minX(), -pageRectPoints.minY());
        core::Matrix caller = tile;
        if (geometry->display.view != geometry->nativeView) {
            const std::optional<core::Matrix> nativeInverse =
                userToDisplayMatrix(geometry->nativeView).inverted();
            if (!nativeInverse.has_value()) {
                return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                                       "page " + std::to_string(pageIndex) +
                                                           " has a singular display matrix",
                                                       "pdf"));
            }
            caller = tile * userToDisplayMatrix(geometry->display.view) * *nativeInverse;
        }
        const double coefficients[] = {caller.a, caller.b, caller.c, caller.d, caller.tx, caller.ty};
        for (const double value : coefficients) {
            if (!std::isfinite(value) || std::fabs(value) > 1e9) {
                // Possible only for pathological page geometry (origin far
                // outside any real page); PDFium consumes the matrix as
                // floats.
                return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                       "page rectangle is too far from the page origin",
                                                       "pdf"));
            }
        }

        FS_MATRIX matrix{};
        matrix.a = static_cast<float>(caller.a);
        matrix.b = static_cast<float>(caller.b);
        matrix.c = static_cast<float>(caller.c);
        matrix.d = static_cast<float>(caller.d);
        matrix.e = static_cast<float>(caller.tx);
        matrix.f = static_cast<float>(caller.ty);

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

core::Result<std::shared_ptr<const PdfTextPage>> PdfiumDocument::textPage(std::size_t pageIndex) const {
    return textPageImpl(pageIndex, nullptr);
}

core::Result<std::shared_ptr<const PdfTextPage>> PdfiumDocument::textPageInView(std::size_t pageIndex,
                                                                                const PdfPageView& view) const {
    return textPageImpl(pageIndex, &view);
}

core::Result<std::shared_ptr<const PdfTextPage>> PdfiumDocument::textPageImpl(std::size_t pageIndex,
                                                                              const PdfPageView* view) const {
    // Public entry operation: ONE gate acquisition for the whole extraction.
    // extractTextPage never acquires the gate itself (documented in
    // PdfiumTextPage.h) - the page load, text-page load, per-char queries and
    // the RAII handle closes all run inside this single acquisition.
    return globalPdfiumCallGate().invoke(
        [&]() -> core::Result<std::shared_ptr<const PdfTextPage>> {
            if (pageIndex >= info_.pageCount) {
                return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                       "page index " + std::to_string(pageIndex) +
                                                           " out of range (document has " +
                                                           std::to_string(info_.pageCount) + " pages)",
                                                       "pdf"));
            }
            return extractTextPage(document_, pageIndex, info_.pageCount, view);
        });
}


namespace {

// Title of one bookmark (UTF-16LE per FPDFBookmark_GetTitle). Caller holds
// the gate. Empty on failure.
std::string metaTextFromBookmark(FPDF_BOOKMARK bookmark) {
    if (bookmark == nullptr) return {};
    const unsigned long needed = FPDFBookmark_GetTitle(bookmark, nullptr, 0);
    if (needed < 2 || needed > kMaxMetaTextBytes) return {};
    std::vector<std::uint8_t> bytes(needed);
    const unsigned long written = FPDFBookmark_GetTitle(bookmark, bytes.data(), needed);
    if (written < 2) return {};
    const std::size_t byteCount = std::min<std::size_t>(written, needed) & ~std::size_t{1};
    return utf16leToUtf8(bytes.data(), byteCount);
}

// Safety limits for untrusted outline trees (see ARCHITECTURE.md section 9).
constexpr int kMaxOutlineDepth = 32;
constexpr std::size_t kMaxOutlineNodes = 10000;

using VisitedBookmarks = std::unordered_set<FPDF_BOOKMARK>;

// Node budget shared across the whole walk: a pointer so recursion can
// exhaust it. When it runs out, the enclosing node is marked truncated.
struct OutlineBudget {
    std::size_t nodesLeft = kMaxOutlineNodes;
};

// Extracts the destination of a bookmark/action. Caller holds the gate.
// Returns nullopt when the destination cannot be resolved (no dest, bad
// page index) - the node survives without a destination.
std::optional<PdfDestination> resolveDest(FPDF_DOCUMENT document, FPDF_DEST dest) {
    if (dest == nullptr) return std::nullopt;
    const int pageIndex = FPDFDest_GetDestPageIndex(document, dest);
    if (pageIndex < 0) return std::nullopt;

    PdfDestination result;
    result.pageIndex = static_cast<std::size_t>(pageIndex);

    FPDF_BOOL hasX = 0;
    FPDF_BOOL hasY = 0;
    FPDF_BOOL hasZoom = 0;
    FS_FLOAT x = 0.0f;
    FS_FLOAT y = 0.0f;
    FS_FLOAT zoom = 0.0f;
    if (FPDFDest_GetLocationInPage(dest, &hasX, &hasY, &hasZoom, &x, &y, &zoom) != 0 && hasX != 0 &&
        hasY != 0) {
        // The user-space point needs no page: it is what the file says, and
        // lets a consumer re-map it into any view of the target page.
        result.hasUserPoint = true;
        result.userX = static_cast<double>(x);
        result.userY = static_cast<double>(y);
        // The display point needs the destination PAGE's native display
        // geometry (a dest may reference a different page than the one being
        // processed).
        internal::ScopedPage targetPage(FPDF_LoadPage(document, pageIndex));
        if (targetPage.get() != nullptr) {
            if (const auto geometry = internal::makeDisplayGeometry(targetPage.get());
                geometry.has_value()) {
                result.point = internal::userToDisplay(*geometry, static_cast<double>(x),
                                                       static_cast<double>(y));
                result.hasPoint = true;
            }
        }
    }

    unsigned long numParams = 0;
    FS_FLOAT params[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const unsigned long view = FPDFDest_GetView(dest, &numParams, params);
    switch (view) {
        case PDFDEST_VIEW_XYZ: result.fit = PdfDestination::Fit::XYZ; break;
        case PDFDEST_VIEW_FIT: result.fit = PdfDestination::Fit::Fit; break;
        case PDFDEST_VIEW_FITH: result.fit = PdfDestination::Fit::FitH; break;
        case PDFDEST_VIEW_FITV: result.fit = PdfDestination::Fit::FitV; break;
        case PDFDEST_VIEW_FITR: result.fit = PdfDestination::Fit::FitR; break;
        case PDFDEST_VIEW_FITB: result.fit = PdfDestination::Fit::FitB; break;
        case PDFDEST_VIEW_FITBH: result.fit = PdfDestination::Fit::FitBH; break;
        case PDFDEST_VIEW_FITBV: result.fit = PdfDestination::Fit::FitBV; break;
        default: result.fit = PdfDestination::Fit::Unknown; break;
    }
    return result;
}

// Destination of one bookmark: its GOTO action wins over the bare dest.
std::optional<PdfDestination> bookmarkDestination(FPDF_DOCUMENT document, FPDF_BOOKMARK bookmark) {
    const FPDF_ACTION action = FPDFBookmark_GetAction(bookmark);
    if (action != nullptr &&
        FPDFAction_GetType(action) == static_cast<unsigned long>(PDFACTION_GOTO)) {
        return resolveDest(document, FPDFAction_GetDest(document, action));
    }
    return resolveDest(document, FPDFBookmark_GetDest(document, bookmark));
}

// Recursive outline walk. Caller holds the gate. visitedPath guards against
// circular trees: a bookmark encountered twice on one descent stops that
// branch (its children are not expanded).
std::vector<PdfOutlineNode> collectOutlineLevel(FPDF_DOCUMENT document, FPDF_BOOKMARK first,
                                                int depth, VisitedBookmarks& visitedPath,
                                                OutlineBudget& budget) {
    std::vector<PdfOutlineNode> nodes;
    FPDF_BOOKMARK bookmark = first;
    while (bookmark != nullptr) {
        if (budget.nodesLeft == 0) {
            if (!nodes.empty()) nodes.back().truncated = true;
            break;
        }
        // Cycle guard: a handle seen on the active path must not recurse.
        if (visitedPath.count(bookmark) != 0) break;

        PdfOutlineNode node;
        node.title = metaTextFromBookmark(bookmark);
        node.destination = bookmarkDestination(document, bookmark);

        visitedPath.insert(bookmark);
        if (depth + 1 < kMaxOutlineDepth && budget.nodesLeft > 0) {
            node.children = collectOutlineLevel(document, FPDFBookmark_GetFirstChild(document, bookmark),
                                                depth + 1, visitedPath, budget);
        } else if (FPDFBookmark_GetFirstChild(document, bookmark) != nullptr) {
            node.truncated = true;
        }
        visitedPath.erase(bookmark);

        --budget.nodesLeft;
        nodes.push_back(std::move(node));
        bookmark = FPDFBookmark_GetNextSibling(document, bookmark);
    }
    return nodes;
}

} // namespace

core::Result<std::optional<PdfOutlineNode>> PdfiumDocument::outline() const {
    // Public entry operation: one gate acquisition for the whole walk.
    return globalPdfiumCallGate().invoke([&]() -> core::Result<std::optional<PdfOutlineNode>> {
        VisitedBookmarks visitedPath;
        OutlineBudget budget;
        std::vector<PdfOutlineNode> top =
            collectOutlineLevel(document_, FPDFBookmark_GetFirstChild(document_, nullptr), 0,
                                visitedPath, budget);
        if (top.empty()) return std::optional<PdfOutlineNode>{};
        PdfOutlineNode root;
        root.children = std::move(top);
        return std::optional<PdfOutlineNode>(std::move(root));
    });
}

core::Result<std::string> PdfiumDocument::pageLabel(std::size_t pageIndex) const {
    // Public entry operation: one gate acquisition.
    return globalPdfiumCallGate().invoke([&]() -> core::Result<std::string> {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "page index " + std::to_string(pageIndex) +
                                                       " out of range (document has " +
                                                       std::to_string(info_.pageCount) + " pages)",
                                                   "pdf"));
        }
        const unsigned long needed = FPDF_GetPageLabel(document_, static_cast<int>(pageIndex),
                                                       nullptr, 0);
        if (needed < 2 || needed > kMaxMetaTextBytes) {
            return std::string{}; // no label (or absurd): fall back to numbers
        }
        std::vector<std::uint8_t> bytes(needed);
        const unsigned long written = FPDF_GetPageLabel(document_, static_cast<int>(pageIndex),
                                                        bytes.data(), needed);
        if (written < 2) return std::string{};
        const std::size_t byteCount = std::min<std::size_t>(written, needed) & ~std::size_t{1};
        return utf16leToUtf8(bytes.data(), byteCount);
    });
}

namespace {

// Rects of one link: quad-point boxes (display space), annotation-rect
// fallback. Caller holds the gate.
std::vector<core::Rect> linkRects(FPDF_LINK link, const internal::DisplayGeometry& geometry) {
    std::vector<core::Rect> rects;
    const int quadCount = FPDFLink_CountQuadPoints(link);
    bool haveQuads = false;
    for (int q = 0; q < quadCount; ++q) {
        FS_QUADPOINTSF quad{};
        if (FPDFLink_GetQuadPoints(link, q, &quad) == 0) continue;
        // Two opposite corners of the quad's bounding box (quads may skew;
        // the axis-aligned bound is what the viewer can highlight).
        const core::Point a = internal::userToDisplay(geometry, static_cast<double>(quad.x1),
                                                      static_cast<double>(quad.y1));
        const core::Point b = internal::userToDisplay(geometry, static_cast<double>(quad.x3),
                                                      static_cast<double>(quad.y3));
        const core::Point c = internal::userToDisplay(geometry, static_cast<double>(quad.x2),
                                                      static_cast<double>(quad.y2));
        const core::Point d = internal::userToDisplay(geometry, static_cast<double>(quad.x4),
                                                      static_cast<double>(quad.y4));
        const double minX = std::min({a.x, b.x, c.x, d.x});
        const double maxX = std::max({a.x, b.x, c.x, d.x});
        const double minY = std::min({a.y, b.y, c.y, d.y});
        const double maxY = std::max({a.y, b.y, c.y, d.y});
        rects.push_back(core::Rect{core::Point{minX, minY}, core::Size{maxX - minX, maxY - minY}});
        haveQuads = true;
    }
    if (!haveQuads) {
        FS_RECTF annot{};
        if (FPDFLink_GetAnnotRect(link, &annot) != 0) {
            rects.push_back(internal::userBoxToDisplayRect(geometry, static_cast<double>(annot.left),
                                                           static_cast<double>(annot.right),
                                                           static_cast<double>(annot.bottom),
                                                           static_cast<double>(annot.top)));
        }
    }
    return rects;
}

} // namespace

core::Result<std::vector<PdfPageLink>> PdfiumDocument::pageLinks(std::size_t pageIndex) const {
    return pageLinksImpl(pageIndex, nullptr);
}

core::Result<std::vector<PdfPageLink>> PdfiumDocument::pageLinksInView(std::size_t pageIndex,
                                                                       const PdfPageView& view) const {
    return pageLinksImpl(pageIndex, &view);
}

core::Result<std::vector<PdfPageLink>> PdfiumDocument::pageLinksImpl(std::size_t pageIndex,
                                                                     const PdfPageView* view) const {
    // Public entry operation: one gate acquisition.
    return globalPdfiumCallGate().invoke([&]() -> core::Result<std::vector<PdfPageLink>> {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "page index " + std::to_string(pageIndex) +
                                                       " out of range (document has " +
                                                       std::to_string(info_.pageCount) + " pages)",
                                                   "pdf"));
        }
        internal::ScopedPage page(FPDF_LoadPage(document_, static_cast<int>(pageIndex)));
        if (page.get() == nullptr) {
            const int lastError = static_cast<int>(FPDF_GetLastError());
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                                   "PDFium failed to load page " +
                                                       std::to_string(pageIndex) +
                                                       " (FPDF error " + std::to_string(lastError) + ")",
                                                   "pdf"));
        }
        // Link rects are reported in the requested view's display space.
        const auto resolved = internal::resolvePageGeometry(page.get(), pageIndex, view);
        if (!resolved.has_value()) {
            return std::unexpected(resolved.error());
        }
        const internal::DisplayGeometry* geometry = &resolved->display;

        std::vector<PdfPageLink> links;
        int startPos = 0;
        FPDF_LINK link = nullptr;
        while (FPDFLink_Enumerate(page.get(), &startPos, &link) != 0) {
            PdfPageLink result;

            const FPDF_ACTION action = FPDFLink_GetAction(link);
            const unsigned long actionType =
                action != nullptr ? FPDFAction_GetType(action)
                                  : static_cast<unsigned long>(PDFACTION_UNSUPPORTED);
            // A bare /Dest (no /A action) is a plain internal link.
            FPDF_DEST bareDest = nullptr;
            if (actionType == static_cast<unsigned long>(PDFACTION_UNSUPPORTED)) {
                bareDest = FPDFLink_GetDest(document_, link);
            }
            if (actionType == static_cast<unsigned long>(PDFACTION_GOTO) || bareDest != nullptr) {
                result.kind = PdfPageLink::Kind::Internal;
                result.destination =
                    resolveDest(document_,
                                bareDest != nullptr ? bareDest
                                                    : FPDFAction_GetDest(document_, action))
                        .value_or(PdfDestination{});
            } else if (actionType == static_cast<unsigned long>(PDFACTION_URI)) {
                result.kind = PdfPageLink::Kind::External;
                const unsigned long needed = FPDFAction_GetURIPath(document_, action, nullptr, 0);
                if (needed > 0) {
                    std::string url(needed, '\0');
                    const unsigned long written =
                        FPDFAction_GetURIPath(document_, action, url.data(),
                                              static_cast<unsigned long>(url.size()));
                    if (written > 0 && written <= needed) {
                        // The URI is raw bytes terminated by a NUL.
                        url.resize(std::min<std::size_t>(written, needed) - 1);
                        result.url = std::move(url);
                    } else {
                        result.kind = PdfPageLink::Kind::Other;
                    }
                } else {
                    result.kind = PdfPageLink::Kind::Other;
                }
            } else {
                result.kind = PdfPageLink::Kind::Other;
            }

            result.rects = linkRects(link, *geometry);
            links.push_back(std::move(result));
        }
        return links;
    });
}

} // namespace rivet::pdf
