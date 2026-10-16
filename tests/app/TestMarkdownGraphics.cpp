// SPDX-License-Identifier: MPL-2.0
// Mermaid diagrams and math drawn by the preview and written by the PDF
// exporter (ADR-0032): both follow the same LayoutGraphic geometry, formula
// glyphs are the bundled math face (outlines on screen, embedded text in the
// PDF), the colour roles come from one mapping, and an unrenderable diagram
// shows its notice.
#include "RivetTest.h"

#include "Fakes.hpp"
#include "MarkdownTestKit.hpp"
#include "app/MarkdownGraphics.hpp"
#include "app/MarkdownPdfExport.hpp"
#include "app/MarkdownPreviewView.hpp"
#include "markdown/MarkdownParser.hpp"
#include "pdf/BundledFontMetrics.hpp"
#include "pdf/PdfBuild.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace rivet;
using app::MarkdownPreviewView;

const char* kSample = "# Graphics\n\n"
                      "Inline $x^2 + \\alpha$ math.\n\n"
                      "$$\n\\frac{a+b}{c}\n$$\n\n"
                      "```mermaid\nflowchart TD\n  A[Start] --> B{Choice}\n  B -->|yes| C[Stop]\n  B -.-> D[Later]\n```\n\n"
                      "```mermaid\nsequenceDiagram\n  Alice->>Bob: Hello\n  Bob-->>Alice: Hi\n```\n";

struct Box {
    double x0 = std::numeric_limits<double>::max();
    double y0 = std::numeric_limits<double>::max();
    double x1 = std::numeric_limits<double>::lowest();
    double y1 = std::numeric_limits<double>::lowest();
    void add(core::Point p) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
};

Box boxOf(const ui::Path& path) {
    Box b;
    for (const ui::PathSegment& s : path.segments) {
        if (s.op == ui::PathSegment::Op::Close) continue;
        b.add(s.p);
        if (s.op == ui::PathSegment::Op::CubicTo) {
            b.add(s.c1);
            b.add(s.c2);
        }
    }
    return b;
}

struct Preview {
    ui::testing::FakePaintContext ctx;
    std::unique_ptr<MarkdownPreviewView> view;
    explicit Preview(const std::string& src, double w = 640.0, double h = 4000.0) {
        view = std::make_unique<MarkdownPreviewView>(app::MarkdownPreviewEnvironment{});
        view->setFrame(core::Rect{0.0, 0.0, w, h});
        view->setDocumentSnapshot(
            std::make_shared<const markdown::MarkdownDocument>(markdown::testkit::parse(src)), 1, src);
        view->paint(ctx);
    }
    bool drew(std::string_view text) const {
        return std::any_of(ctx.texts.begin(), ctx.texts.end(), [&](const auto& t) { return t.text == text; });
    }
};

const markdown::LayoutGraphic* graphic(const markdown::MarkdownLayout& l, bool diagram, bool display) {
    for (const markdown::LayoutGraphic& g : l.graphics) {
        if ((g.diagram != nullptr) == diagram && g.display == display) return &g;
    }
    return nullptr;
}

} // namespace

RIVET_TEST(previewPaintsFormulasAsBundledGlyphOutlinesInPlace) {
    Preview p(kSample);
    const markdown::MarkdownLayout* layout = p.view->currentLayout();
    CHECK(layout != nullptr);
    if (layout == nullptr) return;
    CHECK_EQ(layout->graphics.size(), std::size_t{4});
    const app::MarkdownPreviewPalette palette;
    for (const bool display : {false, true}) {
        const markdown::LayoutGraphic* g = graphic(*layout, false, display);
        CHECK(g != nullptr);
        if (g == nullptr) continue;
        // The glyphs are one filled path in the text colour whose ink lies in
        // the graphic's box (ascent/descent were measured from the same outlines).
        bool found = false;
        for (const auto& rec : p.ctx.paths) {
            if (rec.stroke || !(rec.color == palette.text)) continue;
            const Box b = boxOf(rec.path);
            if (b.x0 < g->bounds.minX() - 0.5 || b.x1 > g->bounds.maxX() + 0.5) continue;
            if (b.y0 < g->bounds.minY() - 0.5 || b.y1 > g->bounds.maxY() + 0.5) continue;
            found = true;
            CHECK_NEAR(b.y0, g->bounds.minY(), 0.5);
            CHECK_NEAR(b.y1, g->bounds.maxY(), 0.5);
        }
        CHECK(found);
    }
    // The fraction bar is a filled rule.
    const markdown::LayoutGraphic* display = graphic(*layout, false, true);
    CHECK(display != nullptr && !display->math->rules.empty());
}

RIVET_TEST(previewPaintsDiagramsWithRoleColours) {
    Preview p(kSample);
    for (const char* label : {"Start", "Choice", "Stop", "Later", "yes", "Alice", "Bob", "Hello", "Hi"}) {
        CHECK(p.drew(label));
    }
    const app::MarkdownPreviewPalette palette;
    const ui::Color edge = app::markdownGraphicColor(palette, app::GraphicRole::EdgeStroke);
    const ui::Color node = app::markdownGraphicColor(palette, app::GraphicRole::NodeFill);
    std::size_t edgeStrokes = 0;
    std::size_t nodeFills = 0;
    for (const auto& rec : p.ctx.paths) {
        if (rec.stroke && rec.color == edge) ++edgeStrokes;
        if (!rec.stroke && rec.color == node) ++nodeFills;
    }
    CHECK(edgeStrokes >= 3);
    CHECK(nodeFills >= 4);
    // Labels are drawn inside the diagram's box.
    const markdown::LayoutGraphic* g = graphic(*p.view->currentLayout(), true, true);
    CHECK(g != nullptr);
    if (g == nullptr) return;
    for (const auto& t : p.ctx.texts) {
        if (t.text != "Start") continue;
        CHECK(t.rect.minX() >= g->bounds.minX() && t.rect.minX() <= g->bounds.maxX());
        CHECK(t.rect.center().y >= g->bounds.minY() && t.rect.center().y <= g->bounds.maxY());
    }
}

RIVET_TEST(previewShowsANoticeForAnUnrenderableDiagram) {
    Preview p("```mermaid\ngantt\n  title Plan\n```\n");
    const app::MarkdownPreviewPalette palette;
    bool notice = false;
    for (const auto& t : p.ctx.texts) {
        if (t.text.rfind("Diagram not rendered", 0) == 0) {
            notice = true;
            CHECK(t.color == palette.error);
        }
    }
    CHECK(notice);
    CHECK(p.drew("gantt"));
}

RIVET_TEST(dashedPiecesFollowThePattern) {
    namespace mm = markdown::mermaid;
    std::vector<mm::PathSegment> line{{mm::PathSegment::Kind::MoveTo, {0.0, 0.0}, {}, {}},
                                      {mm::PathSegment::Kind::LineTo, {20.0, 0.0}, {}, {}}};
    const auto pieces = app::dashedPieces(line, {4.0, 3.0});
    CHECK_EQ(pieces.size(), std::size_t{3}); // [0,4] [7,11] [14,18]
    if (pieces.size() == 3) {
        CHECK_NEAR(pieces[1].front().x, 7.0, 1e-9);
        CHECK_NEAR(pieces[1].back().x, 11.0, 1e-9);
        CHECK_NEAR(pieces[2].back().x, 18.0, 1e-9);
    }
    CHECK(app::diagramDashPattern(mm::DashStyle::Solid, 1.0).empty());
    CHECK_EQ(app::diagramRectSegments(core::Rect{0, 0, 10, 10}, 3.0).size(), std::size_t{10});
}

RIVET_TEST(bundledMathMeasurerUsesPdfAdvancesAndInk) {
    const app::BundledFontMeasurer text;
    const app::BundledMathMeasurer m(text);
    const std::string italicX = "\xF0\x9D\x91\xA5"; // U+1D465
    const auto x = m.measure(italicX, markdown::math::MathFont::Math, 10.0);
    const auto& metrics = pdf::bundledFontMetrics(pdf::PdfBundledFont::MathRegular);
    CHECK_NEAR(x.width, metrics.pdfWidth(U'\U0001D465') * 10.0 / 1000.0, 1e-9);
    CHECK(x.ascent > 3.0 && x.ascent < 7.0); // ink: about the x-height
    CHECK(x.descent < 1.0);
    const auto sum = m.measure("\xE2\x88\x91", markdown::math::MathFont::Math, 10.0); // U+2211
    CHECK(sum.ascent > x.ascent);
    CHECK(sum.descent > 0.5);
    // Text roles go to the text measurer.
    const auto t = m.measure("ab", markdown::math::MathFont::Text, 10.0);
    CHECK_NEAR(t.width, text.measure("ab", app::mathTextStyle(markdown::math::MathFont::Text, 10.0)).width, 1e-9);
}

RIVET_TEST(exportWritesGraphicsWhereTheLayoutPutsThem) {
    const app::MarkdownExportOptions options;
    auto built = app::buildMarkdownPdf(kSample, options, {});
    CHECK(built.has_value());
    if (!built.has_value()) return;
    CHECK_EQ(built->replacedCharacters, std::size_t{0});

    // The same layout the exporter makes.
    markdown::Typography t = options.typography;
    t.pageMarginX = t.pageMarginY = 0.0;
    t.maxGraphicHeight = options.pageHeight - 2.0 * options.margin;
    const app::BundledFontMeasurer measurer;
    const app::BundledMathMeasurer mathMeasurer(measurer);
    const markdown::MarkdownDocument doc = markdown::makeMarkdownParser()->parse(kSample);
    const markdown::MarkdownLayout layout = markdown::layoutMarkdown(doc, options.pageWidth - 2.0 * options.margin,
                                                                     measurer, nullptr, t, nullptr, &mathMeasurer);
    CHECK_EQ(layout.graphics.size(), std::size_t{4});

    std::size_t paths = 0;
    std::size_t dashed = 0;
    std::vector<const pdf::PdfBuildText*> mathTexts;
    std::vector<std::string> sans;
    for (const pdf::PdfBuildPage& page : built->document.pages) {
        for (const pdf::PdfBuildItem& item : page.items) {
            if (const auto* path = std::get_if<pdf::PdfBuildPath>(&item)) {
                ++paths;
                dashed += path->dash.empty() ? 0 : 1;
            } else if (const auto* text = std::get_if<pdf::PdfBuildText>(&item)) {
                if (text->font == pdf::PdfBundledFont::MathRegular) mathTexts.push_back(text);
                if (text->font == pdf::PdfBundledFont::SansRegular) sans.push_back(text->utf8);
            }
        }
    }
    CHECK(paths >= 10);
    CHECK(dashed >= 2); // the dotted flowchart edge and the dashed reply
    for (const char* label : {"Start", "Choice", "Alice", "Hello"}) {
        CHECK(std::find(sans.begin(), sans.end(), std::string(label)) != sans.end());
    }
    CHECK(!mathTexts.empty());
    // The inline formula's first glyph run sits exactly at the layout's position.
    const markdown::LayoutGraphic* inl = graphic(layout, false, false);
    CHECK(inl != nullptr);
    if (inl == nullptr || mathTexts.empty()) return;
    const markdown::math::MathGlyphRun& run = inl->math->runs.front();
    const std::vector<app::MathGlyphPiece> pieces = app::mathGlyphPieces(run.utf8, run.size);
    CHECK(!pieces.empty());
    if (pieces.empty()) return;
    const double x = options.margin + inl->origin.x + (run.origin.x + pieces[0].x) * inl->scale;
    const double y = options.pageHeight - options.margin - (inl->origin.y + run.origin.y * inl->scale);
    bool placed = false;
    for (const pdf::PdfBuildText* text : mathTexts) {
        if (text->utf8 == pieces[0].utf8 && std::abs(text->origin.x - x) < 1e-6 && std::abs(text->origin.y - y) < 1e-6) {
            placed = true;
            CHECK_NEAR(text->fontSize, run.size * inl->scale, 1e-9);
        }
    }
    CHECK(placed);
}

RIVET_TEST(colourRolesComeFromOneMapping) {
    const app::MarkdownPreviewPalette palette;
    CHECK(app::markdownGraphicColor(palette, app::GraphicRole::Text) == palette.text);
    CHECK(app::markdownGraphicColor(palette, app::GraphicRole::MathError) == palette.error);
    CHECK(app::graphicRole(markdown::mermaid::DiagramColor::NumberText) == app::GraphicRole::NumberText);
    const pdf::PdfColor half = app::toPdfColor(ui::Color::rgba(0.0, 0.0, 0.0, 0.5));
    CHECK_NEAR(static_cast<double>(half.r), 0.5, 1e-6);
}
