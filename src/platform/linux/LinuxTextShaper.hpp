// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/UiTypes.hpp"

#include <cairo.h>

#include <memory>
#include <string_view>
#include <vector>

namespace rivet::platform::text {

// Single-line text layout for the Linux backend: fontconfig resolves the UI
// families ("sans-serif" / "monospace" with the requested weight and slant,
// plus per-code-point fallback faces for characters the primary face lacks),
// HarfBuzz shapes (its own OpenType font functions, no FreeType state shared
// with rendering), cairo-ft renders.
//
// Semantics mirror the CoreText path of MacosPaintContext:
//   - width  = sum of the shaped advances (CTLineGetTypographicBounds width)
//   - ascent/descent = the largest typographic ascent/descent of the faces
//     used on the line (no leading)
//   - empty text measures {0, 0}.
// measureText() and drawText() both consume shapeLine(), so they agree by
// construction.
//
// Thread-safe: font resolution and the shaping cache are mutex-guarded
// (measurement may be reached from layout code). The cairo font faces in a
// ShapedRun are process-lifetime objects; rendering them is main-thread only.

struct ShapedGlyph {
    unsigned long index = 0; // glyph id in the run's face
    double x = 0.0;          // pen position relative to the line origin (logical units)
    double y = 0.0;          // offset from the baseline, y-down
};

struct ShapedRun {
    cairo_font_face_t* face = nullptr; // process-lifetime, never released
    std::vector<ShapedGlyph> glyphs;
};

struct ShapedLine {
    std::vector<ShapedRun> runs;
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};

std::shared_ptr<const ShapedLine> shapeLine(std::string_view text, const ui::Font& font);

// Convenience: {width, ascent + descent} of shapeLine().
struct LineMetrics {
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};
LineMetrics measureLine(std::string_view text, const ui::Font& font);

// Applies the font options the shaper's metrics assume (unhinted metrics so
// cairo's glyph raster matches HarfBuzz positions; grayscale antialiasing).
void applyFontOptions(cairo_t* cr);

} // namespace rivet::platform::text
