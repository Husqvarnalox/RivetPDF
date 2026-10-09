// SPDX-License-Identifier: MPL-2.0
#pragma once

// Portable Markdown -> PDF exporter (ADR-0023). Pure: parses the source,
// lays it out at the page content width with the bundled-font metrics,
// paginates and emits a pdf::PdfBuildDocument (PdfEngine::buildDocument
// writes it). No platform code, no UI, no PDF backend: runs on any thread
// and in every build.
//
// Page space: x_pdf = margin + x_layout, y_pdf = pageHeight - margin -
// (y_layout - slice.top). Every emitted item lies inside its page so that
// pdf::validatePdfBuild accepts the result (checked before returning).

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "markdown/MarkdownLayout.hpp"
#include "pdf/PdfBuild.hpp"
#include "pdf/PdfContent.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace rivet::app {

// The bundled font a layout style is written with: headings and bold ->
// SansBold; monospace / Code -> MonoRegular (MonoBold when bold or a
// heading); everything else SansRegular. Italic is written upright (there is
// no bundled italic). The size is style.size.
pdf::PdfBundledFont exportFontFor(const markdown::TextStyle& style);

// The code point actually written for `cp` in `font`: `cp` when the font
// covers it and it is printable, else U+003F '?'. (Line breaks and tabs
// never reach a run: the layout expands tabs and splits lines.)
char32_t exportCodepoint(pdf::PdfBundledFont font, char32_t cp);

// ITextMeasurer over pdf::bundledFontMetrics using exportFontFor and
// exportCodepoint, so a measured width equals the width the written PDF
// gives the same run. ascent/descent are positive distances above/below the
// baseline (the convention the layout expects). Deterministic and
// thread-safe (stateless; the metrics are static).
class BundledFontMeasurer final : public markdown::ITextMeasurer {
public:
    markdown::TextMetrics measure(std::string_view utf8, const markdown::TextStyle& style) const override;
};

// The typography of an export: the layout defaults with print sizes (body
// 11, code 9.5, table 10, headings 22/18/15/13/12/11), no layout page
// margins (the exporter applies its own), proportionally smaller list
// markers/checkboxes and paragraph gaps.
markdown::Typography exportTypography();

struct MarkdownExportOptions {
    double pageWidth = 595.276; // A4 portrait, points
    double pageHeight = 841.89;
    double margin = 72.0;       // all sides
    // maxImageHeight is additionally capped at the content height, so an
    // image always fits on one page.
    markdown::Typography typography = exportTypography();
};

// An image available to the exporter: an immutable snapshot taken on the
// main thread (MarkdownImageStore::snapshot). Known + bitmap = drawn;
// anything else is exported as its placeholder box.
struct ExportImage {
    markdown::ImageInfo info;
    std::shared_ptr<const core::Bitmap> bitmap;
};
using ExportImageMap = std::unordered_map<std::string, ExportImage>; // key = URL as written in the document

struct MarkdownExportResult {
    pdf::PdfBuildDocument document;
    std::size_t pageCount = 0;
    // Characters no bundled font covers, written as '?' (once per character).
    std::size_t replacedCharacters = 0;
    // Images that had pixels but could not be embedded (over the PDF image
    // limits or the export's image budget); drawn as placeholders.
    std::size_t skippedImages = 0;
};

// Parse (default limits), lay out, paginate, emit. InvalidArgument for
// unusable page geometry; Unsupported when the document needs more pages
// than a build allows. Pure; runs on any thread.
core::Result<MarkdownExportResult> buildMarkdownPdf(std::string_view source, const MarkdownExportOptions& options,
                                                    const ExportImageMap& images);

} // namespace rivet::app
