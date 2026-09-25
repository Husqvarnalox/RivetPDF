// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared PDFium-adapter internals: the page-view -> displayed-page geometry
// and the FPDF_PAGE RAII wrapper. INTERNAL to src/pdf/pdfium/ - the types
// here exist only so PdfiumTextPage.cpp, PdfiumDocument.cpp and
// PdfiumAssembly.cpp share one implementation of the transform. FPDF_* types
// must not leak past src/pdf/pdfium/.

#include "core/Error.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>

#include "fpdf_edit.h"          // FPDFPage_GetRotation
#include "fpdf_text.h"          // FPDFText_ClosePage
#include "fpdf_transformpage.h" // FPDFPage_GetMediaBox, FPDFPage_GetCropBox
#include "fpdfview.h"

namespace rivet::pdf::internal {

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

// RAII wrapper for FPDF_TEXTPAGE handles (FPDFText_LoadPage /
// FPDFText_ClosePage). The text page must be closed before the page it
// borrows, so declare it after the ScopedPage it is built from.
class ScopedTextPage {
public:
    explicit ScopedTextPage(FPDF_TEXTPAGE textPage) : textPage_(textPage) {}
    ~ScopedTextPage() {
        if (textPage_ != nullptr) {
            FPDFText_ClosePage(textPage_);
        }
    }
    ScopedTextPage(const ScopedTextPage&) = delete;
    ScopedTextPage& operator=(const ScopedTextPage&) = delete;

    FPDF_TEXTPAGE get() const { return textPage_; }

private:
    FPDF_TEXTPAGE textPage_;
};

// Tolerance for box comparisons: PDFium stores and reports boxes as floats.
constexpr double kBoxEpsilon = 1e-3;

// The user-space -> displayed-page geometry of one page presented through
// one view.
//
// PDFium's text/dest/link APIs report coordinates in PDF USER space (points,
// origin at the bottom-left of the raw content box, y-up, NOT rotated). The
// displayed page space is the one renderPage's pageRectPoints uses: points,
// origin at the top-left, y-down, rotation applied, crop-box-relative. The
// mapping itself lives in pdf/PdfPageGeometry (the single implementation,
// engine-independent and unit-tested): it mirrors the display matrix PDFium
// builds for rendering (CPDF_Page::UpdateDimensions' page_matrix_ - the
// crop-box offset plus the /Rotate quarter turn - composed with the
// y-flipping display matrix in GetDisplayMatrixForFloatRect) and the
// convention documented in docs/ARCHITECTURE.md section 4. The turn-1/turn-3
// directions are pinned by the text-rot90.pdf fixture test (the rot90 render
// fixture pinned the same convention for rendering in Phase 1).
//
// For the NATIVE view, the rotation is FPDFPage_GetRotation and the crop box
// is FPDF_GetPageBoundingBox (CPDF_Page::GetBBox: CropBox ∩ MediaBox, the
// MediaBox defaulting to US Letter when absent/empty), so displaySize equals
// the rotation-aware FPDF_GetPageWidthF/FPDF_GetPageHeightF pair.
struct DisplayGeometry {
    PdfPageView view;
    core::Size displaySize; // == pdf::displaySize(view)
};

// A validated geometry for `view`; nullopt when its crop box is invalid or
// its rotation is not a quarter turn.
inline std::optional<DisplayGeometry> makeDisplayGeometry(const PdfPageView& view) {
    if (!view.cropBox.isValid() || static_cast<int>(view.rotation) > 3) {
        return std::nullopt;
    }
    return DisplayGeometry{view, pdf::displaySize(view)};
}

// The page's native view. Caller must hold the PDFium gate (FPDF_* calls).
inline std::optional<PdfPageView> nativePageView(FPDF_PAGE page) {
    PdfPageView view;
    view.rotation = static_cast<core::PageRotation>(std::clamp(FPDFPage_GetRotation(page), 0, 3));

    // FS_RECTF fields are left/top/right/bottom of the box; in user space top
    // is the LARGER y.
    FS_RECTF rect{};
    if (FPDF_GetPageBoundingBox(page, &rect) != 0) {
        view.cropBox = PdfBox{static_cast<double>(rect.left), static_cast<double>(rect.bottom),
                              static_cast<double>(rect.right), static_cast<double>(rect.top)};
    } else {
        // Fallback: a zero-origin user box consistent with the displayed size
        // (odd quarter-turns swap the user-space axes).
        const double displayWidth = static_cast<double>(FPDF_GetPageWidthF(page));
        const double displayHeight = static_cast<double>(FPDF_GetPageHeightF(page));
        const bool swapped = view.rotation == core::PageRotation::Clockwise90 ||
                             view.rotation == core::PageRotation::Clockwise270;
        view.cropBox = PdfBox{0.0, 0.0, swapped ? displayHeight : displayWidth,
                              swapped ? displayWidth : displayHeight};
    }
    if (!view.cropBox.isValid()) {
        return std::nullopt;
    }
    return view;
}

// Caller must hold the PDFium gate (all queries below are FPDF_* calls).
inline std::optional<DisplayGeometry> makeDisplayGeometry(FPDF_PAGE page) {
    const std::optional<PdfPageView> view = nativePageView(page);
    if (!view.has_value()) {
        return std::nullopt;
    }
    return makeDisplayGeometry(*view);
}

// The page's media box with CPDF_Page::UpdateDimensions' semantics: the box
// normalized, and an empty box replaced by the US-Letter default
// [0 0 612 792]. Caller must hold the PDFium gate.
//
// Limitation: FPDFPage_GetMediaBox reads /MediaBox from the PAGE dictionary
// only (fpdf_transformpage.cpp GetBoundingBox - no /Parent inheritance), and
// no public API exposes the inherited value. For a page that inherits its
// media box we fall back to the native effective crop box, which always lies
// within the real media box (it is CropBox ∩ MediaBox): conservative for the
// "view crop box within media box" validation, never permissive.
inline PdfBox pageMediaBox(FPDF_PAGE page, const PdfPageView& nativeView) {
    float left = 0.0f;
    float bottom = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    if (FPDFPage_GetMediaBox(page, &left, &bottom, &right, &top) == 0) {
        return nativeView.cropBox;
    }
    const PdfBox box{static_cast<double>(std::min(left, right)), static_cast<double>(std::min(bottom, top)),
                     static_cast<double>(std::max(left, right)), static_cast<double>(std::max(bottom, top))};
    if (!box.isValid()) {
        return PdfBox{0.0, 0.0, 612.0, 792.0};
    }
    return box;
}

// Validates `view` for a page whose native view and media box are given: a
// quarter-turn rotation and a valid crop box within the media box. The
// native view itself is always acceptable.
inline core::Status checkPageView(const PdfPageView& view,
                                  const PdfPageView& nativeView,
                                  const PdfBox& mediaBox,
                                  std::size_t pageIndex) {
    if (view == nativeView) {
        return core::ok();
    }
    if (!makeDisplayGeometry(view).has_value()) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                               "page view must have a quarter-turn rotation and a "
                                               "finite, non-empty crop box",
                                               "pdf"));
    }
    if (!mediaBox.contains(view.cropBox, kBoxEpsilon)) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                               "page view crop box exceeds the media box of page " +
                                                   std::to_string(pageIndex),
                                               "pdf"));
    }
    return core::ok();
}

// Both geometries a view-aware read needs.
struct PageGeometry {
    PdfPageView nativeView;
    DisplayGeometry display; // of the requested view; the native one when none
};

// Resolves the geometry of `page` presented through `requestedView` (null =
// the native view), validating the request: a quarter-turn rotation and a
// valid crop box within the page's media box (InvalidArgument otherwise).
// Caller must hold the PDFium gate.
inline core::Result<PageGeometry> resolvePageGeometry(FPDF_PAGE page,
                                                      std::size_t pageIndex,
                                                      const PdfPageView* requestedView) {
    const std::optional<PdfPageView> native = nativePageView(page);
    const std::optional<DisplayGeometry> nativeGeometry =
        native.has_value() ? makeDisplayGeometry(*native) : std::nullopt;
    if (!nativeGeometry.has_value()) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "page " + std::to_string(pageIndex) +
                                                   " has invalid display dimensions",
                                               "pdf"));
    }
    if (requestedView == nullptr || *requestedView == *native) {
        return PageGeometry{*native, *nativeGeometry};
    }

    const core::Status valid =
        checkPageView(*requestedView, *native, pageMediaBox(page, *native), pageIndex);
    if (!valid.has_value()) {
        return std::unexpected(valid.error());
    }
    return PageGeometry{*native, *makeDisplayGeometry(*requestedView)};
}

inline core::Point userToDisplay(const DisplayGeometry& geometry, double x, double y) {
    return pdf::userToDisplay(geometry.view, x, y);
}

// Transforms a user-space box (left < right, bottom < top in the y-up sense)
// by mapping two opposite corners and normalizing to min/max, then clamps
// into the displayed page bounds. Degenerate or non-finite boxes yield an
// empty rect.
inline core::Rect userBoxToDisplayRect(const DisplayGeometry& geometry,
                                       double left,
                                       double right,
                                       double bottom,
                                       double top) {
    return pdf::userBoxToDisplayRect(geometry.view, PdfBox{left, bottom, right, top});
}

} // namespace rivet::pdf::internal
