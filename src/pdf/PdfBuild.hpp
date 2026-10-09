// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// A NEW document described as Rivet-owned data (ADR-0023): pages with a size
// in points and an ordered list of items in page user space (points, origin
// bottom-left, y-up), plus external URI link rectangles.
// PdfEngine::buildDocument materializes it; validatePdfBuild is the contract
// every backend enforces first. No backend type appears here.

namespace rivet::pdf {

// --- Limits (defensive; validatePdfBuild rejects anything beyond them) -----

inline constexpr std::size_t kMaxBuildPages = 10000;
inline constexpr std::size_t kMaxBuildItemsPerPage = 100000;
// Items over the whole document (keeps one build's memory bounded).
inline constexpr std::size_t kMaxBuildItemsTotal = 2'000'000;
inline constexpr std::size_t kMaxBuildLinksPerPage = 10000;
// UTF-8 bytes of one text run (one run = one line of text).
inline constexpr std::size_t kMaxBuildTextBytes = 16 * 1024;
// Page width/height range in points (the PDF 1.7 implementation limits).
inline constexpr double kMinBuildPageSize = 3.0;
inline constexpr double kMaxBuildPageSize = 14400.0;
// Rectangle stroke width range in points (exclusive minimum).
inline constexpr double kMaxBuildStrokeWidth = 100.0;
// Bytes of a link URI.
inline constexpr std::size_t kMaxBuildUriBytes = 2048;
// Slack allowed when checking that a box lies within its page (layout
// arithmetic rounds); boxes beyond it are rejected, never clamped.
inline constexpr double kBuildPageEpsilon = 1.0e-6;

// --- Items ---------------------------------------------------------------------

// A rectangle: filled, stroked or both (at least one).
struct PdfBuildRect {
    PdfBox box;
    std::optional<PdfColor> fill;
    std::optional<PdfColor> stroke;
    float strokeWidth = 1.0F; // points, used when `stroke` is set
};

// One line of text in one bundled font. Every code point must be covered by
// the font (bundledFontCovers) and be printable: no line breaks, no control
// characters. Advances are the font's own (see BundledFontMetrics), with no
// kerning, so a run measured with bundledFontMetrics(font).textWidth is
// exactly as wide in the written PDF.
struct PdfBuildText {
    PdfBundledFont font = PdfBundledFont::SansRegular;
    double fontSize = 11.0; // points, [kMinFontSize, kMaxFontSize]
    PdfColor color;
    PdfPoint origin;        // baseline start, user space, inside the page
    std::string utf8;       // non-empty, valid UTF-8
};

// An image stretched onto `box`.
struct PdfBuildImage {
    PdfImageData image;
    PdfBox box;
};

using PdfBuildItem = std::variant<PdfBuildRect, PdfBuildText, PdfBuildImage>; // painted in order

// An external link annotation: clicking `box` opens `uri` (http, https or
// mailto only; printable ASCII).
struct PdfBuildLink {
    PdfBox box;
    std::string uri;
};

struct PdfBuildPage {
    double width = 612.0;
    double height = 792.0;
    std::vector<PdfBuildItem> items;
    std::vector<PdfBuildLink> links;
};

struct PdfBuildDocument {
    std::vector<PdfBuildPage> pages;
};

// Structural validation: at least one page and the limits above; finite,
// in-range page sizes; valid (positive-size) boxes lying within their page;
// text origins inside the page; font sizes in [kMinFontSize, kMaxFontSize];
// color components in [0, 1]; stroke widths in (0, kMaxBuildStrokeWidth];
// text runs non-empty valid UTF-8 whose every code point the run's font
// covers (the first offending one is named as U+XXXX); image data consistent
// with its format (validateImageData); link URIs with an allowed scheme
// (compared case-insensitively), no control characters, spaces or non-ASCII
// bytes. InvalidArgument otherwise; nothing is clamped or replaced.
core::Status validatePdfBuild(const PdfBuildDocument& document);

} // namespace rivet::pdf
