// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "MarkdownTestKit.hpp"

#include "markdown/MarkdownPaginate.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

// Pagination of Markdown layouts (ADR-0023) with the fixed-width fake
// measurer: lines and other atoms are never cut, oversized atoms are split
// at page boundaries, pages are filled greedily.

using namespace rivet::markdown;
using namespace rivet::markdown::testkit;

namespace {

MarkdownLayout lay(const std::string& src, double width, const IImageSizeProvider* images = nullptr) {
    static const FixedMeasurer measurer;
    return layoutMarkdown(parse(src), width, measurer, images, plainTypography());
}

// Slices tile [0, contentHeight) in order.
void checkTiling(const MarkdownLayout& layout, const std::vector<PageSlice>& slices, double pageHeight) {
    CHECK(!slices.empty());
    if (slices.empty()) return;
    CHECK_EQ(slices.front().top, 0.0);
    CHECK_NEAR(slices.back().bottom, layout.contentHeight, 1e-9);
    for (std::size_t i = 0; i < slices.size(); ++i) {
        CHECK_GT(slices[i].bottom, slices[i].top);
        CHECK_LE(slices[i].bottom - slices[i].top, pageHeight + 1e-6);
        if (i > 0) CHECK_EQ(slices[i].top, slices[i - 1].bottom);
    }
}

bool straddles(double top, double bottom, double cut) {
    return top < cut && cut < bottom;
}

} // namespace

RIVET_TEST(paginateEmptyDocument) {
    const MarkdownLayout layout = lay("", 300);
    const std::vector<PageSlice> slices = paginateLayout(layout, 500.0);
    CHECK_EQ(slices.size(), 1u);
    CHECK_EQ(slices[0].top, 0.0);
    CHECK_EQ(slices[0].bottom, 0.0);
}

RIVET_TEST(paginateNonPositivePageHeightGivesOneSlice) {
    const MarkdownLayout layout = lay("one\n\ntwo\n\nthree\n", 300);
    for (const double h : {0.0, -5.0, std::numeric_limits<double>::quiet_NaN()}) {
        const std::vector<PageSlice> slices = paginateLayout(layout, h);
        CHECK_EQ(slices.size(), 1u);
        CHECK_EQ(slices[0].top, 0.0);
        CHECK_EQ(slices[0].bottom, layout.contentHeight);
    }
}

RIVET_TEST(paginateNeverCutsParagraphLines) {
    // A long paragraph wrapped into many lines; a page height that is not a
    // multiple of the line height.
    std::string text;
    for (int i = 0; i < 400; ++i) text += "word ";
    const MarkdownLayout layout = lay(text + "\n\n- item one\n- item two\n\n---\n\n" + text + "\n", 200);
    CHECK_GT(layout.lines.size(), 50u);
    const double pageHeight = 100.0;
    const std::vector<PageSlice> slices = paginateLayout(layout, pageHeight);
    checkTiling(layout, slices, pageHeight);
    CHECK_GT(slices.size(), 5u);
    for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
        const double cut = slices[i].bottom;
        for (const LayoutLine& line : layout.lines) CHECK(!straddles(line.top, line.top + line.height, cut));
        for (const Decoration& d : layout.decorations) {
            if (d.kind == DecorationKind::CodeBackground || d.kind == DecorationKind::QuoteBar) continue;
            CHECK(!straddles(d.rect.origin.y, d.rect.maxY(), cut));
        }
    }
    // Greedy: each page holds as many whole lines as fit; the next line
    // would not have fit.
    const double lineHeight = layout.lines[0].height;
    CHECK_GT(slices[0].bottom - slices[0].top, pageHeight - lineHeight - 1e-9);
}

RIVET_TEST(paginateSplitsTallImageAtPageBoundaries) {
    FakeImages images;
    ImageInfo tall;
    tall.state = ImageState::Known;
    tall.width = 100;
    tall.height = 400; // laid out at 400 (below maxImageHeight 480)
    images.known.insert_or_assign("tall.png", tall);
    const MarkdownLayout layout = lay("Intro line\n\n![t](tall.png)\n\nAfter\n", 300, &images);
    std::size_t imageBlock = layout.blocks.size();
    for (std::size_t i = 0; i < layout.blocks.size(); ++i) {
        if (layout.blocks[i].kind == LayoutBlockKind::Image) imageBlock = i;
    }
    CHECK(imageBlock < layout.blocks.size());
    if (imageBlock >= layout.blocks.size()) return;
    const double imageTop = layout.blocks[imageBlock].rect.origin.y;
    const double imageBottom = layout.blocks[imageBlock].rect.maxY();
    const double pageHeight = 150.0;
    const std::vector<PageSlice> slices = paginateLayout(layout, pageHeight);
    checkTiling(layout, slices, pageHeight);
    // The intro page ends at the image's top (the image cannot fit after the
    // intro), then the image fills full pages.
    CHECK_NEAR(slices[0].bottom, imageTop, 1e-9);
    CHECK_NEAR(slices[1].top, imageTop, 1e-9);
    CHECK_NEAR(slices[1].bottom - slices[1].top, pageHeight, 1e-9);
    CHECK_NEAR(slices[2].bottom - slices[2].top, pageHeight, 1e-9);
    std::size_t cutsInside = 0;
    for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
        cutsInside += straddles(imageTop, imageBottom, slices[i].bottom) ? 1u : 0u;
    }
    CHECK_EQ(cutsInside, 2u); // 400 / 150 -> three pieces
}

RIVET_TEST(paginateFillsPagesGreedilyWithSmallBlocks) {
    std::string src;
    for (int i = 0; i < 60; ++i) src += "Paragraph " + std::to_string(i) + "\n\n";
    const MarkdownLayout layout = lay(src, 400);
    CHECK_EQ(layout.lines.size(), 60u);
    const double pageHeight = 300.0;
    const std::vector<PageSlice> slices = paginateLayout(layout, pageHeight);
    checkTiling(layout, slices, pageHeight);
    for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
        // Greedy: the first line after the cut would have overflowed this page.
        const double cut = slices[i].bottom;
        const LayoutLine* next = nullptr;
        for (const LayoutLine& line : layout.lines) {
            if (line.top >= cut - 1e-9) {
                next = &line;
                break;
            }
        }
        CHECK(next != nullptr);
        if (next != nullptr) CHECK_GT(next->top + next->height, slices[i].top + pageHeight);
    }
    // Slicing the same layout with one big page gives one slice.
    CHECK_EQ(paginateLayout(layout, layout.contentHeight + 1.0).size(), 1u);
    CHECK_EQ(paginateLayout(layout, layout.contentHeight).size(), 1u);
}

RIVET_TEST(paginateKeepsTableCellLinesWhole) {
    std::string src = "| a | b |\n|---|---|\n";
    for (int i = 0; i < 40; ++i) src += "| cell " + std::to_string(i) + " | some longer text here |\n";
    const MarkdownLayout layout = lay(src, 300);
    CHECK(!layout.tableCells.empty());
    const double pageHeight = 123.0;
    const std::vector<PageSlice> slices = paginateLayout(layout, pageHeight);
    checkTiling(layout, slices, pageHeight);
    for (std::size_t i = 0; i + 1 < slices.size(); ++i) {
        for (const TableCellLayout& cell : layout.tableCells) {
            for (std::uint32_t l = cell.lineBegin; l < cell.lineEnd; ++l) {
                const LayoutLine& line = layout.lines[l];
                CHECK(!straddles(line.top, line.top + line.height, slices[i].bottom));
            }
        }
    }
}
