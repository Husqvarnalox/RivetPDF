// SPDX-License-Identifier: MPL-2.0

#include "PdfiumTextPage.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "PdfiumDisplayTransform.h"
#include "fpdf_text.h" // FPDFText_*

namespace rivet::pdf {
namespace {

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
                                                                 std::size_t pageCount,
                                                                 const PdfPageView* view) {
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

    internal::ScopedPage page(FPDF_LoadPage(document, static_cast<int>(pageIndex)));
    if (page.get() == nullptr) {
        const int lastError = static_cast<int>(FPDF_GetLastError());
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "PDFium failed to load page " +
                                                   std::to_string(pageIndex) +
                                                   " (FPDF error " + std::to_string(lastError) + ")",
                                               "pdf"));
    }

    // Char boxes come back in user space; they are mapped into the display
    // space of the requested view (the native one when view is null).
    const auto resolved = internal::resolvePageGeometry(page.get(), pageIndex, view);
    if (!resolved.has_value()) {
        return std::unexpected(resolved.error());
    }
    const internal::DisplayGeometry* geometry = &resolved->display;

    internal::ScopedTextPage textPage(FPDFText_LoadPage(page.get()));
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
                ch.bounds = internal::userBoxToDisplayRect(*geometry, left, right, bottom, top);
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
