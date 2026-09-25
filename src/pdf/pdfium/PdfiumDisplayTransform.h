// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared PDFium-adapter internals: the user-space -> displayed-page geometry
// transform and the FPDF_PAGE RAII wrapper. INTERNAL to src/pdf/pdfium/ - the
// types here exist only so PdfiumTextPage.cpp and PdfiumDocument.cpp share
// one implementation of the transform. FPDF_* types must not leak past
// src/pdf/pdfium/.

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include "fpdf_edit.h" // FPDFPage_GetRotation
#include "fpdf_text.h" // FPDFText_ClosePage
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

// The user-space -> displayed-page geometry of one page.
//
// PDFium's text/dest APIs report coordinates in PDF USER space (points,
// origin at the bottom-left of the raw content box, y-up, NOT rotated). The
// displayed page space is the one renderPage's pageRectPoints uses: points,
// origin at the top-left, y-down, /Rotate applied, crop-box-relative. This
// mirrors the display matrix PDFium builds for rendering
// (CPDF_Page::UpdateDimensions' page_matrix_ - the crop-box offset plus the
// /Rotate quarter turn - composed with the y-flipping display matrix in
// GetDisplayMatrixForFloatRect) and the convention documented in
// docs/ARCHITECTURE.md section 4 and implemented in
// src/render/PageTransform.cpp's displayDelta: for a user point (x, y) with
// dx0 = x - boxLeft, dy0 = y - boxBottom and user box size (w0, h0):
//   turn 0: displayX = dx0,          displayY = h0 - dy0
//   turn 1: displayX = dy0,          displayY = dx0
//   turn 2: displayX = w0 - dx0,     displayY = dy0
//   turn 3: displayX = h0 - dy0,     displayY = w0 - dx0
// The displayed page size (W, H) is the rotation-aware
// FPDF_GetPageWidthF/FPDF_GetPageHeightF pair. The turn-1/turn-3 directions
// are pinned by the text-rot90.pdf fixture test (the rot90 render fixture
// already pinned the same convention for rendering in Phase 1).
struct DisplayGeometry {
    double displayWidth = 0.0; // W, rotation-aware
    double displayHeight = 0.0;
    double boxLeft = 0.0;   // crop box origin in user space (y-up)
    double boxBottom = 0.0;
    double boxWidth = 0.0;  // w0 = right - left
    double boxHeight = 0.0; // h0 = top - bottom
    int turn = 0;           // quarter turns clockwise, 0..3
};

// Caller must hold the PDFium gate (all queries below are FPDF_* calls).
inline std::optional<DisplayGeometry> makeDisplayGeometry(FPDF_PAGE page) {
    const double displayWidth = static_cast<double>(FPDF_GetPageWidthF(page));
    const double displayHeight = static_cast<double>(FPDF_GetPageHeightF(page));
    if (!std::isfinite(displayWidth) || !std::isfinite(displayHeight) || displayWidth <= 0.0 ||
        displayHeight <= 0.0) {
        return std::nullopt;
    }

    DisplayGeometry geometry;
    geometry.displayWidth = displayWidth;
    geometry.displayHeight = displayHeight;
    geometry.turn = std::clamp(FPDFPage_GetRotation(page), 0, 3);

    // The crop box (crop ∩ media) in user space. FS_RECTF fields are
    // left/top/right/bottom of the box; in user space top is the LARGER y.
    FS_RECTF rect{};
    if (FPDF_GetPageBoundingBox(page, &rect) != 0) {
        geometry.boxLeft = static_cast<double>(rect.left);
        geometry.boxBottom = static_cast<double>(rect.bottom);
        geometry.boxWidth = static_cast<double>(rect.right) - geometry.boxLeft;
        geometry.boxHeight = static_cast<double>(rect.top) - geometry.boxBottom;
    } else {
        // Fallback: a zero-origin user box consistent with the displayed size
        // (odd quarter-turns swap the user-space axes).
        geometry.boxLeft = 0.0;
        geometry.boxBottom = 0.0;
        geometry.boxWidth = (geometry.turn % 2 == 1) ? displayHeight : displayWidth;
        geometry.boxHeight = (geometry.turn % 2 == 1) ? displayWidth : displayHeight;
    }
    if (!std::isfinite(geometry.boxWidth) || !std::isfinite(geometry.boxHeight) ||
        geometry.boxWidth <= 0.0 || geometry.boxHeight <= 0.0) {
        return std::nullopt;
    }
    return geometry;
}

inline core::Point userToDisplay(const DisplayGeometry& geometry, double x, double y) {
    const double dx0 = x - geometry.boxLeft;
    const double dy0 = y - geometry.boxBottom;
    switch (geometry.turn) {
        case 1:
            return {dy0, dx0};
        case 2:
            return {geometry.boxWidth - dx0, dy0};
        case 3:
            return {geometry.boxHeight - dy0, geometry.boxWidth - dx0};
        default:
            return {dx0, geometry.boxHeight - dy0};
    }
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
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(bottom) ||
        !std::isfinite(top) || right <= left || top <= bottom) {
        return {};
    }
    const core::Point a = userToDisplay(geometry, left, bottom);
    const core::Point b = userToDisplay(geometry, right, top);
    const double minX = std::clamp(std::min(a.x, b.x), 0.0, geometry.displayWidth);
    const double maxX = std::clamp(std::max(a.x, b.x), 0.0, geometry.displayWidth);
    const double minY = std::clamp(std::min(a.y, b.y), 0.0, geometry.displayHeight);
    const double maxY = std::clamp(std::max(a.y, b.y), 0.0, geometry.displayHeight);
    return core::Rect{core::Point{minX, minY}, core::Size{maxX - minX, maxY - minY}};
}

} // namespace rivet::pdf::internal
