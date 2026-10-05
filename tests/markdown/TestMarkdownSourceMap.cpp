// SPDX-License-Identifier: MPL-2.0
// Source <-> preview position mapping used by split-view scroll sync.
#include "RivetTest.h"

#include "MarkdownTestKit.hpp"
#include "markdown/MarkdownSourceMap.hpp"

#include <string>

using namespace rivet::markdown;
using namespace rivet::markdown::testkit;

namespace {

MarkdownLayout layoutOf(const std::string& src, double width = 600.0) {
    const FixedMeasurer m;
    return layoutMarkdown(parse(src), width, m, nullptr, plainTypography());
}

std::string manyParagraphs(int n) {
    std::string s;
    for (int i = 0; i < n; ++i) s += "Paragraph " + std::to_string(i) + " text.\n\n";
    return s;
}

} // namespace

RIVET_TEST(sourceMapEmptyLayoutMapsToZero) {
    const MarkdownLayout layout = layoutOf("");
    CHECK_EQ(previewYForSourceOffset(layout, 10), 0.0);
    CHECK_EQ(sourceOffsetForPreviewY(layout, 10.0), 0u);
}

RIVET_TEST(sourceMapBlockStartsMapToBlockTops) {
    const std::string src = manyParagraphs(10);
    const MarkdownLayout layout = layoutOf(src);
    for (const LayoutBlock& b : layout.blocks) {
        CHECK_EQ(previewYForSourceOffset(layout, b.source.start), b.rect.origin.y);
        CHECK_EQ(sourceOffsetForPreviewY(layout, b.rect.origin.y), b.source.start);
    }
}

RIVET_TEST(sourceMapIsMonotonicBothWays) {
    const std::string src = generateMarkdown(20 * 1024);
    const MarkdownLayout layout = layoutOf(src);
    double lastY = -1.0;
    for (std::size_t off = 0; off < src.size(); off += 37) {
        const double y = previewYForSourceOffset(layout, off);
        CHECK(y >= lastY);
        lastY = y;
    }
    std::size_t lastOffset = 0;
    for (double y = 0.0; y < layout.contentHeight; y += 7.0) {
        const std::size_t off = sourceOffsetForPreviewY(layout, y);
        CHECK(off >= lastOffset);
        lastOffset = off;
    }
}

RIVET_TEST(sourceMapRoundTripStaysInsideTheBlock) {
    const std::string src = generateMarkdown(8 * 1024);
    const MarkdownLayout layout = layoutOf(src);
    for (std::size_t off = 0; off < src.size(); off += 53) {
        const double y = previewYForSourceOffset(layout, off);
        const std::size_t back = sourceOffsetForPreviewY(layout, y);
        const LayoutBlock& block = layout.blocks[layout.blockIndexAtY(y)];
        CHECK(back >= block.source.start && back <= block.source.end);
    }
}

RIVET_TEST(sourceMapInterpolatesInsideTallBlocks) {
    std::string code = "```\n";
    for (int i = 0; i < 100; ++i) code += "line " + std::to_string(i) + "\n";
    code += "```\n\nafter\n";
    const MarkdownLayout layout = layoutOf(code);
    const LayoutBlock& block = layout.blocks.front();
    const double mid = previewYForSourceOffset(layout, block.source.start + block.source.size() / 2);
    CHECK(mid > block.rect.origin.y + block.rect.size.height * 0.4);
    CHECK(mid < block.rect.origin.y + block.rect.size.height * 0.6);
}

RIVET_TEST(sourceMapOutOfRangeInputsAreClamped) {
    const MarkdownLayout layout = layoutOf(manyParagraphs(3));
    CHECK(previewYForSourceOffset(layout, 1'000'000) <= layout.contentHeight);
    CHECK_EQ(sourceOffsetForPreviewY(layout, -50.0), 0u);
    CHECK(sourceOffsetForPreviewY(layout, 1e9) < 1000u);
}
