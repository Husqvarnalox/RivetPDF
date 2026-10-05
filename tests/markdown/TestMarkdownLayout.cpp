// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "MarkdownTestKit.hpp"

#include <chrono>

using namespace rivet::markdown;
using namespace rivet::markdown::testkit;
using rivet::core::Point;

namespace {

MarkdownLayout lay(const MarkdownDocument& d, double width, const FixedMeasurer& m, const IImageSizeProvider* im = nullptr,
                   MeasureCache* c = nullptr) {
    return layoutMarkdown(d, width, m, im, plainTypography(), c);
}

std::string lineText(const MarkdownLayout& l, std::size_t line) {
    std::string s;
    for (std::uint32_t r = l.lines[line].runBegin; r < l.lines[line].runEnd; ++r) s += l.runs[r].text;
    return s;
}

TextPosition endOf(const MarkdownLayout& l) {
    const std::uint32_t r = static_cast<std::uint32_t>(l.runs.size() - 1);
    return {r, static_cast<std::uint32_t>(l.runs[r].text.size())};
}

} // namespace

RIVET_TEST(layoutEmptyDocument) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse(""), 300, m);
    CHECK(l.blocks.empty());
    CHECK(l.lines.empty());
    CHECK_EQ(l.blockIndexAtY(10), static_cast<std::size_t>(-1));
    CHECK(!l.hitTest({5, 5}, m).valid);
    CHECK(l.selectedText({0, 0}, {0, 0}).empty());
}

RIVET_TEST(layoutWrapsAtWordBoundaries) {
    FixedMeasurer m;
    // 10 px per character; width 100 holds 10 characters.
    const MarkdownDocument doc = parse("aaaa bbbb cccc dddd\n");
    const MarkdownLayout l = lay(doc, 100, m);
    CHECK_EQ(l.lines.size(), 2u);
    CHECK_EQ(lineText(l, 0), "aaaa bbbb");
    CHECK_EQ(lineText(l, 1), "cccc dddd");
    CHECK_EQ(l.lines[0].end, LineEnd::Wrap);
    CHECK_EQ(l.lines[1].end, LineEnd::BlockEnd);
    CHECK_GT(l.lines[1].top, l.lines[0].top);
    CHECK_NEAR(l.lines[0].height, 15.0 * 1.45, 0.5);
    for (const LayoutLine& ln : l.lines) CHECK_LE(ln.x + ln.width, 100.0 + 10.0); // trailing space may hang
}

RIVET_TEST(layoutBreaksLongWordsPerCodePoint) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse("abcdefghijklmnopqrstuvwxyz\n"), 100, m);
    CHECK_EQ(l.lines.size(), 3u);
    CHECK_EQ(lineText(l, 0), "abcdefghij");
    CHECK_EQ(lineText(l, 2), "uvwxyz");
    const MarkdownLayout c = lay(parse("абвгдежзийклмнопрстуф\n"), 100, m);
    CHECK_EQ(c.lines.size(), 3u);
    CHECK_EQ(lineText(c, 0), "абвгдежзий"); // never splits inside a UTF-8 sequence
}

RIVET_TEST(layoutNarrowerIsTaller) {
    FixedMeasurer m;
    const MarkdownDocument doc = parse(generateMarkdown(20 * 1024));
    MeasureCache cache;
    const MarkdownLayout wide = lay(doc, 800, m, nullptr, &cache);
    const MarkdownLayout narrow = lay(doc, 300, m, nullptr, &cache);
    CHECK_GT(narrow.contentHeight, wide.contentHeight);
    CHECK_GT(narrow.lines.size(), wide.lines.size());
    CHECK_EQ(narrow.blocks.size(), wide.blocks.size());
    // Degenerate widths never crash or produce empty lines forever.
    const MarkdownLayout tiny = lay(doc, 1, m, nullptr, &cache);
    CHECK_GT(tiny.contentHeight, narrow.contentHeight);
    const MarkdownLayout zero = lay(doc, 0, m, nullptr, &cache);
    CHECK_GT(zero.lines.size(), 0u);
}

RIVET_TEST(layoutHeadingsUseHeadingStyleAndSize) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse("# Big\n\n###### Small\n\ntext\n"), 400, m);
    CHECK_EQ(l.blocks.size(), 3u);
    CHECK_EQ(l.blocks[0].kind, LayoutBlockKind::Heading);
    CHECK_EQ(l.blocks[0].headingLevel, 1);
    CHECK_EQ(l.runs[l.lines[l.blocks[0].lineBegin].runBegin].style.kind, TextKind::H1);
    CHECK_NEAR(l.runs[l.lines[l.blocks[0].lineBegin].runBegin].style.size, 28.0, 0.01);
    CHECK_EQ(l.blocks[1].headingLevel, 6);
    CHECK_NEAR(l.runs[l.lines[l.blocks[1].lineBegin].runBegin].style.size, 14.0, 0.01);
    CHECK_GT(l.blocks[0].rect.size.height, l.blocks[2].rect.size.height);
    // Block order is ascending y with gaps.
    CHECK_LT(l.blocks[0].rect.maxY(), l.blocks[1].rect.minY() + 0.01);
    CHECK_LT(l.blocks[1].rect.maxY(), l.blocks[2].rect.minY() + 0.01);
}

RIVET_TEST(layoutInlineStylesAndLinks) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse("a **b** *c* `d` ~~e~~ [f](http://x.y \"T\")\n"), 600, m);
    bool bold = false, italic = false, mono = false, strike = false, link = false;
    for (const LayoutRun& r : l.runs) {
        bold |= r.style.bold && r.text == "b";
        italic |= r.style.italic && r.text == "c";
        mono |= r.style.monospace && r.text == "d";
        strike |= r.style.strike && r.text == "e";
        if (r.text == "f") {
            CHECK(r.link != 0);
            CHECK_EQ(l.links[r.link - 1].url, "http://x.y");
            CHECK_EQ(l.links[r.link - 1].title, "T");
            link = true;
        }
    }
    CHECK(bold && italic && mono && strike && link);
    // Runs on a line are laid out left to right without overlap.
    for (std::uint32_t i = l.lines[0].runBegin + 1; i < l.lines[0].runEnd; ++i)
        CHECK_GE(l.runs[i].x, l.runs[i - 1].x + l.runs[i - 1].width - 0.01);
}

RIVET_TEST(layoutNestedListIndent) {
    FixedMeasurer m;
    const MarkdownDocument doc = parse(readFixture("nested-lists.md"));
    const MarkdownLayout l = lay(doc, 600, m);
    // Four nested levels: the text x of each is strictly increasing by at least listIndent.
    double prevX = -1.0;
    int found = 0;
    for (const LayoutRun& r : l.runs) {
        if (r.text.rfind("level", 0) == 0) {
            if (prevX >= 0) CHECK_GE(r.x, prevX + Typography{}.listIndent - 0.01);
            prevX = r.x;
            if (++found == 4) break;
        }
    }
    CHECK_EQ(found, 4);
    std::size_t bullets = 0;
    for (const Decoration& d : l.decorations) bullets += d.kind == DecorationKind::ListBullet;
    CHECK_GE(bullets, 6u);
}

RIVET_TEST(layoutListNumbersAndTaskCheckboxes) {
    FixedMeasurer m;
    const MarkdownLayout lo = lay(parse("5. a\n6. b\n"), 400, m);
    std::vector<std::string> labels;
    for (const Decoration& d : lo.decorations) {
        if (d.kind == DecorationKind::ListNumber) labels.push_back(d.text);
    }
    CHECK_EQ(labels.size(), 2u);
    CHECK_EQ(labels[0], "5.");
    CHECK_EQ(labels[1], "6.");
    const MarkdownLayout lt = lay(parse(readFixture("task-list.md")), 400, m);
    int checked = 0, unchecked = 0;
    for (const Decoration& d : lt.decorations) {
        if (d.kind == DecorationKind::TaskCheckbox) (d.checked ? checked : unchecked)++;
    }
    CHECK_EQ(checked, 2);
    CHECK_EQ(unchecked, 1);
}

RIVET_TEST(layoutQuoteIndentAndBar) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse("> quoted text\n\nplain\n"), 400, m);
    CHECK_EQ(l.blocks.size(), 2u);
    const LayoutRun& q = l.runs[l.lines[l.blocks[0].lineBegin].runBegin];
    const LayoutRun& p = l.runs[l.lines[l.blocks[1].lineBegin].runBegin];
    CHECK_NEAR(p.x, 0.0, 0.01);
    CHECK_GE(q.x, Typography{}.quoteIndent - 0.01);
    int bars = 0;
    for (const Decoration& d : l.decorations) {
        if (d.kind != DecorationKind::QuoteBar) continue;
        ++bars;
        CHECK_LT(d.rect.minX(), q.x);
        CHECK_GE(d.rect.size.height, l.blocks[0].rect.size.height - 0.5);
    }
    CHECK_EQ(bars, 1);
    // Nested quotes indent further.
    const MarkdownLayout n = lay(parse("> > deep\n"), 400, m);
    CHECK_GE(n.runs[0].x, 2 * Typography{}.quoteIndent - 0.01);
}

RIVET_TEST(layoutCodeBlockDoesNotWrapAndReportsOverflow) {
    FixedMeasurer m;
    const std::string longLine(60, 'x'); // 60 * (13*2/3) = 520 wide
    const MarkdownDocument doc = parse("```\n" + longLine + "\nshort\n```\n");
    const MarkdownLayout l = lay(doc, 200, m);
    CHECK_EQ(l.blocks.size(), 1u);
    const LayoutBlock& b = l.blocks[0];
    CHECK_EQ(b.kind, LayoutBlockKind::Code);
    CHECK_EQ(b.lineEnd - b.lineBegin, 2u);
    CHECK(b.overflowsHorizontally());
    CHECK_GT(b.contentWidth, 500.0);
    CHECK_NEAR(b.rect.size.width, 200.0, 0.01);
    CHECK_GT(l.contentWidth, 200.0);
    for (std::uint32_t i = b.lineBegin; i < b.lineEnd; ++i) {
        CHECK(l.runs[l.lines[i].runBegin].style.monospace);
        CHECK_EQ(l.lines[i].end, LineEnd::CodeLine);
    }
    const Decoration* bg = nullptr;
    for (const Decoration& d : l.decorations) {
        if (d.kind == DecorationKind::CodeBackground) bg = &d;
    }
    CHECK(bg != nullptr);
    CHECK_NEAR(bg->rect.minX(), b.rect.minX(), 0.01);
    CHECK_NEAR(bg->rect.size.width, b.rect.size.width, 0.01);
    CHECK_NEAR(bg->rect.size.height, b.rect.size.height, 0.01);
    // Text starts inside the padding.
    CHECK_NEAR(l.runs[l.lines[b.lineBegin].runBegin].x, b.rect.minX() + Typography{}.codePadding, 0.01);
    // Widening the viewport removes the overflow.
    const MarkdownLayout wide = lay(doc, 800, m);
    CHECK(!wide.blocks[0].overflowsHorizontally());
}

RIVET_TEST(layoutTableGeometry) {
    FixedMeasurer m;
    const MarkdownDocument doc = parse(readFixture("tables.md"));
    const MarkdownLayout l = lay(doc, 600, m);
    CHECK_EQ(l.blocks.size(), 1u);
    const LayoutBlock& b = l.blocks[0];
    CHECK_EQ(b.kind, LayoutBlockKind::Table);
    CHECK_EQ(b.cellEnd - b.cellBegin, 12u); // header + 3 rows x 3 columns
    // Cells of a column share x and width; cells of a row share y and height.
    const TableCellLayout* hdr[3] = {};
    for (std::uint32_t i = b.cellBegin; i < b.cellEnd; ++i) {
        const TableCellLayout& c = l.tableCells[i];
        if (c.row == 0) hdr[c.col] = &c;
        CHECK_NEAR(c.rect.minX(), hdr[c.col]->rect.minX(), 0.01);
        CHECK_NEAR(c.rect.size.width, hdr[c.col]->rect.size.width, 0.01);
        CHECK_GE(c.rect.minY(), b.rect.minY() - 0.01);
        CHECK_LE(c.rect.maxY(), b.rect.maxY() + 0.01);
    }
    CHECK(hdr[0]->header);
    CHECK_EQ(hdr[1]->align, ColumnAlign::Center);
    CHECK(hdr[1]->rect.minX() >= hdr[0]->rect.maxX() - 0.01);
    CHECK(!b.overflowsHorizontally());
    // A very narrow viewport makes the table overflow instead of squashing it.
    const MarkdownLayout narrow = lay(doc, 60, m);
    CHECK(narrow.blocks[0].overflowsHorizontally());
    CHECK_GT(narrow.contentWidth, 60.0);
}

RIVET_TEST(layoutImagesClampAndPlaceholders) {
    FixedMeasurer m;
    FakeImages imgs;
    ImageInfo known;
    known.state = ImageState::Known;
    known.width = 1000;
    known.height = 500;
    imgs.known["a.png"] = known;
    ImageInfo tall;
    tall.state = ImageState::Known;
    tall.width = 100;
    tall.height = 2000;
    imgs.known["tall.png"] = tall;
    ImageInfo blocked;
    blocked.state = ImageState::Blocked;
    imgs.known["bad.png"] = blocked;
    const MarkdownDocument doc = parse("![a](a.png)\n\n![t](tall.png)\n\n![b](bad.png)\n\n![u](unknown.png)\n");
    const MarkdownLayout l = lay(doc, 400, m, &imgs);
    CHECK_EQ(l.blocks.size(), 4u);
    CHECK_EQ(l.blocks[0].kind, LayoutBlockKind::Image);
    CHECK_NEAR(l.blocks[0].rect.size.width, 400.0, 0.5);
    CHECK_NEAR(l.blocks[0].rect.size.height, 200.0, 0.5); // aspect preserved
    CHECK_LE(l.blocks[1].rect.size.height, Typography{}.maxImageHeight + 0.5);
    CHECK_NEAR(l.blocks[1].rect.size.width / l.blocks[1].rect.size.height, 100.0 / 2000.0, 0.01);
    CHECK_NEAR(l.blocks[2].rect.size.height, Typography{}.imageBlockedHeight, 0.5);
    CHECK_NEAR(l.blocks[3].rect.size.height, Typography{}.imagePlaceholderHeight, 0.5);
    std::size_t blockedCount = 0, unknownCount = 0;
    for (const Decoration& d : l.decorations) {
        if (d.kind != DecorationKind::ImagePlaceholder) continue;
        blockedCount += d.imageState == ImageState::Blocked;
        unknownCount += d.imageState == ImageState::Unknown;
    }
    CHECK_EQ(blockedCount, 1u);
    CHECK_EQ(unknownCount, 1u);
    // Known images in a narrow viewport shrink to fit.
    const MarkdownLayout n = lay(doc, 100, m, &imgs);
    CHECK_LE(n.blocks[0].rect.size.width, 100.0 + 0.5);
}

RIVET_TEST(layoutHorizontalRule) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse("a\n\n---\n\nb\n"), 300, m);
    CHECK_EQ(l.blocks.size(), 3u);
    CHECK_EQ(l.blocks[1].kind, LayoutBlockKind::Rule);
    bool rule = false;
    for (const Decoration& d : l.decorations) rule |= d.kind == DecorationKind::Rule;
    CHECK(rule);
}

RIVET_TEST(layoutLookupByYAndSource) {
    FixedMeasurer m;
    const std::string src = "# T\n\nfirst para\n\nsecond para\n\nthird para\n";
    const MarkdownDocument doc = parse(src);
    const MarkdownLayout l = lay(doc, 300, m);
    CHECK_EQ(l.blocks.size(), 4u);
    for (std::size_t i = 0; i < l.blocks.size(); ++i) {
        CHECK_EQ(l.blockIndexAtY(l.blocks[i].rect.minY() + 1.0), i);
        CHECK_EQ(l.blockIndexAtSource(l.blocks[i].source.start), i);
        CHECK_NEAR(l.yForBlockId(l.blocks[i].blockId), l.blocks[i].rect.minY(), 0.01);
    }
    CHECK_EQ(l.blockIndexAtY(-50.0), 0u);
    CHECK_EQ(l.blockIndexAtY(1e9), 3u);
    CHECK_NEAR(l.yForSourceOffset(src.find("second")), l.blocks[2].rect.minY(), 20.0);
    CHECK_LT(l.yForBlockId(99999), 0.0);
    const auto range = l.blockRangeForY(l.blocks[1].rect.minY() + 1, l.blocks[2].rect.minY() + 1);
    CHECK_EQ(range.first, 1u);
    CHECK_EQ(range.second, 3u);
    const auto none = l.blockRangeForY(1e8, 1e9);
    CHECK_EQ(none.first, none.second);
    CHECK(!l.decorationsInRange(0, 1e9).empty() || l.decorations.empty());
}

RIVET_TEST(layoutHitTestAndCaretX) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse("abcdefghij klmno\n"), 100, m);
    CHECK_EQ(l.lines.size(), 2u);
    // Click in the middle of the 4th character on line 0 (x in [30,40)).
    HitResult h = l.hitTest({34.0, l.lines[0].top + 5.0}, m);
    CHECK(h.valid);
    CHECK(h.onText);
    CHECK_EQ(h.line, 0u);
    CHECK_EQ(h.position.offset, 3u); // caret snaps to the nearer boundary (x=30)
    h = l.hitTest({37.0, l.lines[0].top + 5.0}, m);
    CHECK_EQ(h.position.offset, 4u);
    CHECK_NEAR(l.xForPosition(h.position, m), 40.0, 0.01);
    // Past the end of a line clamps to its end; below the doc hits the last line.
    h = l.hitTest({500.0, l.lines[1].top + 2.0}, m);
    CHECK(h.valid);
    CHECK(!h.onText);
    h = l.hitTest({5.0, 1e6}, m);
    CHECK(h.valid);
    CHECK_EQ(h.line, 1u);
    // Link hit.
    const MarkdownLayout lk = lay(parse("[go](http://a.b)\n"), 300, m);
    const HitResult lh = lk.hitTest({5.0, lk.lines[0].top + 5.0}, m);
    CHECK(lh.link != 0);
    CHECK_EQ(lk.links[lh.link - 1].url, "http://a.b");
    // Cyrillic positions stay on code point boundaries.
    const MarkdownLayout cy = lay(parse("привет\n"), 300, m);
    const HitResult ch = cy.hitTest({25.0, cy.lines[0].top + 5.0}, m);
    CHECK_EQ(ch.position.offset % 2, 0u);
}

RIVET_TEST(layoutSelectionAcrossLinesAndBlocks) {
    FixedMeasurer m;
    const std::string src = "aaaa bbbb cccc dddd\n\nsecond block\n";
    const MarkdownDocument doc = parse(src);
    const MarkdownLayout l = lay(doc, 100, m);
    const TextPosition a{0, 0};
    // Whole document, in both orders.
    const std::string all = l.selectedText(a, endOf(l));
    CHECK_EQ(all, "aaaa bbbb cccc dddd\nsecond block");
    CHECK_EQ(l.selectedText(endOf(l), a), all);
    CHECK(l.selectedText(a, a).empty());
    const auto rects = l.selectionRects(a, endOf(l), m);
    CHECK_GE(rects.size(), l.lines.size());
    for (const auto& r : rects) {
        CHECK_GT(r.size.width, 0.0);
        CHECK_GT(r.size.height, 0.0);
    }
    const SourceRange sr = l.selectedSource(a, endOf(l));
    CHECK_EQ(sr.start, 0u);
    CHECK_GE(sr.end, src.find("block") + 5 - 1);
    CHECK_EQ(l.sourceOffsetAt(a), 0u);
}

RIVET_TEST(layoutSelectionInTables) {
    FixedMeasurer m;
    const std::string src = "| a | b |\n|---|---|\n| c | d |\n";
    const MarkdownLayout l = lay(parse(src), 400, m);
    const std::string all = l.selectedText({0, 0}, endOf(l));
    CHECK_EQ(all, "a\tb\nc\td");
}

RIVET_TEST(layoutSelectionInCodeKeepsNewlines) {
    FixedMeasurer m;
    const MarkdownLayout l = lay(parse("```\nline1\nline2\n```\n"), 400, m);
    CHECK_EQ(l.selectedText({0, 0}, endOf(l)), "line1\nline2");
}

RIVET_TEST(layoutRelayoutWithCacheIsMeasurementFree) {
    FixedMeasurer m;
    const MarkdownDocument doc = parse(generateMarkdown(50 * 1024));
    MeasureCache cache;
    lay(doc, 500, m, nullptr, &cache);
    const std::size_t afterFirst = m.calls;
    CHECK_GT(afterFirst, 0u);
    const MarkdownLayout again = lay(doc, 420, m, nullptr, &cache);
    // Only words broken at new positions may need new measurements; far fewer than the first pass.
    CHECK_LT(m.calls - afterFirst, afterFirst / 4 + 1);
    CHECK_GT(again.lines.size(), 0u);
    // Identical width: no new measurements at all.
    const std::size_t before = m.calls;
    lay(doc, 420, m, nullptr, &cache);
    CHECK_EQ(m.calls, before);
}

RIVET_TEST(layoutScalesLinearly) {
    const auto measure = [](std::size_t bytes) {
        FixedMeasurer m;
        const MarkdownDocument doc = parse(generateMarkdown(bytes));
        const MarkdownLayout l = lay(doc, 600, m);
        return std::pair<std::size_t, std::size_t>(m.calls, l.lines.size());
    };
    const auto small = measure(100 * 1024);
    const auto large = measure(400 * 1024);
    const double callRatio = static_cast<double>(large.first) / static_cast<double>(small.first);
    CHECK_GT(callRatio, 2.0);
    CHECK_LT(callRatio, 8.0); // quadratic behaviour would give ~16
    const auto t0 = std::chrono::steady_clock::now();
    FixedMeasurer m;
    const MarkdownDocument doc = parse(generateMarkdown(1024 * 1024));
    const MarkdownLayout l = lay(doc, 600, m);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK_GT(l.contentHeight, 1000.0);
    CHECK_LT(secs, 30.0); // generous: catches pathological complexity, not regressions
}
