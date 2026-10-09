// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pdf/PdfContent.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Portable horizontal metrics of the bundled TrueType programs (ADR-0016,
// ADR-0023). Read from the font bytes themselves - `head` (unitsPerEm),
// `hhea` (ascender, descender, lineGap, numberOfHMetrics), `OS/2`
// (typographic metrics), `maxp` (glyph count), `hmtx` (advances) and `cmap`
// (format 12 from a (3,10)/(0,x) subtable, else format 4 from (3,1)/(0,x)) -
// so a layout measured here matches what PDFium writes into the embedded
// font's widths (see pdfWidth: PDFium stores advances truncated to integer
// thousandths of an em, and textWidth measures exactly those). No platform
// text engine and no PDFium are involved.
//
// Vertical metrics: the OS/2 sTypoAscender/sTypoDescender/sTypoLineGap when
// the font sets fsSelection bit 7 (USE_TYPO_METRICS), else the hhea values
// (what most renderers use for such fonts; the bundled Arimo/Tinos/Cousine
// subsets do not set the bit, so hhea applies to all of them).
//
// Every table offset and length is bounds-checked even though the data is
// compiled in; a malformed program yields metrics that are not valid():
// unitsPerEm 1000, zero vertical metrics and no glyphs.

namespace rivet::pdf {

class BundledFontMetrics {
public:
    // Parses `fontProgram` (a TrueType/OpenType file). The bytes are not
    // retained.
    explicit BundledFontMetrics(std::span<const std::uint8_t> fontProgram);

    bool valid() const { return valid_; }

    double unitsPerEm() const { return unitsPerEm_; }
    double ascent() const { return ascent_; }   // font units, positive
    double descent() const { return descent_; } // font units, negative (below the baseline)
    double lineGap() const { return lineGap_; } // font units

    // Whether the cmap maps `cp` to a real glyph (not .notdef).
    bool hasGlyph(char32_t cp) const;
    // Advance width of `cp` in font units; 0 when the font has no glyph.
    double advance(char32_t cp) const;
    // Width of `cp` as a PDF font program embedding it declares it: the
    // advance in thousandths of an em, truncated to an integer
    // (floor(advance * 1000 / unitsPerEm)), which is what PDFium writes into
    // the /W array of the embedded CID font (FPDFText_LoadFont) and therefore
    // what every PDF consumer uses to position the next glyph. 0 when the
    // font has no glyph.
    double pdfWidth(char32_t cp) const;
    // Width of a UTF-8 string set on one line at `fontSize` points, in
    // points, exactly as PdfEngine::buildDocument writes it: the sum of the
    // pdfWidth()s times fontSize / 1000 (no kerning). Code points without a
    // glyph (and ill-formed sequences) count as 0.
    double textWidth(std::string_view utf8, double fontSize) const;

private:
    std::uint16_t glyphOf(char32_t cp) const;

    bool valid_ = false;
    double unitsPerEm_ = 1000.0;
    double ascent_ = 0.0;
    double descent_ = 0.0;
    double lineGap_ = 0.0;
    std::vector<std::pair<char32_t, std::uint16_t>> cmap_; // sorted by code point, glyph != 0
    std::vector<std::uint16_t> advances_;                  // per glyph id
};

// Metrics of a bundled font: parsed once on first use (thread-safe), static
// storage.
const BundledFontMetrics& bundledFontMetrics(PdfBundledFont font);

} // namespace rivet::pdf
