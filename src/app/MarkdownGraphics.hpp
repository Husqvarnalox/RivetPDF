// SPDX-License-Identifier: MPL-2.0
#pragma once

// Drawing Markdown diagrams and formulas (ADR-0032), shared by the preview
// painter and the PDF exporter so screen, print and export match:
//  - the ONE colour-role mapping (markdownGraphicColor; a theme-token system
//    replaces this function later),
//  - math glyph faces, metrics and outlines: formula glyphs (MathFont::Math)
//    are always the bundled Noto Sans Math (PdfBundledFont::MathRegular,
//    Arimo for the few code points it lacks). The exporter writes them as
//    text in those embedded faces; the preview fills the same glyphs'
//    outlines (pdf::bundledGlyphOutline) through the portable PaintContext
//    path API, so no platform font loading is needed and both place every
//    glyph at the same advance (the PDF widths),
//  - dash patterns, rounded rectangles and dash splitting for diagrams,
//  - paintMarkdownGraphic: a LayoutGraphic through a ui::PaintContext.
// Portable; no platform code. Everything except painting is thread-safe
// (the exporter runs on a worker thread).

#include "app/MarkdownPalette.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "markdown/MarkdownLayout.hpp"
#include "markdown/math/MathScene.hpp"
#include "markdown/mermaid/DiagramScene.hpp"
#include "pdf/PdfContent.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Path.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::app {

// ------------------------------------------------------------------ colours

// Every colour role of a rendered diagram, formula or notice. The first
// values mirror mermaid::DiagramColor one to one.
enum class GraphicRole : std::uint8_t {
    Text,
    NodeFill,
    NodeStroke,
    EdgeStroke,
    LabelFill,
    ClusterFill,
    ClusterStroke,
    NoteFill,
    NoteStroke,
    FrameStroke,
    FrameFill,
    Lifeline,
    NumberFill,
    NumberText,
    MathError, // unknown TeX commands rendered literally
    Notice,    // "could not be rendered" line above a fallback code block
};

constexpr GraphicRole graphicRole(markdown::mermaid::DiagramColor color) {
    return static_cast<GraphicRole>(color);
}

// The colour of a role in a palette (ONE mapping for preview and export).
ui::Color markdownGraphicColor(const MarkdownPreviewPalette& palette, GraphicRole role);

// A colour as written into a PDF: alpha composited over white paper.
pdf::PdfColor toPdfColor(const ui::Color& color);

// --------------------------------------------------------------------- math

// The body-face style a MathFont::Text / TextBold run is measured and drawn in.
markdown::TextStyle mathTextStyle(markdown::math::MathFont font, double size);

// A MathFont::Math run split by bundled face: consecutive code points drawn
// with the same face. `utf8` holds the code points as written (uncovered ones
// become '?'); x and width are at the run's `size`.
struct MathGlyphPiece {
    pdf::PdfBundledFont font = pdf::PdfBundledFont::MathRegular;
    std::string utf8;
    double x = 0.0;
    double width = 0.0;
};
std::vector<MathGlyphPiece> mathGlyphPieces(std::string_view utf8, double size);

// IMathMeasurer for layouts drawn by this module: Math runs from the bundled
// faces (advances as the PDF writes them; ascent/descent = ink extents of
// the glyph outlines), Text runs through `text` (the layout's measurer).
// Thread-safe when `text` is.
class BundledMathMeasurer final : public markdown::math::IMathMeasurer {
public:
    explicit BundledMathMeasurer(const markdown::ITextMeasurer& text) : text_(text) {}
    markdown::math::MathGlyphMetrics measure(std::string_view utf8, markdown::math::MathFont font,
                                             double size) const override;

private:
    const markdown::ITextMeasurer& text_;
};

// Appends the glyph outlines of a Math run set at `size` with the left end of
// its baseline at `origin` (y grows downward) to `path`.
void appendMathRunOutlines(ui::Path& path, std::string_view utf8, double size, core::Point origin);

// ------------------------------------------------------------------ diagrams

// On/off lengths of a dashed stroke (empty: solid).
std::vector<double> diagramDashPattern(markdown::mermaid::DashStyle dash, double strokeWidth);

// A (rounded) rectangle as a closed path.
std::vector<markdown::mermaid::PathSegment> diagramRectSegments(const core::Rect& rect, double cornerRadius);

// The stroked pieces of a dashed outline: every subpath flattened (cubics
// subdivided) and cut into "on" pieces of `pattern`.
std::vector<std::vector<core::Point>> dashedPieces(const std::vector<markdown::mermaid::PathSegment>& segments,
                                                   const std::vector<double>& pattern);

// ------------------------------------------------------------------ painting

// Paints `graphic` with every document coordinate shifted by `offset`.
// `mathColor` is the colour of normal formula glyphs (the surrounding text's).
// Text runs are drawn with the platform text API in MarkdownPaintMeasurer's
// fonts, positioned on their baselines like the preview's other text.
void paintMarkdownGraphic(ui::PaintContext& context, const markdown::LayoutGraphic& graphic, core::Point offset,
                          const MarkdownPreviewPalette& palette, const ui::Color& mathColor);

} // namespace rivet::app
