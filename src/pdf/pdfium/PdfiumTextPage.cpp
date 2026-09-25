// SPDX-License-Identifier: MPL-2.0

#include "PdfiumTextPage.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "fpdf_edit.h" // FPDFPage_GetRotation
#include "fpdf_text.h" // FPDFText_*

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
// PDFium's text APIs report coordinates in PDF USER space (points, origin at
// the bottom-left of the raw content box, y-up, NOT rotated). The displayed
// page space is the one renderPage's pageRectPoints uses: points, origin at
// the top-left, y-down, /Rotate applied, crop-box-relative. This mirrors the
// display matrix PDFium builds for rendering (CPDF_Page::UpdateDimensions'
// page_matrix_ - the crop-box offset plus the /Rotate quarter turn - composed
// with the y-flipping display matrix in GetDisplayMatrixForFloatRect) and the
// convention documented in docs/ARCHITECTURE.md section 4 and implemented in
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
std::optional<DisplayGeometry> makeDisplayGeometry(FPDF_PAGE page) {
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

core::Point userToDisplay(const DisplayGeometry& geometry, double x, double y) {
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

// Transforms a user-space char box (left < right, bottom < top in the y-up
// sense) by mapping two opposite corners and normalizing to min/max, then
// clamps into the displayed page bounds (chars may stick out of the page).
// Degenerate or non-finite boxes yield an empty rect.
core::Rect userBoxToDisplayRect(const DisplayGeometry& geometry,
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

// FPDFText_GetUnicode returns 0 on failure; surrogate halves and values above
// U+10FFFF cannot appear in valid UTF-8 and are reported as U+FFFD.
char32_t sanitizeUnicode(unsigned int unicode) {
    if (unicode == 0u || (unicode >= 0xD800u && unicode <= 0xDFFFu) || unicode > 0x10FFFFu) {
        return 0xFFFD;
    }
    return static_cast<char32_t>(unicode);
}

} // namespace

// Caller must hold the PDFium gate: every call below (page load, text load,
// per-char queries, FPDF_GetLastError, and the RAII closes at scope exit)
// must run serialized against all other PDFium calls. This function never
// acquires the gate itself (see PdfiumTextPage.h).
core::Result<std::shared_ptr<const PdfTextPage>> extractTextPage(FPDF_DOCUMENT document,
                                                                 std::size_t pageIndex,
                                                                 std::size_t pageCount) {
    if (document == nullptr) {
        return std::unexpected(
            core::makeError(core::ErrorCode::InvalidArgument, "document handle is null", "pdf"));
    }
    if (pageIndex >= pageCount) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                               "page index " + std::to_string(pageIndex) +
                                                   " out of range (document has " +
                                                   std::to_string(pageCount) + " pages)",
                                               "pdf"));
    }

    ScopedPage page(FPDF_LoadPage(document, static_cast<int>(pageIndex)));
    if (page.get() == nullptr) {
        const int lastError = static_cast<int>(FPDF_GetLastError());
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "PDFium failed to load page " +
                                                   std::to_string(pageIndex) +
                                                   " (FPDF error " + std::to_string(lastError) + ")",
                                               "pdf"));
    }

    const std::optional<DisplayGeometry> geometry = makeDisplayGeometry(page.get());
    if (!geometry.has_value()) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "page " + std::to_string(pageIndex) +
                                                   " has invalid display dimensions",
                                               "pdf"));
    }

    ScopedTextPage textPage(FPDFText_LoadPage(page.get()));
    if (textPage.get() == nullptr) {
        const int lastError = static_cast<int>(FPDF_GetLastError());
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "PDFium failed to load the text of page " +
                                                   std::to_string(pageIndex) + " (FPDF error " +
                                                   std::to_string(lastError) + ")",
                                               "pdf"));
    }

    const int counted = FPDFText_CountChars(textPage.get());
    const std::size_t count = counted > 0 ? static_cast<std::size_t>(counted) : 0;

    std::vector<TextChar> chars;
    chars.reserve(count);
    for (int i = 0; i < static_cast<int>(count); ++i) {
        TextChar ch;
        ch.index = static_cast<std::uint32_t>(i);
        ch.unicode = sanitizeUnicode(FPDFText_GetUnicode(textPage.get(), i));

        // Generated chars (line breaks, inserted spaces) have no glyph box of
        // their own; they get an empty (zero-area) bounds.
        if (FPDFText_IsGenerated(textPage.get(), i) != 1) {
            double left = 0.0;
            double right = 0.0;
            double bottom = 0.0;
            double top = 0.0;
            if (FPDFText_GetCharBox(textPage.get(), i, &left, &right, &bottom, &top) != 0) {
                ch.bounds = userBoxToDisplayRect(*geometry, left, right, bottom, top);
            }
        }

        const double fontSize = FPDFText_GetFontSize(textPage.get(), i);
        ch.fontSize = (std::isfinite(fontSize) && fontSize > 0.0) ? fontSize : 0.0;

        chars.push_back(std::move(ch));
    }

    // Detached plain data: safe to use after every FPDF_* handle is closed.
    return std::make_shared<const PdfTextPage>(std::move(chars));
}

} // namespace rivet::pdf
