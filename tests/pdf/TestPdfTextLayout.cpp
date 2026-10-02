// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "pdf/PdfTextLayout.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// PDFium-free tests of the UTF-8 helpers and the deterministic text layout.
// Both build modes.

namespace {

using namespace rivet::pdf;
using rivet::test::utf32;

// Every code point is 10 points wide (5 for combining marks, 0 for none).
double fixedAdvance(char32_t c) {
    if (c >= 0x0300 && c <= 0x036F) return 0.0;
    return 10.0;
}

std::vector<std::string> linesOf(const PdfTextLayoutResult& result) {
    std::vector<std::string> out;
    for (const PdfTextLine& line : result.lines) out.push_back(encodeUtf8(line.text));
    return out;
}

std::vector<std::string> layout(std::string_view text, double wrap) {
    return linesOf(layoutTextBlock(text, wrap, fixedAdvance));
}

using Lines = std::vector<std::string>;

} // namespace

// --- UTF-8 -------------------------------------------------------------------

RIVET_TEST(utf8DecodeValid) {
    bool invalid = true;
    CHECK(decodeUtf8("", &invalid).empty());
    CHECK(!invalid);
    CHECK(decodeUtf8("abc", &invalid) == utf32("abc"));
    CHECK(!invalid);
    // 2-, 3- and 4-byte sequences.
    const std::string mixed = "a\xC3\xA9\xD0\x9F\xE2\x82\xAC\xF0\x9F\x98\x80";
    const std::u32string decoded = decodeUtf8(mixed, &invalid);
    CHECK(!invalid);
    CHECK_EQ(decoded.size(), std::size_t{5});
    CHECK(decoded[1] == 0x00E9);
    CHECK(decoded[2] == 0x041F);
    CHECK(decoded[3] == 0x20AC);
    CHECK(decoded[4] == 0x1F600);
    // Boundary values.
    CHECK(decodeUtf8("\xC2\x80", &invalid)[0] == 0x80);
    CHECK(decodeUtf8("\xDF\xBF", &invalid)[0] == 0x7FF);
    CHECK(decodeUtf8("\xE0\xA0\x80", &invalid)[0] == 0x800);
    CHECK(decodeUtf8("\xED\x9F\xBF", &invalid)[0] == 0xD7FF);
    CHECK(decodeUtf8("\xEE\x80\x80", &invalid)[0] == 0xE000);
    CHECK(decodeUtf8("\xEF\xBF\xBF", &invalid)[0] == 0xFFFF);
    CHECK(decodeUtf8("\xF0\x90\x80\x80", &invalid)[0] == 0x10000);
    CHECK(decodeUtf8("\xF4\x8F\xBF\xBF", &invalid)[0] == 0x10FFFF);
    CHECK(!invalid);
}

RIVET_TEST(utf8DecodeRejectsIllFormed) {
    auto bad = [](std::string_view bytes, std::size_t expectedReplacements) {
        bool invalid = false;
        const std::u32string out = decodeUtf8(bytes, &invalid);
        CHECK(invalid);
        std::size_t replacements = 0;
        for (char32_t c : out) replacements += (c == 0xFFFD) ? 1 : 0;
        CHECK_EQ(replacements, expectedReplacements);
    };
    bad("\x80", 1);                    // lone continuation
    bad("\xC0\x80", 2);                // overlong NUL (C0 never valid)
    bad("\xC1\xBF", 2);                // overlong
    bad("\xE0\x80\x80", 3);            // overlong 3-byte
    bad("\xF0\x80\x80\x80", 4);        // overlong 4-byte
    bad("\xED\xA0\x80", 3);            // UTF-16 surrogate U+D800
    bad("\xED\xBF\xBF", 3);            // surrogate U+DFFF
    bad("\xF4\x90\x80\x80", 4);        // above U+10FFFF
    bad("\xF5\x80\x80\x80", 4);        // F5 never valid
    bad("\xFF", 1);
    bad("\xC3", 1);                    // truncated at the end
    bad("\xE2\x82", 1);                // truncated maximal subpart: ONE replacement
    bad("\xF0\x9F\x98", 1);
    bad("\xC3(", 1);                   // lead then ASCII: replacement, ASCII survives
    // Valid text around an ill-formed byte survives.
    bool invalid = false;
    CHECK(decodeUtf8("a\xFF" "b", &invalid) == utf32("a\xEF\xBF\xBD" "b"));
    CHECK(invalid);
    CHECK(decodeUtf8("\xC3(", nullptr) == utf32("\xEF\xBF\xBD("));
}

RIVET_TEST(utf8EncodeRoundTrip) {
    CHECK(encodeUtf8(std::u32string{}) == "");
    const std::string samples[] = {"abc", "a\xC3\xA9\xD0\x9F\xE2\x82\xAC\xF0\x9F\x98\x80", "\xF4\x8F\xBF\xBF"};
    for (const std::string& s : samples) {
        bool invalid = true;
        CHECK(encodeUtf8(decodeUtf8(s, &invalid)) == s);
        CHECK(!invalid);
    }
    // Unencodable scalars become U+FFFD.
    const std::u32string bad = {char32_t{0xD800}, char32_t{0x110000}};
    CHECK(encodeUtf8(bad) == "\xEF\xBF\xBD\xEF\xBF\xBD");
}

// --- Layout ------------------------------------------------------------------

RIVET_TEST(layoutEmptyAndSimple) {
    CHECK(layout("", 0) == Lines{""});
    CHECK(layout("", 100) == Lines{""});
    CHECK(layout("hello", 0) == Lines{"hello"});
    const PdfTextLayoutResult one = layoutTextBlock("abc", 0, fixedAdvance);
    CHECK_EQ(one.lines.size(), std::size_t{1});
    CHECK(one.lines[0].width == 30.0);
    CHECK(!one.hadInvalidUtf8);
}

RIVET_TEST(layoutHardBreaks) {
    CHECK(layout("a\nb", 0) == (Lines{"a", "b"}));
    CHECK(layout("a\r\nb", 0) == (Lines{"a", "b"}));
    CHECK(layout("a\rb", 0) == (Lines{"a", "b"}));
    CHECK(layout("a\n", 0) == (Lines{"a", ""}));
    CHECK(layout("\n", 0) == (Lines{"", ""}));
    CHECK(layout("a\n\nb", 0) == (Lines{"a", "", "b"}));
    CHECK(layout("a\r\n\r\nb", 0) == (Lines{"a", "", "b"}));
    CHECK(layout("a\n\rb", 0) == (Lines{"a", "", "b"})); // LF then CR: two breaks
}

RIVET_TEST(layoutControlsAndTabs) {
    CHECK(layout("a\tb", 0) == (Lines{"a b"}));
    CHECK(layout("a\x01" "b\x1F" "c\x7F", 0) == (Lines{"abc"}));
    CHECK(layout("a\xC2\x85" "b", 0) == (Lines{"ab"})); // U+0085 (C1)
    CHECK(layout("a\xC2\x9F" "b", 0) == (Lines{"ab"}));
    // Non-controls that look special are kept.
    CHECK(layout("a\xC2\xA0" "b", 0) == (Lines{"a\xC2\xA0" "b"})); // NBSP
}

RIVET_TEST(layoutInvalidUtf8) {
    const PdfTextLayoutResult r = layoutTextBlock("a\xFF" "b", 0, fixedAdvance);
    CHECK(r.hadInvalidUtf8);
    CHECK(linesOf(r) == (Lines{"a\xEF\xBF\xBD" "b"}));
}

RIVET_TEST(layoutWrapsAtSpaces) {
    // 10 per char: width 50 fits 5 chars.
    CHECK(layout("aaa bbb ccc", 50) == (Lines{"aaa", "bbb", "ccc"}));
    CHECK(layout("aa bb cc", 50) == (Lines{"aa bb", "cc"}));
    CHECK(layout("aa bb", 50) == (Lines{"aa bb"}));          // exactly fits
    CHECK(layout("aa bbb", 50) == (Lines{"aa", "bbb"}));
    // Spaces at a break are dropped from both lines.
    CHECK(layout("aaaa     bbbb", 50) == (Lines{"aaaa", "bbbb"}));
    CHECK(layout("aa   bb", 100) == (Lines{"aa   bb"}));     // run kept when it fits
    // Leading spaces are kept, trailing spaces at paragraph end are kept.
    CHECK(layout("  ab", 100) == (Lines{"  ab"}));
    CHECK(layout("ab  ", 100) == (Lines{"ab  "}));
    CHECK(layout("   ", 100) == (Lines{"   "}));
    // Wrapping applies per paragraph.
    CHECK(layout("aa bb cc\ndd ee", 50) == (Lines{"aa bb", "cc", "dd ee"}));
}

RIVET_TEST(layoutLineWidthsMatchAdvances) {
    const PdfTextLayoutResult r = layoutTextBlock("aaa bbb", 40, fixedAdvance);
    CHECK_EQ(r.lines.size(), std::size_t{2});
    CHECK(r.lines[0].width == 30.0);
    CHECK(r.lines[1].width == 30.0);
}

RIVET_TEST(layoutBreaksLongWords) {
    CHECK(layout("abcdefghij", 30) == (Lines{"abc", "def", "ghi", "j"}));
    // A long word after short text starts on its own line first.
    CHECK(layout("ab abcdefgh", 30) == (Lines{"ab", "abc", "def", "gh"}));
    CHECK(layout("abcdefgh ij", 30) == (Lines{"abc", "def", "gh", "ij"}));
    // Narrower than one glyph: still one code point per line, no endless loop.
    CHECK(layout("abc", 1) == (Lines{"a", "b", "c"}));
    CHECK(layout("a b", 1) == (Lines{"a", "b"}));
}

RIVET_TEST(layoutNeverBreaksBeforeCombiningMark) {
    // "e" + U+0301 (combining acute, zero width here).
    const std::string e = "e\xCC\x81";
    CHECK(layout(e + e + e, 20) == (Lines{e + e, e}));
    // Marks stay with their base even when wrapWidth is tiny.
    CHECK(layout(e + e, 1) == (Lines{e, e}));
    // A leading mark (no base) is its own cluster, never dropped.
    CHECK(layout("\xCC\x81" "a", 0) == (Lines{"\xCC\x81" "a"}));
    const auto lines = layout("\xCC\x81" "ab", 10);
    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK(lines[0] == "\xCC\x81" "a");
    CHECK(lines[1] == "b");
}

RIVET_TEST(layoutCyrillicAndGreek) {
    CHECK(layout("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xD0\xBC\xD0\xB8\xD1\x80", 70) ==
          (Lines{"\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", "\xD0\xBC\xD0\xB8\xD1\x80"}));
    CHECK(layout("\xCE\xB1\xCE\xB2 \xCE\xB3", 100) == (Lines{"\xCE\xB1\xCE\xB2 \xCE\xB3"}));
}

RIVET_TEST(layoutDeterministicAndAdvanceDriven) {
    const std::string text = "The quick brown fox jumps over the lazy dog\nSecond line here";
    const auto first = layout(text, 120);
    CHECK(layout(text, 120) == first);
    CHECK(layout(text, 120) == first);
    // Proportional advances change the breaks (narrow letters fit more).
    auto proportional = [](char32_t c) { return c == U'i' || c == U'l' ? 3.0 : (c == U' ' ? 4.0 : 10.0); };
    const auto prop = linesOf(layoutTextBlock("iiii llll wwww", 60, proportional));
    CHECK(prop == (Lines{"iiii llll", "wwww"}));
    // Zero / negative / NaN advances never poison the widths.
    auto weird = [](char32_t) { return -5.0; };
    const PdfTextLayoutResult w = layoutTextBlock("abc def", 20, weird);
    CHECK_EQ(w.lines.size(), std::size_t{1});
    CHECK(w.lines[0].width == 0.0);
}

RIVET_TEST(layoutHugeInputStaysLinear) {
    const std::string text(60000, 'x');
    const PdfTextLayoutResult r = layoutTextBlock(text, 100, fixedAdvance);
    CHECK_EQ(r.lines.size(), std::size_t{6000});
    const std::string words = [] {
        std::string s;
        for (int i = 0; i < 12000; ++i) s += "word ";
        return s;
    }();
    const PdfTextLayoutResult w = layoutTextBlock(words, 250, fixedAdvance);
    CHECK_GT(w.lines.size(), std::size_t{1000});
}
