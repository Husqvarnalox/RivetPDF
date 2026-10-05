// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "MarkdownTestKit.hpp"

#include <algorithm>
#include <cstdio>
#include <thread>

using namespace rivet::markdown;
using namespace rivet::markdown::testkit;

namespace {

bool hasDiag(const MarkdownDocument& d, DiagnosticCode code) {
    for (const Diagnostic& x : d.diagnostics) {
        if (x.code == code) return true;
    }
    return false;
}

// Compact structural dump used to compare two parses.
void dumpInlines(const std::vector<Inline>& v, std::string& out) {
    for (const Inline& in : v) {
        out += "<" + std::to_string(static_cast<int>(in.kind)) + ":" + std::to_string(in.range.start) + "-" +
               std::to_string(in.range.end) + ":" + in.text + ":" + in.url + ">";
        dumpInlines(in.children, out);
    }
}
void dumpBlocks(const std::vector<Block>& v, std::string& out) {
    for (const Block& b : v) {
        out += "[" + std::to_string(static_cast<int>(b.kind)) + ":" + std::to_string(b.range.start) + "-" +
               std::to_string(b.range.end) + ":" + b.slug + ":" + b.text;
        dumpInlines(b.inlines, out);
        dumpBlocks(b.children, out);
        for (const ListItem& it : b.items) dumpBlocks(it.children, out);
        out += "]";
    }
}
std::string dump(const MarkdownDocument& d) {
    std::string s;
    dumpBlocks(d.blocks, s);
    return s;
}

} // namespace

RIVET_TEST(parseBasicParagraphs) {
    const std::string src = readFixture("basic.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 3u);
    CHECK_EQ(doc.blocks[0].kind, BlockKind::Heading);
    CHECK_EQ(doc.blocks[0].level, 1);
    CHECK_EQ(doc.blocks[1].kind, BlockKind::Paragraph);
    CHECK_EQ(doc.blocks[2].kind, BlockKind::Paragraph);
    // The soft break between the first two lines is its own inline.
    const auto& in = doc.blocks[1].inlines;
    CHECK_EQ(in.size(), 3u);
    CHECK_EQ(in[1].kind, InlineKind::SoftBreak);
    CHECK_EQ(doc.blockCount, 3u);
    CHECK_EQ(doc.sourceLength, src.size());
    CHECK(doc.diagnostics.empty());
    CHECK_EQ(slice(src, doc.blocks[2].range), "Another paragraph.");
    CHECK_EQ(slice(src, doc.blocks[0].range), "# Basic");
    CHECK_EQ(inlinesToPlainText(doc.blocks[1].inlines), "A paragraph with a second line that continues here.");
}

RIVET_TEST(parseHeadingsSlugsAndAnchors) {
    const std::string src = readFixture("headings.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.headings.size(), 9u);
    const char* slugs[] = {"one", "two", "three", "four", "five", "six", "two-1", "setext-heading", "hello-world-2024"};
    const int levels[] = {1, 2, 3, 4, 5, 6, 2, 1, 2};
    for (std::size_t i = 0; i < doc.headings.size(); ++i) {
        CHECK_EQ(doc.headings[i].slug, slugs[i]);
        CHECK_EQ(doc.headings[i].level, levels[i]);
    }
    const HeadingEntry* h = doc.findHeadingBySlug("#two-1");
    CHECK(h != nullptr);
    CHECK_EQ(h->level, 2);
    CHECK(doc.findHeadingBySlug("two") != nullptr);
    CHECK(doc.findHeadingBySlug("missing") == nullptr);
    CHECK(doc.findHeadingBySlug("") == nullptr);
    // Setext heading range includes the underline.
    CHECK_EQ(slice(src, doc.headings[7].range), "Setext Heading\n===============");
    // blockId links back to the block.
    CHECK_EQ(doc.blocks[0].id, doc.headings[0].blockId);
    CHECK_EQ(doc.headings[7].text, "Setext Heading");
}

RIVET_TEST(parseInlineNodes) {
    const std::string src = readFixture("emphasis.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 3u);
    const auto& in = doc.blocks[0].inlines;
    CHECK_EQ(in[0].kind, InlineKind::Text);
    CHECK_EQ(in[1].kind, InlineKind::Emphasis);
    CHECK_EQ(slice(src, in[1].range), "*emphasis*");
    CHECK_EQ(in[3].kind, InlineKind::Strong);
    CHECK_EQ(slice(src, in[3].range), "**strong**");
    // ***both*** nests em/strong; the outer range includes all three markers.
    CHECK_EQ(slice(src, in[5].range), "***both***");
    CHECK_EQ(in[5].children.size(), 1u);
    CHECK_EQ(slice(src, in[7].range), "~~strike~~");
    CHECK_EQ(in[7].kind, InlineKind::Strike);
    CHECK_EQ(in[9].kind, InlineKind::Code);
    CHECK_EQ(in[9].text, "code");
    CHECK_EQ(slice(src, in[9].range), "`code`");
    // Escapes and entities are decoded into the text.
    CHECK_EQ(inlinesToPlainText(doc.blocks[1].inlines), "Escapes: *not emphasis* & \xC2\xA9 # \xF0\x9F\x98\x80");
    // Hard breaks from two spaces and from a backslash.
    std::size_t hard = 0;
    for (const Inline& i : doc.blocks[2].inlines) hard += i.kind == InlineKind::HardBreak;
    CHECK_EQ(hard, 2u);
}

RIVET_TEST(parseLinksImagesAutolinks) {
    const std::string src = readFixture("links.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 2u);
    const auto& in = doc.blocks[0].inlines;
    // [link](...) with title.
    CHECK_EQ(in[1].kind, InlineKind::Link);
    CHECK_EQ(in[1].url, "https://example.com");
    CHECK_EQ(in[1].title, "Title");
    CHECK_EQ(slice(src, in[1].range), "[link](https://example.com \"Title\")");
    // <autolink>, www. and e-mail autolinks.
    CHECK_EQ(in[3].url, "https://auto.example.org");
    CHECK_EQ(slice(src, in[3].range), "<https://auto.example.org>");
    CHECK_EQ(in[5].url, "http://www.example.net");
    CHECK_EQ(in[7].url, "mailto:mail@example.com");
    // Reference link and image in the second paragraph.
    const auto& in2 = doc.blocks[1].inlines;
    CHECK_EQ(in2[0].kind, InlineKind::Link);
    CHECK_EQ(in2[0].url, "https://ref.example.com");
    CHECK_EQ(slice(src, in2[0].range), "[ref link][r]");
    const Inline* img = nullptr;
    for (const Inline& i : in2) {
        if (i.kind == InlineKind::Image) img = &i;
    }
    CHECK(img != nullptr);
    CHECK_EQ(img->url, "pic.png");
    CHECK_EQ(img->title, "Pic");
    CHECK_EQ(img->text, "alt text");
    CHECK_EQ(slice(src, img->range), "![alt text](pic.png \"Pic\")");
}

RIVET_TEST(parseImageBlocks) {
    const std::string src = readFixture("local-image.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 4u);
    CHECK_EQ(doc.blocks[1].kind, BlockKind::ImageBlock);
    CHECK_EQ(doc.blocks[1].inlines.size(), 1u);
    CHECK_EQ(doc.blocks[1].inlines[0].url, "images/diagram.png");
    CHECK_EQ(doc.blocks[1].inlines[0].text, "Local diagram");
    CHECK_EQ(doc.blocks[2].kind, BlockKind::Paragraph); // image inside text stays a paragraph
    CHECK_EQ(doc.blocks[3].kind, BlockKind::ImageBlock);
    CHECK_EQ(doc.blocks[3].inlines[0].text, "");
    CHECK_EQ(slice(src, doc.blocks[3].range), "![](no-alt.png)");
}

RIVET_TEST(parseListsAndTasks) {
    {
        const std::string src = readFixture("lists.md");
        const MarkdownDocument doc = parse(src);
        CHECK_EQ(doc.blocks.size(), 3u);
        CHECK(!doc.blocks[0].ordered);
        CHECK(doc.blocks[0].tight);
        CHECK_EQ(doc.blocks[0].items.size(), 3u);
        CHECK(doc.blocks[1].ordered);
        CHECK_EQ(doc.blocks[1].start, 1u);
        CHECK_EQ(doc.blocks[2].start, 5u);
        CHECK_EQ(slice(src, doc.blocks[0].items[1].range), "- banana");
        CHECK_EQ(doc.blocks[0].items[0].children.size(), 1u);
        CHECK_EQ(doc.blocks[0].items[0].children[0].kind, BlockKind::Paragraph);
    }
    {
        const std::string src = readFixture("task-list.md");
        const MarkdownDocument doc = parse(src);
        CHECK_EQ(doc.blocks.size(), 1u);
        const auto& items = doc.blocks[0].items;
        CHECK_EQ(items.size(), 4u);
        CHECK(items[0].isTask && items[0].checked);
        CHECK(items[1].isTask && !items[1].checked);
        CHECK(items[2].isTask && items[2].checked);
        CHECK(!items[3].isTask);
        CHECK_EQ(inlinesToPlainText(items[0].children[0].inlines), "done item");
    }
    {
        // Loose list: a blank line between items.
        const MarkdownDocument doc = parse("- a\n\n- b\n");
        CHECK(!doc.blocks[0].tight);
    }
}

RIVET_TEST(parseNestedLists) {
    const std::string src = readFixture("nested-lists.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 2u);
    const Block* cur = &doc.blocks[0];
    int depth = 1;
    while (cur->items[0].children.size() > 1) {
        cur = &cur->items[0].children[1];
        CHECK_EQ(cur->kind, BlockKind::List);
        ++depth;
    }
    CHECK_EQ(depth, 4);
    CHECK_EQ(doc.blocks[0].items.size(), 2u);
    CHECK_EQ(slice(src, doc.blocks[0].items[0].range), "- level 1\n  - level 2\n    - level 3\n      - level 4");
    // Ordered list with a nested bullet list.
    const Block& ord = doc.blocks[1];
    CHECK(ord.ordered);
    CHECK_EQ(ord.items.size(), 2u);
    CHECK_EQ(ord.items[0].children[1].kind, BlockKind::List);
    CHECK_EQ(ord.items[0].children[1].items.size(), 2u);
}

RIVET_TEST(parseBlockquotes) {
    const std::string src = readFixture("blockquote.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 2u);
    const Block& q = doc.blocks[0];
    CHECK_EQ(q.kind, BlockKind::Quote);
    CHECK_EQ(q.children.size(), 2u);
    CHECK_EQ(q.children[0].kind, BlockKind::Paragraph);
    CHECK_EQ(q.children[1].kind, BlockKind::Quote);
    CHECK_EQ(slice(src, q.children[1].range), "> > nested quote");
    CHECK_EQ(doc.blocks[1].kind, BlockKind::Quote);
    CHECK_EQ(doc.blocks[1].children[0].kind, BlockKind::List);
}

RIVET_TEST(parseCodeBlocks) {
    {
        const std::string src = readFixture("code.md");
        const MarkdownDocument doc = parse(src);
        CHECK_EQ(doc.blocks.size(), 3u);
        CHECK_EQ(doc.blocks[1].kind, BlockKind::CodeBlock);
        CHECK(!doc.blocks[1].fenced);
        CHECK_EQ(doc.blocks[1].text, "int main() {\n    return 0;\n}\n");
        CHECK_EQ(slice(src, doc.blocks[1].range), "    int main() {\n        return 0;\n    }");
    }
    {
        const std::string src = readFixture("fenced-code.md");
        const MarkdownDocument doc = parse(src);
        CHECK_EQ(doc.blocks.size(), 3u);
        CHECK(doc.blocks[0].fenced);
        CHECK_EQ(doc.blocks[0].info, "cpp");
        CHECK_EQ(doc.blocks[0].text, "#include <cstdio>\n\nint main() {\n    puts(\"hi\");\n}\n");
        CHECK_EQ(slice(src, doc.blocks[0].range).substr(0, 6), "```cpp");
        CHECK_EQ(slice(src, doc.blocks[0].range).substr(slice(src, doc.blocks[0].range).size() - 3), "```");
        CHECK_EQ(doc.blocks[1].text, "tilde fence\n");
        CHECK_EQ(slice(src, doc.blocks[1].range), "~~~\ntilde fence\n~~~");
        // Unterminated fence runs to the end of the document.
        CHECK_EQ(doc.blocks[2].text, "unterminated fence\n");
    }
}

RIVET_TEST(parseTables) {
    const std::string src = readFixture("tables.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 1u);
    const Block& t = doc.blocks[0];
    CHECK_EQ(t.kind, BlockKind::Table);
    CHECK_EQ(t.alignments.size(), 3u);
    CHECK_EQ(t.alignments[0], ColumnAlign::Left);
    CHECK_EQ(t.alignments[1], ColumnAlign::Center);
    CHECK_EQ(t.alignments[2], ColumnAlign::Right);
    CHECK_EQ(t.header.cells.size(), 3u);
    CHECK_EQ(inlinesToPlainText(t.header.cells[2].inlines), "Price");
    CHECK_EQ(t.body.size(), 3u);
    CHECK_EQ(t.body[1].cells[0].inlines[0].kind, InlineKind::Strong);
    // The short row is padded to the column count.
    CHECK_EQ(t.body[2].cells.size(), 3u);
    CHECK(t.body[2].cells[2].inlines.empty());
    CHECK_EQ(slice(src, t.body[0].cells[0].range), "Apple");
    CHECK_EQ(slice(src, t.header.range), "| Name | Qty | Price |");
    CHECK_EQ(slice(src, t.range).substr(0, 6), "| Name");
}

RIVET_TEST(parseHtmlBlockStaysText) {
    const std::string src = "<div class=\"x\">\n<b>raw</b>\n</div>\n\nafter\n";
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks[0].kind, BlockKind::HtmlBlock);
    CHECK_EQ(doc.blocks[0].text, "<div class=\"x\">\n<b>raw</b>\n</div>\n");
    CHECK_EQ(doc.blocks[1].kind, BlockKind::Paragraph);
    // Inline HTML is kept as literal text.
    const MarkdownDocument d2 = parse("a <span>b</span> c\n");
    CHECK_EQ(inlinesToPlainText(d2.blocks[0].inlines), "a <span>b</span> c");
}

RIVET_TEST(parseHorizontalRule) {
    const std::string src = "above\n\n---\n\nbelow\n\n***\n";
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 4u);
    CHECK_EQ(doc.blocks[1].kind, BlockKind::HorizontalRule);
    CHECK_EQ(slice(src, doc.blocks[1].range), "---");
    CHECK_EQ(slice(src, doc.blocks[3].range), "***");
}

RIVET_TEST(parseCyrillic) {
    const std::string src = readFixture("cyrillic.md");
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.headings.size(), 2u);
    CHECK_EQ(doc.headings[0].slug, "привет-мир");
    CHECK_EQ(doc.headings[1].slug, "заголовок-второго-уровня");
    CHECK(doc.findHeadingBySlug("#привет-мир") != nullptr);
    CHECK_EQ(inlinesToPlainText(doc.blocks[1].inlines), "Это простой абзац на русском языке с жирным текстом.");
    CHECK(doc.diagnostics.empty());
    CHECK_EQ(checkRanges(doc).problems, 0u);
    // Source ranges address bytes (UTF-8), not characters.
    CHECK_EQ(slice(src, doc.blocks[0].range), "# Привет, мир!");
}

RIVET_TEST(parseCrlf) {
    const std::string src = readFixture("crlf.md");
    CHECK(src.find("\r\n") != std::string::npos);
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 5u);
    CHECK_EQ(doc.blocks[0].kind, BlockKind::Heading);
    CHECK_EQ(slice(src, doc.blocks[0].range), "# CRLF");
    CHECK_EQ(slice(src, doc.blocks[1].range), "Line one\r\nLine two");
    CHECK_EQ(doc.blocks[2].kind, BlockKind::List);
    CHECK_EQ(doc.blocks[3].kind, BlockKind::CodeBlock);
    CHECK_EQ(slice(src, doc.blocks[3].range), "```\r\ncode\r\n```");
    CHECK_EQ(doc.blocks[4].kind, BlockKind::Table);
    // No carriage returns leak into the model text.
    const PlainText pt = extractPlainText(doc, false);
    CHECK(pt.text.find('\r') == std::string::npos);
    CHECK_EQ(checkRanges(doc).problems, 0u);
}

RIVET_TEST(parseBomIsStrippedAndOffsetsRelativeToText) {
    const std::string raw = readFixture("bom.md");
    CHECK(raw.size() > 3 && static_cast<unsigned char>(raw[0]) == 0xEF);
    const MarkdownDocument doc = parse(raw);
    CHECK_EQ(doc.sourceOffsetBase, 3u);
    CHECK_EQ(doc.sourceLength, raw.size() - 3);
    CHECK_EQ(doc.blocks[0].kind, BlockKind::Heading);
    CHECK_EQ(doc.headings[0].text, "BOM heading");
    CHECK_EQ(doc.blocks[0].range.start, 0u);
    const std::string_view text = std::string_view(raw).substr(doc.sourceOffsetBase);
    CHECK_EQ(slice(text, doc.blocks[0].range), "# BOM heading");
    CHECK_EQ(doc.blocks[1].range.start, 15u);
    // Without a BOM the base is zero.
    CHECK_EQ(parse("# x\n").sourceOffsetBase, 0u);
}

RIVET_TEST(parseMalformedNeverCrashesAndRangesStayValid) {
    const std::string src = readFixture("malformed-ish.md");
    const MarkdownDocument doc = parse(src);
    CHECK(!doc.blocks.empty());
    const RangeCheck rc = checkRanges(doc);
    if (rc.problems) std::fprintf(stderr, "    %s\n", rc.firstProblem.c_str());
    CHECK_EQ(rc.problems, 0u);
    // Unknown and absurd entities are kept verbatim / replaced, not fatal.
    const PlainText pt = extractPlainText(doc, false);
    CHECK(pt.text.find("&unknown;") != std::string::npos);
}

RIVET_TEST(parseNulAndInvalidUtf8) {
    std::string src = "a";
    src.push_back('\0');
    src += "b \xFF\xFE c\n\n```\nx\xC3\n```\n";
    const MarkdownDocument doc = parse(src);
    CHECK_EQ(doc.blocks.size(), 2u);
    const std::string text = inlinesToPlainText(doc.blocks[0].inlines);
    CHECK(text.find('\0') == std::string::npos);
    CHECK(text.find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(hasDiag(doc, DiagnosticCode::InvalidUtf8));
    // Offsets stay in original-byte space.
    CHECK_EQ(checkRanges(doc).problems, 0u);
    // All emitted text is valid UTF-8.
    const std::string code = doc.blocks[1].text;
    for (std::size_t i = 0; i < code.size();) {
        const auto b = static_cast<unsigned char>(code[i]);
        const std::size_t len = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
        CHECK(i + len <= code.size());
        i += len;
    }
}

RIVET_TEST(parseEmptyAndWhitespaceOnly) {
    CHECK(parse("").blocks.empty());
    CHECK(parse("\n\n   \n").blocks.empty());
    CHECK(parse("\xEF\xBB\xBF").blocks.empty());
    CHECK_EQ(parse("x").blocks.size(), 1u);
}

RIVET_TEST(parseSourceRangesForAllFixtures) {
    const char* names[] = {"basic.md",       "headings.md",   "emphasis.md", "lists.md",     "nested-lists.md",
                           "task-list.md",   "blockquote.md", "code.md",     "fenced-code.md", "tables.md",
                           "links.md",       "local-image.md", "cyrillic.md", "malformed-ish.md", "crlf.md", "bom.md"};
    for (const char* n : names) {
        const std::string raw = readFixture(n);
        CHECK(!raw.empty());
        const MarkdownDocument doc = parse(raw);
        const RangeCheck rc = checkRanges(doc);
        if (rc.problems) std::fprintf(stderr, "    %s: %s\n", n, rc.firstProblem.c_str());
        CHECK_EQ(rc.problems, 0u);
        CHECK_EQ(doc.blockCount, countBlocks(doc.blocks));
    }
}

RIVET_TEST(parseBlockIdsAreUniquePreorder) {
    const MarkdownDocument doc = parse(readFixture("nested-lists.md") + "\n> q\n\n# h\n");
    std::vector<std::uint32_t> ids;
    const auto walk = [&](auto&& self, const std::vector<Block>& v) -> void {
        for (const Block& b : v) {
            ids.push_back(b.id);
            self(self, b.children);
            for (const ListItem& it : b.items) self(self, it.children);
        }
    };
    walk(walk, doc.blocks);
    CHECK_EQ(ids.size(), doc.blockCount);
    for (std::size_t i = 0; i < ids.size(); ++i) CHECK_EQ(ids[i], i);
}

RIVET_TEST(parseLimitNestingDepthFlattens) {
    std::string src;
    for (int i = 0; i < 100; ++i) src += "> ";
    src += "deep text\n";
    ParseLimits lim;
    lim.maxNestingDepth = 8;
    const MarkdownDocument doc = parse(src, lim);
    CHECK(hasDiag(doc, DiagnosticCode::NestingTooDeep));
    int depth = 0;
    const Block* cur = &doc.blocks[0];
    while (cur->kind == BlockKind::Quote && !cur->children.empty()) {
        ++depth;
        cur = &cur->children[0];
    }
    CHECK_EQ(depth, 8);
    // The text survives, flattened into the deepest container.
    CHECK_EQ(extractPlainText(doc, false).text, "deep text");
    CHECK_EQ(checkRanges(doc).problems, 0u);
}

RIVET_TEST(parseLimitDeepNestingDoesNotOverflowTheStack) {
    std::string src;
    for (int i = 0; i < 20000; ++i) src += "> ";
    src += "x\n";
    const MarkdownDocument doc = parse(src);
    CHECK(hasDiag(doc, DiagnosticCode::NestingTooDeep));
    CHECK_EQ(extractPlainText(doc, false).text, "x");
}

RIVET_TEST(parseLimitInlineDepth) {
    // Alternating * and _ emphasis nests: *w _w *w _w ... x ... _ w* ...
    std::string deep;
    std::vector<char> marks;
    for (int i = 0; i < 40; ++i) {
        marks.push_back(i % 2 ? '_' : '*');
        deep += marks.back();
        deep += "w ";
    }
    deep += "x";
    for (int i = 39; i >= 0; --i) {
        deep += marks[static_cast<std::size_t>(i)];
        if (i) deep += " w";
    }
    ParseLimits lim;
    lim.maxInlineDepth = 4;
    const MarkdownDocument doc = parse(deep + "\n", lim);
    CHECK(hasDiag(doc, DiagnosticCode::InlineTooDeep));
    int maxDepth = 0;
    const auto walk = [&](auto&& self, const std::vector<Inline>& v, int d) -> void {
        for (const Inline& i : v) {
            if (i.kind != InlineKind::Text) maxDepth = std::max(maxDepth, d);
            self(self, i.children, d + 1);
        }
    };
    walk(walk, doc.blocks[0].inlines, 1);
    CHECK_LE(maxDepth, 4);
    CHECK(inlinesToPlainText(doc.blocks[0].inlines).find('x') != std::string::npos);
    CHECK_EQ(checkRanges(doc).problems, 0u);
}

RIVET_TEST(parseLimitBlockCountStopsParsing) {
    std::string src;
    for (int i = 0; i < 1000; ++i) src += "para " + std::to_string(i) + "\n\n";
    ParseLimits lim;
    lim.maxBlocks = 50;
    const MarkdownDocument doc = parse(src, lim);
    CHECK(hasDiag(doc, DiagnosticCode::TooManyBlocks));
    CHECK_LE(doc.blocks.size(), 50u);
    CHECK_GE(doc.blocks.size(), 40u);
    CHECK_EQ(checkRanges(doc).problems, 0u);
}

RIVET_TEST(parseLimitTableCellsAndColumns) {
    std::string src = "| a | b | c | d |\n|---|---|---|---|\n";
    for (int i = 0; i < 100; ++i) src += "| 1 | 2 | 3 | 4 |\n";
    ParseLimits lim;
    lim.maxTableCells = 40;
    const MarkdownDocument doc = parse(src, lim);
    CHECK(hasDiag(doc, DiagnosticCode::TableTooLarge));
    const Block& t = doc.blocks[0];
    CHECK_LE((t.body.size() + 1) * 4, 44u);
    for (const TableRow& r : t.body) CHECK_EQ(r.cells.size(), 4u); // no half-built rows
    ParseLimits lim2;
    lim2.maxTableColumns = 2;
    const MarkdownDocument d2 = parse(src, lim2);
    CHECK(hasDiag(d2, DiagnosticCode::TableTooLarge));
    CHECK_EQ(d2.blocks[0].alignments.size(), 2u);
    CHECK_EQ(d2.blocks[0].header.cells.size(), 2u);
    CHECK_EQ(d2.blocks[0].body[5].cells.size(), 2u);
}

RIVET_TEST(parseLimitCodeBlockSize) {
    std::string src = "```\n";
    for (int i = 0; i < 1000; ++i) src += "line of code number " + std::to_string(i) + "\n";
    src += "```\n";
    ParseLimits lim;
    lim.maxCodeBlockBytes = 500;
    const MarkdownDocument doc = parse(src, lim);
    CHECK(hasDiag(doc, DiagnosticCode::CodeBlockTruncated));
    CHECK_LE(doc.blocks[0].text.size(), 500u);
    CHECK_GE(doc.blocks[0].text.size(), 400u);
    // Range still covers the whole block in the source.
    CHECK_EQ(doc.blocks[0].range.end, src.size() - 1 - 0);
}

RIVET_TEST(parseLimitSourceTruncatedAtUtf8Boundary) {
    std::string src = "# Заголовок\n\nТекст абзаца\n";
    ParseLimits lim;
    lim.maxSourceBytes = 5; // lands inside the second Cyrillic letter
    const MarkdownDocument doc = parse(src, lim);
    CHECK(hasDiag(doc, DiagnosticCode::SourceTruncated));
    CHECK_EQ(doc.sourceLength, 4u); // "# " + one 2-byte letter; the next would cross the limit
    CHECK(!doc.blocks.empty());
    CHECK_EQ(doc.headings[0].text, "З");
}

RIVET_TEST(parseIsPureAndThreadSafe) {
    const std::string src = generateMarkdown(60 * 1024) + readFixture("tables.md") + readFixture("cyrillic.md");
    const std::string reference = dump(parse(src));
    CHECK(!reference.empty());
    std::vector<std::string> results(4);
    std::vector<std::thread> threads;
    for (std::size_t i = 0; i < results.size(); ++i) {
        threads.emplace_back([&, i] {
            for (int k = 0; k < 3; ++k) results[i] = dump(parse(src));
        });
    }
    for (auto& t : threads) t.join();
    for (const auto& r : results) CHECK(r == reference);
}

RIVET_TEST(parseGeneratedDocumentRangesAreValid) {
    const std::string src = generateMarkdown(200 * 1024);
    const MarkdownDocument doc = parse(src);
    const RangeCheck rc = checkRanges(doc);
    if (rc.problems) std::fprintf(stderr, "    %s\n", rc.firstProblem.c_str());
    CHECK_EQ(rc.problems, 0u);
    CHECK_GT(doc.headings.size(), 10u);
    // Slugs are unique.
    std::vector<std::string> slugs;
    for (const auto& h : doc.headings) slugs.push_back(h.slug);
    std::sort(slugs.begin(), slugs.end());
    CHECK(std::adjacent_find(slugs.begin(), slugs.end()) == slugs.end());
}

RIVET_TEST(plainTextExtractionMapsToSource) {
    const std::string src = "# Title\n\nHello **bold** world\n\n- one\n- two\n\n| a | b |\n|---|---|\n| 1 | 2 |\n";
    const MarkdownDocument doc = parse(src);
    const PlainText pt = extractPlainText(doc);
    CHECK_EQ(pt.text, "Title\nHello bold world\none\ntwo\na\tb\n1\t2");
    const std::size_t at = pt.text.find("bold");
    const SourceRange r = sourceRangeForText(pt, at, at + 4);
    CHECK_EQ(slice(src, r), "bold"); // text spans exclude delimiters
    const SourceRange r2 = sourceRangeForText(pt, 0, 5);
    CHECK_EQ(slice(src, r2), "Title");
    CHECK(sourceRangeForText(pt, 1000, 1010).empty());
    // Without spans only text is produced.
    CHECK(extractPlainText(doc, false).spans.empty());
}
