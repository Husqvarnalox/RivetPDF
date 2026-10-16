// SPDX-License-Identifier: MPL-2.0
// Mermaid diagrams and math in the Markdown layout (ADR-0032): inline math
// atoms on the baseline, centered display math, diagrams scaled down to fit,
// the code-block fallback with a notice, and the scene cache.
#include "RivetTest.h"

#include "MarkdownTestKit.hpp"
#include "markdown/MarkdownPaginate.hpp"
#include "markdown/math/MathScene.hpp"
#include "markdown/mermaid/DiagramScene.hpp"

#include <cmath>
#include <cstddef>
#include <string>

using namespace rivet::markdown;
using namespace rivet::markdown::testkit;

namespace {

const FixedMeasurer& measurer() {
    static const FixedMeasurer m;
    return m;
}

MarkdownLayout lay(std::string_view src, double width, MeasureCache* cache = nullptr, Typography t = plainTypography()) {
    return layoutMarkdown(parse(src), width, measurer(), nullptr, t, cache);
}

const LayoutRun* firstGraphicRun(const MarkdownLayout& l) {
    for (const LayoutRun& r : l.runs) {
        if (r.graphic != 0) return &r;
    }
    return nullptr;
}

const LayoutBlock* firstBlock(const MarkdownLayout& l, LayoutBlockKind kind) {
    for (const LayoutBlock& b : l.blocks) {
        if (b.kind == kind) return &b;
    }
    return nullptr;
}

const char* kWideFlowchart = "```mermaid\nflowchart LR\n"
                             "  A[Alpha node] --> B[Beta node] --> C[Gamma node] --> D[Delta node]\n"
                             "  D --> E[Epsilon node] --> F[Zeta node] --> G[Eta node]\n```\n";

} // namespace

RIVET_TEST(inlineMathIsAnAtomOnTheBaseline) {
    const MarkdownLayout l = lay("Euler: $e^{i\\pi} + 1 = 0$ holds.", 2000.0);
    CHECK_EQ(l.graphics.size(), std::size_t{1});
    const LayoutRun* run = firstGraphicRun(l);
    CHECK(run != nullptr);
    if (run == nullptr) return;
    CHECK_EQ(run->text, std::string("$e^{i\\pi} + 1 = 0$"));
    CHECK(!run->exactSource);
    const LayoutGraphic& g = l.graphics[run->graphic - 1];
    CHECK(g.math != nullptr);
    CHECK(g.diagram == nullptr);
    CHECK(!g.display);
    CHECK_NEAR(g.scale, 1.0, 1e-9);
    // The formula's baseline is the line's baseline, shared with the text.
    const LayoutLine& line = l.lines[run->line];
    CHECK_NEAR(g.origin.y, line.baseline, 1e-9);
    CHECK_NEAR(g.origin.x, run->x, 1e-9);
    CHECK_NEAR(run->width, g.math->width, 1e-9);
    for (std::uint32_t r = line.runBegin; r < line.runEnd; ++r) CHECK_NEAR(l.runs[r].baseline, line.baseline, 1e-9);
    // The drawn box spans ascent above / descent below the baseline and lies in the line.
    CHECK_NEAR(g.bounds.minY(), line.baseline - g.math->ascent, 1e-9);
    CHECK_NEAR(g.bounds.maxY(), line.baseline + g.math->descent, 1e-9);
    CHECK(g.bounds.minY() >= line.top - 1e-9);
    CHECK(g.bounds.maxY() <= line.top + line.height + 1e-9);
    // Text before and after: the atom never merges with neighbouring runs.
    CHECK(run > &l.runs.front());
    CHECK((run - 1)->graphic == 0);
    CHECK((run + 1)->text.find("holds") != std::string::npos);
    // Hit-testing an atom yields its edges only.
    const HitResult left = l.hitTest({run->x + run->width * 0.3, line.baseline}, measurer());
    const HitResult right = l.hitTest({run->x + run->width * 0.7, line.baseline}, measurer());
    CHECK(left.valid && right.valid);
    CHECK_EQ(left.position.offset, 0u);
    CHECK_EQ(right.position.offset, static_cast<std::uint32_t>(run->text.size()));
    CHECK_NEAR(l.xForPosition(right.position, measurer()), run->x + run->width, 1e-9);
}

RIVET_TEST(tallInlineMathGrowsItsLineOnly) {
    const MarkdownLayout plain = lay("plain line", 2000.0);
    const MarkdownLayout tall = lay("a $\\frac{\\sum_{i=1}^{n} x_i}{\\sqrt{n}}$ b", 2000.0);
    CHECK_EQ(plain.lines.size(), std::size_t{1});
    CHECK_EQ(tall.lines.size(), std::size_t{1});
    CHECK(tall.lines[0].height > plain.lines[0].height);
    const LayoutGraphic& g = tall.graphics.at(0);
    CHECK(g.bounds.minY() >= tall.lines[0].top - 1e-9);
    CHECK(g.bounds.maxY() <= tall.lines[0].top + tall.lines[0].height + 1e-9);
}

RIVET_TEST(moneyIsNotMath) {
    const MarkdownLayout l = lay("It costs $5 and $10, or $ 3 $ at most.", 2000.0);
    CHECK(l.graphics.empty());
    CHECK(firstGraphicRun(l) == nullptr);
}

RIVET_TEST(displayMathIsCenteredAndAtomic) {
    const double width = 600.0;
    const MarkdownLayout l = lay("Above\n\n$$\n\\int_0^1 x^2 \\, dx = \\frac{1}{3}\n$$\n\nBelow\n", width);
    const LayoutBlock* block = firstBlock(l, LayoutBlockKind::Graphic);
    CHECK(block != nullptr);
    if (block == nullptr) return;
    CHECK(block->special == SpecialBlock::DisplayMath);
    CHECK(block->readOnlyIsland());
    CHECK_EQ(block->lineEnd, block->lineBegin); // no caret stops
    const LayoutGraphic& g = l.graphics.at(block->graphic - 1);
    CHECK(g.math != nullptr);
    CHECK(g.display);
    CHECK_NEAR(g.bounds.minX() - block->rect.minX(), block->rect.maxX() - g.bounds.maxX(), 1e-6);
    CHECK(g.bounds.minY() >= block->rect.minY());
    CHECK(g.bounds.maxY() <= block->rect.maxY() + 1e-9);
    CHECK_NEAR(g.origin.y, g.bounds.minY() + g.math->ascent * g.scale, 1e-9);
    // Pagination never cuts through it.
    const std::vector<PageSlice> slices = paginateLayout(l, block->rect.minY() + block->rect.size.height * 0.5);
    for (const PageSlice& s : slices) CHECK(!(s.bottom > block->rect.minY() + 1e-6 && s.bottom < block->rect.maxY() - 1e-6));
}

RIVET_TEST(diagramsScaleDownToFitButNeverUp) {
    const MarkdownLayout wide = lay(kWideFlowchart, 5000.0);
    const LayoutBlock* wb = firstBlock(wide, LayoutBlockKind::Graphic);
    CHECK(wb != nullptr);
    if (wb == nullptr) return;
    CHECK(wb->special == SpecialBlock::Mermaid);
    const LayoutGraphic& wg = wide.graphics.at(wb->graphic - 1);
    CHECK(wg.diagram != nullptr);
    CHECK_NEAR(wg.scale, 1.0, 1e-12);
    CHECK_NEAR(wg.bounds.size.width, wg.diagram->width, 1e-9);
    CHECK_NEAR(wg.bounds.size.height, wg.diagram->height, 1e-9);
    CHECK_NEAR(wg.bounds.center().x, wb->rect.center().x, 1e-6);

    const double narrow = wg.diagram->width / 3.0;
    const MarkdownLayout fit = lay(kWideFlowchart, narrow);
    const LayoutBlock* fb = firstBlock(fit, LayoutBlockKind::Graphic);
    CHECK(fb != nullptr);
    if (fb == nullptr) return;
    const LayoutGraphic& fg = fit.graphics.at(fb->graphic - 1);
    CHECK(fg.scale < 1.0);
    CHECK_NEAR(fg.bounds.size.width, narrow, 1e-6);
    CHECK_NEAR(fg.bounds.size.height, fg.diagram->height * fg.scale, 1e-9);
    CHECK_NEAR(fb->rect.size.height, fg.bounds.size.height, 1e-9);
    CHECK(fit.contentWidth <= narrow + 1e-6);

    // A height cap (the exporter's page) scales it down as well.
    Typography capped = plainTypography();
    capped.maxGraphicHeight = wg.diagram->height / 2.0;
    const MarkdownLayout c = lay(kWideFlowchart, 5000.0, nullptr, capped);
    CHECK_NEAR(c.graphics.at(0).bounds.size.height, capped.maxGraphicHeight, 1e-6);
}

RIVET_TEST(unrenderableDiagramFallsBackToCodeWithANotice) {
    const MarkdownLayout l = lay("```mermaid\ngantt\n  title Plan\n```\n", 600.0);
    CHECK(l.graphics.empty());
    const LayoutBlock* code = firstBlock(l, LayoutBlockKind::Code);
    CHECK(code != nullptr);
    if (code == nullptr) return;
    CHECK(code->special == SpecialBlock::Mermaid);
    CHECK(code->readOnlyIsland());
    std::size_t notices = 0;
    double noticeBottom = 0.0;
    double codeTop = 0.0;
    for (std::uint32_t d = code->decoBegin; d < code->decoEnd; ++d) {
        const Decoration& deco = l.decorations[d];
        if (deco.kind == DecorationKind::Notice) {
            ++notices;
            CHECK(!deco.text.empty());
            CHECK(deco.text.find("Diagram") == 0);
            noticeBottom = deco.rect.maxY();
            CHECK(deco.baseline > deco.rect.minY() && deco.baseline < deco.rect.maxY());
        } else if (deco.kind == DecorationKind::CodeBackground) {
            codeTop = deco.rect.minY();
        }
    }
    CHECK_EQ(notices, std::size_t{1});
    CHECK(codeTop >= noticeBottom);
    std::string text;
    for (std::uint32_t ln = code->lineBegin; ln < code->lineEnd; ++ln) {
        for (std::uint32_t r = l.lines[ln].runBegin; r < l.lines[ln].runEnd; ++r) text += l.runs[r].text + "\n";
    }
    CHECK(text.find("gantt") != std::string::npos);
    CHECK_NEAR(code->rect.minY(), l.decorations[code->decoBegin].rect.minY(), 1e-9);
}

RIVET_TEST(scenesAreCachedAcrossEditsElsewhereAndResizes) {
    MeasureCache cache;
    const std::string body = "```mermaid\nsequenceDiagram\n  Alice->>Bob: Hello\n  Bob-->>Alice: Hi\n```\n\n"
                             "Inline $a^2+b^2=c^2$ here.\n\n$$\n\\sum_{k=1}^n k = \\frac{n(n+1)}{2}\n$$\n";
    (void)lay("Intro\n\n" + body, 800.0, &cache);
    const std::size_t rendered = cache.graphicsRendered();
    CHECK_EQ(rendered, std::size_t{3});
    // Typing elsewhere and resizing reuse the scenes.
    (void)lay("Intro, edited\n\n" + body, 800.0, &cache);
    (void)lay("Intro, edited\n\n" + body, 420.0, &cache);
    CHECK_EQ(cache.graphicsRendered(), rendered);
    // Editing a formula renders that one only.
    std::string changed = body;
    changed.replace(changed.find("a^2"), 3, "x^3");
    (void)lay("Intro\n\n" + changed, 800.0, &cache);
    CHECK_EQ(cache.graphicsRendered(), rendered + 1);
    cache.clear();
    CHECK_EQ(cache.graphicCount(), std::size_t{0});
}
