// SPDX-License-Identifier: MPL-2.0
// WYSIWYG editing core (ADR-0031): source provenance of laid-out text, the
// caret map (source offset <-> rendered position), and the pure source edits
// (typing with escaping, deleting, Enter, paste, formatting toggles). Every
// edit is checked through a re-parse: what the user sees afterwards.
#include "RivetTest.h"

#include "MarkdownTestKit.hpp"
#include "markdown/Grapheme.hpp"
#include "markdown/MarkdownCaret.hpp"
#include "markdown/MarkdownEditing.hpp"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace rivet::markdown;
using namespace rivet::markdown::testkit;
namespace ed = rivet::markdown::editing;

namespace {

// A parsed + laid out source with its caret map.
struct Rendered {
    std::string src;
    MarkdownDocument doc;
    MarkdownLayout layout;
    CaretMap map;

    explicit Rendered(std::string text, double width = 2000.0) : src(std::move(text)) {
        doc = parse(src);
        layout = layoutMarkdown(doc, width, measurer(), nullptr, plainTypography());
        map = CaretMap(layout);
    }
    Rendered(const Rendered&) = delete;
    Rendered& operator=(const Rendered&) = delete;

    static const FixedMeasurer& measurer() {
        static const FixedMeasurer m;
        return m;
    }
    // Every caret stop in order.
    std::vector<TextPosition> stops() const {
        std::vector<TextPosition> out;
        for (std::optional<TextPosition> p = map.first(); p; p = map.next(*p)) out.push_back(*p);
        return out;
    }
    // What the reader sees: block texts joined by '|'.
    std::string text() const {
        std::string out;
        for (const LayoutBlock& b : layout.blocks) {
            if (!out.empty()) out.push_back('|');
            for (std::uint32_t l = b.lineBegin; l < b.lineEnd; ++l) {
                const LayoutLine& line = layout.lines[l];
                for (std::uint32_t r = line.runBegin; r < line.runEnd; ++r) out += layout.runs[r].text;
                if (l + 1 < b.lineEnd) out.push_back(line.end == LineEnd::Wrap ? ' ' : '/');
            }
        }
        return out;
    }
};

bool same(const std::string& actual, const std::string& expected) {
    if (actual == expected) return true;
    std::fprintf(stderr, "    expected: [%s]\n    actual:   [%s]\n", expected.c_str(), actual.c_str());
    return false;
}

std::string applyEdit(const std::string& src, const ed::Edit& e) { return ed::applied(src, e); }

// Offset of the n-th occurrence of `needle`.
std::size_t at(const std::string& src, std::string_view needle, std::size_t n = 0) {
    std::size_t pos = src.find(needle);
    while (n-- > 0 && pos != std::string::npos) pos = src.find(needle, pos + 1);
    return pos;
}

std::string backspace(const std::string& src, std::size_t caret, std::size_t* newCaret = nullptr) {
    Rendered r(src);
    const ed::DeleteResult res = ed::deleteCharacter(r.src, &r.doc, r.map, caret, false);
    if (newCaret != nullptr) *newCaret = res.edit.caret;
    return applyEdit(src, res.edit);
}

std::string forwardDelete(const std::string& src, std::size_t caret) {
    Rendered r(src);
    return applyEdit(src, ed::deleteCharacter(r.src, &r.doc, r.map, caret, true).edit);
}

// Types `typed` one code point at a time, re-parsing between keystrokes like
// the app does.
std::string typeEach(std::string src, std::size_t caret, std::string_view typed) {
    for (std::size_t i = 0; i < typed.size();) {
        const std::size_t len = utf8::decode(typed, i).length;
        const MarkdownDocument doc = parse(src);
        const ed::Edit e = ed::insertText(src, &doc, caret, caret, typed.substr(i, len));
        src = applyEdit(src, e);
        caret = e.caret;
        i += len;
    }
    return src;
}

} // namespace

// ================================================================ provenance

RIVET_TEST(runSpansMapExactTextToTheSameSourceBytes) {
    std::vector<std::string> sources = {generateMarkdown(30000, 7), generateMarkdown(20000, 99)};
    for (const auto& entry : std::filesystem::directory_iterator(RIVET_MD_FIXTURE_DIR)) {
        if (entry.path().extension() == ".md") sources.push_back(readFixture(entry.path().filename().string()));
    }
    CHECK(sources.size() > 3);
    for (const std::string& src : sources) {
        Rendered r(src, 500.0);
        const std::string_view text = std::string_view(src).substr(r.doc.sourceOffsetBase); // BOM stripped
        for (const LayoutRun& run : r.layout.runs) {
            CHECK(run.spanBegin <= run.spanEnd);
            std::uint32_t lastText = 0;
            std::size_t lastSource = 0;
            for (std::uint32_t i = run.spanBegin; i < run.spanEnd; ++i) {
                const TextSpan& sp = r.layout.runSpans[i];
                CHECK(sp.textEnd() <= run.text.size());
                CHECK(sp.text >= lastText);
                CHECK(sp.source >= lastSource);
                CHECK(sp.sourceEnd() <= text.size());
                lastText = sp.textEnd();
                lastSource = sp.source;
                if (sp.exact() && text.substr(sp.source, sp.sourceLength) !=
                                      std::string_view(run.text).substr(sp.text, sp.textLength)) {
                    std::fprintf(stderr, "    span text [%s] source [%s]\n",
                                 std::string(run.text.substr(sp.text, sp.textLength)).c_str(),
                                 std::string(text.substr(sp.source, sp.sourceLength)).c_str());
                    CHECK(false);
                }
            }
        }
    }
}

RIVET_TEST(caretStopsRoundTripThroughSourceOffsets) {
    const std::string src = "# Title *x*\n\nA **bold** and *em* and `code` and [link](http://e.com \"t\") "
                            "\\*not\\* &amp; &copy; ~~gone~~ end.\n\n- item **one**\n- item two\n\n"
                            "> quoted *text*\n> more\n\n```\nfn()\n\n\tx\n```\n\nПривет мир 👍🏽 é\n";
    Rendered r(src, 260.0); // narrow: several soft wraps
    const std::vector<TextPosition> stops = r.stops();
    CHECK(stops.size() > 60);
    std::size_t previous = 0;
    for (const TextPosition& p : stops) {
        CHECK(r.map.isStop(p));
        const std::size_t s = r.map.sourceOf(p);
        CHECK(s >= previous); // source order follows the reading order
        previous = s;
        const std::optional<TextPosition> back = r.map.positionOf(s);
        CHECK(back.has_value());
        if (r.map.sourceOf(*back) != s || !(*back == p)) {
            std::fprintf(stderr, "    stop run %u off %u src %zu -> run %u off %u src %zu [%s]\n", p.run, p.offset, s,
                         back->run, back->offset, r.map.sourceOf(*back), r.layout.runs[p.run].text.c_str());
        }
        CHECK(r.map.sourceOf(*back) == s);
        CHECK(*back == p);
        // Never inside a UTF-8 sequence.
        CHECK(s == src.size() || (static_cast<unsigned char>(src[s]) & 0xC0) != 0x80);
    }
    // previous() walks the same stops backwards.
    std::vector<TextPosition> reverse;
    for (std::optional<TextPosition> p = r.map.last(); p; p = r.map.previous(*p)) reverse.push_back(*p);
    CHECK_EQ(reverse.size(), stops.size());
    for (std::size_t i = 0; i < stops.size(); ++i) CHECK(reverse[stops.size() - 1 - i] == stops[i]);
}

RIVET_TEST(caretMapsAcrossInlineMarkup) {
    const std::string src = "a **bold** `co` [ln](http://x.y) \\* &amp; z";
    Rendered r(src);
    // Hidden markup snaps to the nearest rendered position.
    const auto at4 = r.map.positionOf(at(src, "bold"));
    const auto at3 = r.map.positionOf(at(src, "**") + 1);
    CHECK(at4 && at3);
    CHECK(*at4 == *at3);
    CHECK_EQ(r.map.sourceOf(*at4), at(src, "**")); // the end of "a " (style of the text before)
    // Inside the link target: lands on a rendered position outside it.
    const std::size_t url = at(src, "http");
    const std::size_t urlEnd = at(src, ")");
    const auto inUrl = r.map.positionOf(url + 3);
    CHECK(inUrl.has_value());
    const std::size_t snapped = r.map.sourceOf(*inUrl);
    CHECK(snapped <= url - 2 || snapped >= urlEnd);
    // An escape and an entity are one character each, with their whole source.
    const auto afterStar = r.map.positionOf(at(src, "\\*") + 2);
    CHECK(afterStar.has_value());
    const auto star = r.map.charBefore(*afterStar);
    CHECK(star && star->start == at(src, "\\*") && star->end == at(src, "\\*") + 2);
    const auto afterAmp = r.map.positionOf(at(src, "&amp;") + 5);
    const auto amp = r.map.charBefore(*afterAmp);
    CHECK(amp && amp->start == at(src, "&amp;") && amp->size() == 5);
    CHECK(r.map.charTextBefore(*afterAmp) == "&");
    // Inside the code span: 1:1.
    const auto inCode = r.map.positionOf(at(src, "co") + 1);
    CHECK_EQ(r.map.sourceOf(*inCode), at(src, "co") + 1);
}

RIVET_TEST(caretWalksGraphemesNotCodePoints) {
    const std::string src = "a👍🏽b👨‍👩‍👧c🇫🇷🇩🇪dе́f";
    Rendered r(src);
    std::vector<std::string> chars;
    for (std::optional<TextPosition> p = r.map.first(); p;) {
        const std::string_view c = r.map.charTextAfter(*p);
        if (c.empty()) break;
        chars.emplace_back(c);
        p = r.map.next(*p);
    }
    const std::vector<std::string> expected = {"a", "👍🏽", "b", "👨‍👩‍👧", "c", "🇫🇷", "🇩🇪", "d", "е́", "f"};
    CHECK(chars == expected);
    CHECK(grapheme::isBoundary("е́", 0));
    CHECK(!grapheme::isBoundary("е́", 2));
}

RIVET_TEST(caretSkipsReadOnlyIslands) {
    const std::string src = "before\n\n| a | b |\n|---|---|\n| c | d |\n\n<div>\nhtml\n</div>\n\n"
                            "![alt](pic.png)\n\n```mermaid\ngraph TD\n```\n\n$$\nx_1 * y_2\n$$\n\n```js\nlet x;\n```\n\nafter\n";
    Rendered r(src);
    std::size_t codeStops = 0;
    for (const TextPosition& p : r.stops()) {
        const LayoutBlock& b = r.layout.blocks[r.map.blockOf(p)];
        CHECK(!b.readOnlyIsland());
        if (b.kind == LayoutBlockKind::Code) ++codeStops;
    }
    CHECK(codeStops > 0); // ordinary code content stays editable
    // Down from "before" lands in the code block, skipping every island line.
    const TextPosition start = *r.map.first();
    const TextPosition down = r.map.vertical(start, 0.0, 1, Rendered::measurer());
    CHECK(r.layout.blocks[r.map.blockOf(down)].kind == LayoutBlockKind::Code);
    // The special blocks are marked for their future renderers.
    std::size_t mermaid = 0;
    std::size_t math = 0;
    for (const LayoutBlock& b : r.layout.blocks) {
        if (b.special == SpecialBlock::Mermaid) ++mermaid;
        if (b.special == SpecialBlock::DisplayMath) ++math;
    }
    CHECK_EQ(mermaid, 1u);
    CHECK_EQ(math, 1u);
}

RIVET_TEST(parserMarksMermaidAndDisplayMathBlocks) {
    const MarkdownDocument doc = parse("```Mermaid theme\ngraph\n```\n\n> $$\n> a_1 *b*\n> $$\n\n$$ e^x $$\n\n```math\ny\n```\n");
    CHECK_EQ(doc.blocks.size(), 4u);
    CHECK(doc.blocks[0].special == SpecialBlock::Mermaid);
    CHECK(doc.blocks[0].text == "graph\n");
    const Block& quoted = doc.blocks[1].children.at(0);
    CHECK(quoted.special == SpecialBlock::DisplayMath);
    CHECK(quoted.kind == BlockKind::CodeBlock);
    CHECK(same(quoted.text, "a_1 *b*\n"));
    CHECK(doc.blocks[2].special == SpecialBlock::DisplayMath);
    CHECK(same(doc.blocks[2].text, " e^x \n"));
    CHECK(doc.blocks[3].special == SpecialBlock::DisplayMath);
    CHECK(isReadOnlyIsland(doc.blocks[3]));
    CHECK(!isReadOnlyIsland(parse("```\nx\n```\n").blocks[0]));
}

RIVET_TEST(caretVerticalAndLineMovement) {
    const std::string src = "first line\n\nsecond paragraph here\n\nthird";
    Rendered r(src);
    const TextPosition a = *r.map.positionOf(at(src, "line"));
    const double x = r.layout.xForPosition(a, Rendered::measurer());
    const TextPosition down = r.map.vertical(a, x, 1, Rendered::measurer());
    CHECK_EQ(r.map.sourceOf(down), at(src, "second") + 6);
    const TextPosition down2 = r.map.vertical(down, x, 1, Rendered::measurer());
    CHECK_EQ(r.map.sourceOf(down2), src.size()); // "third" is shorter: its end
    CHECK_EQ(r.map.sourceOf(r.map.vertical(down2, x, 1, Rendered::measurer())), src.size());
    CHECK_EQ(r.map.sourceOf(r.map.lineStart(down)), at(src, "second"));
    CHECK_EQ(r.map.sourceOf(r.map.lineEnd(down)), at(src, "here") + 4);
    const auto [w0, w1] = r.map.wordAt(*r.map.positionOf(at(src, "paragraph") + 3));
    CHECK_EQ(r.map.sourceOf(w0), at(src, "paragraph"));
    CHECK_EQ(r.map.sourceOf(w1), at(src, "paragraph") + 9);
    CHECK_EQ(r.map.sourceOf(*r.map.wordRight(*r.map.positionOf(at(src, "second")))), at(src, "second") + 6);
    CHECK_EQ(r.map.sourceOf(*r.map.wordLeft(*r.map.positionOf(at(src, "here")))), at(src, "paragraph"));
}

// ==================================================================== typing

RIVET_TEST(typedSpecialCharactersStayLiteralAtLineStartAndMidLine) {
    const std::string specials = "\\`*_[]<>#-+=&;.)~!|{}()\"'$%^:/?,@0123456789 ";
    for (const char c : specials) {
        const std::string typed(1, c);
        for (const std::size_t offset : {std::size_t{0}, std::size_t{3}, std::size_t{7}}) {
            const std::string src = "abc def\n";
            const MarkdownDocument doc = parse(src);
            const std::string out = applyEdit(src, ed::insertText(src, &doc, offset, offset, typed));
            std::string expected = "abc def";
            expected.insert(offset, typed);
            if (c == ' ') expected = "abc def"; // spaces at the edges or next to a space do not render
            Rendered r(out);
            CHECK(r.doc.blocks.size() == 1 && r.doc.blocks[0].kind == BlockKind::Paragraph);
            CHECK(same(r.text(), expected));
        }
    }
}

RIVET_TEST(typingMarkupSequencesKeepsThemLiteral) {
    const struct {
        const char* typed;
        const char* shown;
    } cases[] = {
        {"# not a heading", "# not a heading"},
        {"- not a list", "- not a list"},
        {"1. not a list", "1. not a list"},
        {"12) nor this", "12) nor this"},
        {"> no quote", "> no quote"},
        {"*not em* and **not strong**", "*not em* and **not strong**"},
        {"[no](link) ![no](img) <b>tag</b>", "[no](link) ![no](img) <b>tag</b>"},
        {"`no code` ~~no strike~~", "`no code` ~~no strike~~"},
        {"&amp; &copy; &#65; \\*", "&amp; &copy; &#65; \\*"},
        {"===", "==="},
        {"---", "---"},
        {"Привет, мир 👍🏽", "Привет, мир 👍🏽"},
    };
    for (const auto& c : cases) {
        const std::string out = typeEach("x\n\n", 3, c.typed);
        Rendered r(out);
        CHECK_EQ(r.doc.blocks.size(), 2u);
        CHECK(r.doc.blocks.back().kind == BlockKind::Paragraph);
        CHECK(same(r.text(), std::string("x|") + c.shown));
    }
    // Inside a heading, a typed '#' is text too (a closing sequence would vanish).
    const std::string h = typeEach("# Title\n", 7, " #");
    CHECK(same(Rendered(h).text(), "Title #"));
}

RIVET_TEST(typingInCodeIsVerbatimAndUsesEmphasisEdgesCorrectly) {
    const std::string code = "```\nab\n```\n";
    CHECK(same(typeEach(code, 5, "*_`<"), "```\na*_`<b\n```\n"));
    const std::string span = "x `ab` y";
    CHECK(same(typeEach(span, 4, "*"), "x `a*b` y"));
    // Typing at the end of a bold word continues the bold...
    const std::string bold = "**bold** x";
    const std::size_t end = 6;
    CHECK(same(typeEach(bold, end, "er"), "**bolder** x"));
    // ...but a space goes outside the delimiters, so the emphasis survives.
    const std::string spaced = typeEach(bold, end, " more");
    CHECK(same(spaced, "**bold** more x"));
    CHECK(same(Rendered(spaced).text(), "bold more x"));
}

RIVET_TEST(typingReplacesTheSelection) {
    const std::string src = "one **two** three";
    const MarkdownDocument doc = parse(src);
    const ed::Edit e = ed::insertText(src, &doc, at(src, "two"), at(src, "two") + 3, "2");
    CHECK(same(applyEdit(src, e), "one **2** three"));
    CHECK_EQ(e.caret, at(src, "two") + 1);
    // Across the end of the bold: its closing delimiter stays.
    const ed::Edit e2 = ed::insertText(src, &doc, at(src, "wo"), at(src, "hree"), "X");
    CHECK(same(Rendered(applyEdit(src, e2)).text(), "one tXhree"));
}

// ================================================================== deleting

RIVET_TEST(backspaceAndDeleteInsideAndAcrossInlineMarkup) {
    const std::string src = "a **b** c";
    // Deleting the only character of a span removes the span.
    CHECK(same(backspace(src, at(src, "b") + 1), "a  c"));
    CHECK(same(forwardDelete(src, at(src, "**")), "a  c"));
    // Whitespace never stays just inside a delimiter.
    const std::string two = "**a b** c";
    const std::string out = backspace(two, at(two, "b") + 1);
    CHECK(same(out, "**a**  c"));
    CHECK(same(Rendered(out).text(), "a c"));
    // Escapes, entities and images go as one character.
    const std::string esc = "x\\*y &amp; ![i](p.png)z";
    CHECK(same(backspace(esc, at(esc, "y")), "xy &amp; ![i](p.png)z"));
    CHECK(same(backspace(esc, at(esc, "&amp;") + 5), "x\\*y  ![i](p.png)z"));
    CHECK(same(backspace(esc, at(esc, "z")), "x\\*y &amp; z"));
    // Inside a link's text.
    const std::string link = "[ab](u) c";
    CHECK(same(backspace(link, 2), "[b](u) c"));
    CHECK(same(backspace(link, 3), "[a](u) c"));
    CHECK(same(backspace("[a](u) c", 2), " c")); // the last character takes the link along
    // A deletion at line start never turns the rest into markup.
    const std::string ls = "x- y\n";
    CHECK(same(backspace(ls, 1), "\\- y\n"));
    CHECK(same(Rendered(backspace(ls, 1)).text(), "- y"));
}

RIVET_TEST(inlineMathIsOneCaretUnit) {
    const std::string src = "a $x^2 + y$ b **$z$**";
    Rendered r(src);
    const std::size_t open = at(src, "$x");
    const std::size_t close = at(src, "$ b") + 1;
    // No stop inside the formula's source; its edges are stops.
    bool before = false;
    bool after = false;
    for (const TextPosition& p : r.stops()) {
        const std::size_t s = r.map.sourceOf(p);
        CHECK(!(s > open && s < close));
        before = before || s == open;
        after = after || s == close;
    }
    CHECK(before && after);
    // One step right skips the whole formula.
    const std::optional<TextPosition> start = r.map.positionOf(open);
    CHECK(start.has_value());
    if (!start) return;
    const std::optional<TextPosition> next = r.map.next(*start);
    CHECK(next.has_value());
    if (!next) return;
    CHECK_EQ(r.map.sourceOf(*next), close);
    CHECK(r.layout.runs[start->run].graphic != 0 || r.layout.runs[next->run].graphic != 0);
    // Backspace after it / delete before it removes it as a whole.
    CHECK(same(backspace(src, close), "a  b **$z$**"));
    CHECK(same(forwardDelete(src, open), "a  b **$z$**"));
    // Inside emphasis too; the emphasis goes with its only content.
    CHECK(same(backspace(src, at(src, "$z$") + 3), "a $x^2 + y$ b "));
    // A word selection never cuts into it.
    const auto [w0, w1] = r.map.wordAt(*start);
    CHECK(r.map.sourceOf(w0) <= open);
    CHECK(r.map.sourceOf(w1) == open || r.map.sourceOf(w1) >= close);
}

RIVET_TEST(backspaceJoinsBlocksAndRespectsIslands) {
    CHECK(same(backspace("para\n\n# Head\n", 8), "paraHead\n"));
    CHECK(same(backspace("- a\n- b\n", 6), "- ab\n"));
    CHECK(same(forwardDelete("- a\n- b\n", 3), "- ab\n"));
    CHECK(same(backspace("> a\n>\n> b\n", 8), "> ab\n"));
    CHECK(same(backspace("one\ntwo\n", 4), "onetwo\n")); // a soft break is one character
    CHECK(same(backspace("> one\n> two\n", 8), "> onetwo\n"));
    CHECK(same(backspace("a\\\nb\n", 3), "ab\n"));        // a hard break too
    // Code blocks never merge with their neighbours.
    const std::string code = "para\n\n```\ncode\n```\n";
    CHECK(same(backspace(code, at(code, "code")), code));
    // Inside code, Backspace at a line start joins the lines.
    const std::string lines = "```\nab\ncd\n```\n";
    CHECK(same(backspace(lines, at(lines, "cd")), "```\nabcd\n```\n"));
    // A table in the way blocks; an image block is offered for selection.
    const std::string table = "a\n\n| x |\n|---|\n| y |\n\nb\n";
    {
        Rendered r(table);
        const ed::DeleteResult res = ed::deleteCharacter(r.src, &r.doc, r.map, at(table, "b\n"), false);
        CHECK(res.blocked);
        CHECK(!res.edit.changesText());
    }
    const std::string image = "a\n\n![alt](p.png)\n\nb\n";
    {
        Rendered r(image);
        const ed::DeleteResult res = ed::deleteCharacter(r.src, &r.doc, r.map, at(image, "b\n"), false);
        CHECK(res.selectIsland.has_value());
        CHECK(same(image.substr(res.selectIsland->start, res.selectIsland->size()), "![alt](p.png)"));
        const std::string removed = applyEdit(image, ed::deleteRange(image, &r.doc, res.selectIsland->start,
                                                                 res.selectIsland->end));
        CHECK(same(Rendered(removed).text(), "a|b"));
    }
}

RIVET_TEST(backspaceRemovesWholeGraphemes) {
    const std::string src = "ab👍🏽c е́ я 👨‍👩‍👧 🇫🇷";
    CHECK(same(backspace(src, at(src, "c")), "abc е́ я 👨‍👩‍👧 🇫🇷"));
    CHECK(same(backspace(src, at(src, " я")), "ab👍🏽c  я 👨‍👩‍👧 🇫🇷"));
    CHECK(same(backspace(src, at(src, " 👨") ), "ab👍🏽c е́  👨‍👩‍👧 🇫🇷"));
    CHECK(same(backspace(src, at(src, " 🇫")), "ab👍🏽c е́ я  🇫🇷"));
    CHECK(same(backspace(src, src.size()), "ab👍🏽c е́ я 👨‍👩‍👧 "));
}

// ===================================================================== Enter

RIVET_TEST(enterSplitsParagraphsContinuesListsAndQuotes) {
    const auto enter = [](const std::string& src, std::size_t caret, std::size_t* newCaret = nullptr) {
        const MarkdownDocument doc = parse(src);
        const ed::Edit e = ed::insertParagraphBreak(src, &doc, caret, caret);
        if (newCaret != nullptr) *newCaret = e.caret;
        return applyEdit(src, e);
    };
    std::size_t c = 0;
    CHECK(same(enter("abcd\n", 2, &c), "ab\n\ncd\n"));
    CHECK_EQ(c, 4u);
    CHECK(same(Rendered(enter("ab - cd\n", 3)).text(), "ab|- cd"));
    // At the end, the caret waits on a blank line; typing makes a paragraph.
    const std::string end = enter("abcd\n\nnext\n", 4, &c);
    CHECK(same(end, "abcd\n\n\n\nnext\n"));
    CHECK(same(Rendered(typeEach(end, c, "x")).text(), "abcd|x|next"));
    // Lists continue with the same marker, numbers count up, tasks restart unchecked.
    CHECK(same(enter("- a\n", 3, &c), "- a\n- \n"));
    CHECK_EQ(c, 6u);
    CHECK(same(enter("* a\n", 3), "* a\n* \n"));
    CHECK(same(enter("1. a\n", 4), "1. a\n2. \n"));
    CHECK(same(enter("3) a\n", 4), "3) a\n4) \n"));
    CHECK(same(enter("- [x] a\n", 7), "- [x] a\n- [ ] \n"));
    CHECK(same(enter("- ab\n", 3), "- a\n- b\n"));
    CHECK(same(enter("  - a\n", 5), "  - a\n  - \n"));
    CHECK(same(enter("> - a\n", 5), "> - a\n> - \n"));
    // An empty item leaves the list; typing then starts a paragraph after it.
    const std::string exit = enter("- a\n- \n", 6, &c);
    CHECK(same(exit, "- a\n\n\n"));
    CHECK(same(Rendered(typeEach(exit, c, "x")).text(), "a|x"));
    // Quotes continue with a blank quote line; an empty quote line leaves.
    const std::string q = enter("> a\n", 3, &c);
    CHECK(same(q, "> a\n>\n> \n"));
    const std::string q2 = enter(q, c, &c);
    CHECK(same(q2, "> a\n>\n\n"));
    CHECK(same(Rendered(typeEach(q2, c, "x")).text(), "a|x"));
    // Headings split into heading + paragraph.
    CHECK(same(enter("# Title\n", 4), "# Ti\n\ntle\n"));
    CHECK(same(Rendered(enter("# Title\n", 4)).text(), "Ti|tle"));
    // At the start of a block a blank line opens above it.
    const std::string above = enter("# Title\n", 2, &c);
    CHECK(same(above, "\n\n# Title\n"));
    CHECK(same(Rendered(typeEach(above, c, "x")).text(), "x|Title"));
    const std::string pAbove = enter("> para\n", 2, &c);
    CHECK(same(Rendered(typeEach(pAbove, c, "x")).text(), "x|para"));
    // Code keeps the line's indentation.
    CHECK(same(enter("```\n  ab\n```\n", 7), "```\n  a\n  b\n```\n"));
    // Shift+Enter: hard break continuing the container.
    const MarkdownDocument doc = parse("- ab\n");
    CHECK(same(applyEdit("- ab\n", ed::insertLineBreak("- ab\n", &doc, 3, 3)), "- a\\\n  b\n"));
    CHECK(same(Rendered(applyEdit("- ab\n", ed::insertLineBreak("- ab\n", &doc, 3, 3))).text(), "a/b"));
}

RIVET_TEST(pastePlainTextEscapesAndBreaksLikeEnter) {
    const std::string src = "- one\n";
    const MarkdownDocument doc = parse(src);
    const std::string out = applyEdit(src, ed::pastePlainText(src, &doc, 5, 5, " *x*\r\n# two\n\n3. three"));
    CHECK(same(out, "- one \\*x\\*\n- \\# two\n- 3\\. three\n"));
    CHECK(same(Rendered(out).text(), "one *x*|# two|3. three"));
    const std::string code = "```\nab\n```\n";
    const MarkdownDocument cdoc = parse(code);
    CHECK(same(applyEdit(code, ed::pastePlainText(code, &cdoc, 5, 5, "*1\n*2")), "```\na*1\n*2b\n```\n"));
}

// ================================================================ formatting

RIVET_TEST(inlineFormattingTogglesAreIdempotent) {
    const ed::InlineFormat formats[] = {ed::InlineFormat::Bold, ed::InlineFormat::Italic,
                                        ed::InlineFormat::Strikethrough, ed::InlineFormat::Code,
                                        ed::InlineFormat::Link};
    const std::string sources[] = {"hello world here", "**hello** world", "*a* and ***b*** `c`", "x\n> two lines\n"};
    for (const std::string& src : sources) {
        for (const ed::InlineFormat f : formats) {
            for (const auto& [b, e] : {std::pair<std::size_t, std::size_t>{0, src.size()}, {2, 2}, {1, src.size() - 2}}) {
                const ed::Edit once = ed::toggleInline(src, b, e, f, "u");
                if (b == e && !once.changesText()) continue; // stepped past a closing delimiter
                const std::string a1 = applyEdit(src, once);
                const std::string a2 = applyEdit(a1, ed::toggleInline(a1, once.anchor, once.caret, f, "u"));
                CHECK(same(a2, src));
            }
        }
    }
    // Wrapping produces the expected structure.
    const std::string w = applyEdit("hello world", ed::toggleInline("hello world", 6, 11, ed::InlineFormat::Bold));
    CHECK(same(w, "hello **world**"));
    CHECK(same(applyEdit(w, ed::toggleInline(w, 8, 13, ed::InlineFormat::Italic)), "hello ***world***"));
    CHECK(same(applyEdit("*it*", ed::toggleInline("*it*", 1, 3, ed::InlineFormat::Italic)), "it"));
    CHECK(same(applyEdit("a`b", ed::toggleInline("a`b", 0, 3, ed::InlineFormat::Code)), "``a`b``"));
    const std::string l = applyEdit("see docs", ed::toggleInline("see docs", 4, 8, ed::InlineFormat::Link, "https://d.e"));
    CHECK(same(l, "see [docs](https://d.e)"));
    CHECK(same(Rendered(l).text(), "see docs"));
    // Multi-line selections wrap each line's content, not its prefix.
    const std::string ml = "> one\n> two\n";
    CHECK(same(applyEdit(ml, ed::toggleInline(ml, 2, 11, ed::InlineFormat::Bold)), "> **one**\n> **two**\n"));
}

RIVET_TEST(inlineFormattingWithCollapsedCaretNeverSelects) {
    // QA bug 5: Cmd+B with no selection inserts an empty pair, typing goes
    // inside it, a second Cmd+B steps out; nothing is ever selected.
    std::string src = "more words";
    std::size_t caret = src.size();
    const auto toggle = [&](ed::InlineFormat f) {
        const ed::Edit e = ed::toggleInline(src, caret, caret, f);
        CHECK(e.anchor == e.caret);
        src = applyEdit(src, e);
        caret = e.caret;
    };
    const auto type = [&](std::string_view t) {
        src.insert(caret, t);
        caret += t.size();
    };
    toggle(ed::InlineFormat::Bold);
    CHECK(same(src, "more words****"));
    CHECK(caret == 12);
    type("bold");
    toggle(ed::InlineFormat::Bold);
    CHECK(same(src, "more words**bold**"));
    CHECK(caret == src.size());
    type(" plain");
    CHECK(same(src, "more words**bold** plain"));
    // Toggling twice before typing restores the text.
    toggle(ed::InlineFormat::Italic);
    toggle(ed::InlineFormat::Italic);
    CHECK(same(src, "more words**bold** plain"));
    for (const ed::InlineFormat f : {ed::InlineFormat::Italic, ed::InlineFormat::Strikethrough, ed::InlineFormat::Code}) {
        src = "x ";
        caret = 2;
        toggle(f);
        type("y");
        toggle(f);
        type(" z");
        CHECK(same(Rendered(src).text(), "x y z"));
    }
    src = "see ";
    caret = 4;
    const ed::Edit link = ed::toggleInline(src, caret, caret, ed::InlineFormat::Link, "https://d.e");
    CHECK(link.anchor == link.caret);
    src = applyEdit(src, link);
    caret = link.caret;
    CHECK(same(src, "see [](https://d.e)"));
    type("docs");
    toggle(ed::InlineFormat::Link);
    CHECK(caret == src.size());
    CHECK(same(Rendered(src).text(), "see docs"));
}

RIVET_TEST(blockFormattingAppliesAndTogglesOff) {
    const auto fmt = [](const std::string& src, std::size_t b, std::size_t e, ed::BlockFormat f) {
        return applyEdit(src, ed::applyBlockFormat(src, b, e, f));
    };
    CHECK(same(fmt("title\n", 2, 2, ed::BlockFormat::Heading2), "## title\n"));
    CHECK(same(fmt("## title\n", 4, 4, ed::BlockFormat::Heading2), "title\n"));
    CHECK(same(fmt("## title\n", 4, 4, ed::BlockFormat::Heading1), "# title\n"));
    CHECK(same(fmt("## title\n", 4, 4, ed::BlockFormat::Paragraph), "title\n"));
    CHECK(same(fmt("Title\n=====\n", 1, 1, ed::BlockFormat::Heading3), "### Title\n"));
    CHECK(same(fmt("a\nb\n", 0, 3, ed::BlockFormat::BulletList), "- a\n- b\n"));
    CHECK(same(fmt("- a\n- b\n", 2, 7, ed::BlockFormat::BulletList), "a\nb\n"));
    CHECK(same(fmt("- a\n- b\n", 2, 7, ed::BlockFormat::NumberedList), "1. a\n2. b\n"));
    CHECK(same(fmt("a\n", 0, 0, ed::BlockFormat::TaskList), "- [ ] a\n"));
    CHECK(same(fmt("- [ ] a\n", 6, 6, ed::BlockFormat::TaskList), "a\n"));
    CHECK(same(fmt("a\n\nb\n", 0, 4, ed::BlockFormat::Quote), "> a\n>\n> b\n"));
    CHECK(same(fmt("> a\n>\n> b\n", 2, 9, ed::BlockFormat::Quote), "a\n\nb\n"));
    CHECK(same(fmt("> a\n", 2, 2, ed::BlockFormat::BulletList), "> - a\n"));
    // Idempotence on single lines, and the caret stays on the same text.
    for (const ed::BlockFormat f : {ed::BlockFormat::Heading1, ed::BlockFormat::Heading6, ed::BlockFormat::BulletList,
                                    ed::BlockFormat::NumberedList, ed::BlockFormat::TaskList, ed::BlockFormat::Quote}) {
        const std::string src = "some text\n";
        const ed::Edit once = ed::applyBlockFormat(src, 5, 5, f);
        const std::string a1 = applyEdit(src, once);
        CHECK(a1.substr(once.caret, 4) == "text");
        const std::string a2 = applyEdit(a1, ed::applyBlockFormat(a1, once.caret, once.caret, f));
        CHECK(same(a2, src));
    }
}
