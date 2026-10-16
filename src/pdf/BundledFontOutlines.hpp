// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfContent.hpp"

#include <cstdint>
#include <span>
#include <vector>

// TrueType glyph outlines of the bundled fonts (ADR-0016, ADR-0025): read
// from the `glyf`/`loca` tables of the compiled-in programs so a typed
// visual signature can be drawn as vector paths - identically on screen and
// in the saved /AP - without embedding a font in every annotation.
//
// Coordinates are font units, y-up, relative to the glyph origin. Quadratic
// TrueType curves are converted exactly to cubic segments. Every read is
// bounds-checked; composite glyphs are followed to a bounded depth, and a
// malformed or over-limit glyph yields an empty outline (drawn as nothing,
// its advance still applies).

namespace rivet::pdf {

// Outline of glyph `glyphId` of a TrueType program (empty for an empty
// glyph such as the space, for .notdef, or when unreadable).
std::vector<PdfPathSegment> trueTypeGlyphOutline(std::span<const std::uint8_t> fontProgram, std::uint16_t glyphId);

// Outline of the glyph a bundled font maps `codepoint` to (empty when the
// font has none).
std::vector<PdfPathSegment> bundledGlyphOutline(PdfBundledFont font, char32_t codepoint);

} // namespace rivet::pdf
