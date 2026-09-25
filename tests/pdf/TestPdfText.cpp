#include "RivetTest.h"

#include "core/Error.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfText.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// PDFium-free tests of the Rivet text model (PdfTextPage, folding, search).
// These run in BOTH build modes: they link only rivet::pdf and the harness,
// never PDFium. The adapter-level extraction tests live in TestPdfiumText.cpp.

namespace {

namespace core = rivet::core;
using rivet::pdf::PdfTextPage;
using rivet::pdf::TextChar;
using rivet::pdf::TextSearchOptions;
using rivet::pdf::TextSearchResult;

constexpr char32_t kReplacement = 0xFFFD;

// Lays `text` out as a grid of `perRow` glyph cells per row: cell
// (row, col) gets the box x [col*cellW, col*cellW + glyphW],
// y [row*cellH, row*cellH + glyphH] (display space, y-down). Chars on one
// grid row share the y-center, so they cluster into one model row.
PdfTextPage makeGridPage(const std::u32string& text, std::size_t perRow, double cellW = 10.0,
                         double cellH = 12.0, double glyphW = 8.0, double glyphH = 10.0) {
    std::vector<TextChar> chars;
    chars.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        TextChar ch;
        ch.unicode = text[i];
        ch.index = static_cast<std::uint32_t>(i);
        const std::size_t row = i / perRow;
        const std::size_t col = i % perRow;
        ch.bounds = core::Rect{core::Point{col * cellW, row * cellH}, core::Size{glyphW, glyphH}};
        ch.fontSize = 10.0;
        chars.push_back(ch);
    }
    return PdfTextPage(chars);
}

// Lays `text` out as a single VERTICAL run (a /Rotate 90 line): every char
// at the same x, stepping down by `step` > 0.5 * fontSize, so each char
// clusters into its own model row - the model's view of vertical text.
PdfTextPage makeVerticalPage(const std::u32string& text, double step = 14.0) {
    std::vector<TextChar> chars;
    chars.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        TextChar ch;
        ch.unicode = text[i];
        ch.index = static_cast<std::uint32_t>(i);
        ch.bounds = core::Rect{core::Point{0.0, static_cast<double>(i) * step},
                               core::Size{10.0, 10.0}};
        ch.fontSize = 10.0;
        chars.push_back(ch);
    }
    return PdfTextPage(chars);
}

// A minimal concrete PdfDocument: proves the textPage() default
// implementation keeps engineless fakes compiling, and returns NotAvailable.
class FakeDocument final : public rivet::pdf::PdfDocument {
public:
    const rivet::pdf::PdfDocumentInfo& info() const override { return info_; }

    core::Result<rivet::pdf::PdfPageInfo> pageInfo(std::size_t) const override {
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable, "fake", "pdf"));
    }

    core::Result<core::Bitmap> renderPage(std::size_t, const core::Rect&, double) override {
        return std::unexpected(core::makeError(core::ErrorCode::NotAvailable, "fake", "pdf"));
    }

private:
    rivet::pdf::PdfDocumentInfo info_;
};

// Encodes code points as UTF-8 (the test's needle builder; the model has its
// own encoder).
std::string utf8(const std::u32string& codePoints) {
    std::string out;
    for (const char32_t cp : codePoints) {
        const std::uint32_t c = static_cast<std::uint32_t>(cp);
        if (c <= 0x7Fu) {
            out.push_back(static_cast<char>(c));
        } else if (c <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (c >> 6)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else if (c <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (c >> 12)));
            out.push_back(static_cast<char>(0x80u | ((c >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (c >> 18)));
            out.push_back(static_cast<char>(0x80u | ((c >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((c >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (c & 0x3Fu)));
        }
    }
    return out;
}

} // namespace

// foldCodePoint covers ASCII, Latin-1, Greek and Cyrillic uppercase ranges;
// lowercase and unknown code points fold to themselves.
RIVET_TEST(pdfTextFoldCodePoint) {
    using rivet::pdf::foldCodePoint;
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(U'A')), static_cast<char32_t>(U'a'));
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(U'Z')), static_cast<char32_t>(U'z'));
    // Cyrillic: U+042F (Я) -> U+044F (я); U+0410 (А) -> U+0430 (а).
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0x042F)), static_cast<char32_t>(0x044F));
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0x0410)), static_cast<char32_t>(0x0430));
    // Greek: U+03A9 (Ω) -> U+03C9 (ω).
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0x03A9)), static_cast<char32_t>(0x03C9));
    // Latin-1: U+00C9 (É) -> U+00E9 (é); multiplication sign folds to itself.
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0xC9)), static_cast<char32_t>(0xE9));
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0xD7)), static_cast<char32_t>(0xD7));
    // U+0178 (Ÿ) -> U+00FF (ÿ).
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0x178)), static_cast<char32_t>(0xFF));
    // Already-lowercase code points are unchanged.
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(U'a')), static_cast<char32_t>(U'a'));
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0x044F)), static_cast<char32_t>(0x044F));
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0x03C9)), static_cast<char32_t>(0x03C9));
    // Unknown / unassigned code points fold to themselves.
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(0x263A)), static_cast<char32_t>(0x263A));
    CHECK_EQ(foldCodePoint(kReplacement), kReplacement);
    CHECK_EQ(foldCodePoint(static_cast<char32_t>(U' ')), static_cast<char32_t>(U' '));
}

// isWordChar: letters, digits and underscore are word characters; spaces,
// punctuation and symbols are not. Cyrillic and Greek letters count.
RIVET_TEST(pdfTextIsWordChar) {
    using rivet::pdf::isWordChar;
    CHECK(isWordChar(static_cast<char32_t>(U'a')));
    CHECK(isWordChar(static_cast<char32_t>(U'Z')));
    CHECK(isWordChar(static_cast<char32_t>(U'0')));
    CHECK(isWordChar(static_cast<char32_t>(U'9')));
    CHECK(isWordChar(static_cast<char32_t>(U'_')));
    CHECK(isWordChar(static_cast<char32_t>(0x0430))); // а
    CHECK(isWordChar(static_cast<char32_t>(0x042F))); // Я
    CHECK(isWordChar(static_cast<char32_t>(0x03B1))); // α
    CHECK(!isWordChar(static_cast<char32_t>(U' ')));
    CHECK(!isWordChar(static_cast<char32_t>(U'!')));
    CHECK(!isWordChar(static_cast<char32_t>(U'-')));
    CHECK(!isWordChar(static_cast<char32_t>(U'.')));
    CHECK(!isWordChar(static_cast<char32_t>(0x263A)));
}

// Construction: text() is the UTF-8 encoding of the char code points with one
// code point per char, charCount() matches, and memoryBytes() accounts for
// the chars.
RIVET_TEST(pdfTextPageBuildsUtf8Text) {
    // "Abя": UTF-8 bytes 'A', 'b', 0xD1 0x8F.
    const std::u32string content{U'A', U'b', static_cast<char32_t>(0x044F)};
    const PdfTextPage page = makeGridPage(content, 3);
    CHECK_EQ(page.charCount(), std::size_t{3});
    CHECK_EQ(page.text(), std::string("Ab\xD1\x8F"));
    for (std::size_t i = 0; i < page.chars().size(); ++i) {
        CHECK_EQ(page.chars()[i].index, static_cast<std::uint32_t>(i));
        CHECK_EQ(page.chars()[i].unicode, content[i]);
    }
    CHECK_GT(page.memoryBytes(), page.charCount() * sizeof(TextChar));
    CHECK_GT(page.memoryBytes(), page.text().size());

    // Default-constructed page: empty and safe to query.
    const PdfTextPage empty;
    CHECK_EQ(empty.charCount(), std::size_t{0});
    CHECK(empty.text().empty());
    CHECK(empty.rectsForRange(0, 10).empty());
    CHECK(!empty.charIndexAtPoint(core::Point{0.0, 0.0}).has_value());
}

// rectsForRange merges chars on the same text row into one rect per row,
// skips zero-area chars, and clamps the requested range.
RIVET_TEST(pdfTextRectsForRange) {
    // One row of four chars: one union rect.
    const PdfTextPage single = makeGridPage(U"abcd", 4);
    const std::vector<core::Rect> singleRects = single.rectsForRange(0, 4);
    CHECK_EQ(singleRects.size(), std::size_t{1});
    CHECK(core::Rect::nearlyEqual(singleRects[0], core::Rect{0.0, 0.0, 38.0, 10.0}));

    // Two rows of three: two rects with disjoint y bands.
    const PdfTextPage twoRows = makeGridPage(U"abcdef", 3);
    const std::vector<core::Rect> rows = twoRows.rectsForRange(0, 6);
    CHECK_EQ(rows.size(), std::size_t{2});
    CHECK_LT(rows[0].maxY(), rows[1].minY()); // reading order: row 0 above row 1
    CHECK(core::Rect::nearlyEqual(rows[0], core::Rect{0.0, 0.0, 28.0, 10.0}));
    CHECK(core::Rect::nearlyEqual(rows[1], core::Rect{0.0, 12.0, 28.0, 10.0}));

    // A full-line range yields one rect per row (grouping across the whole
    // page covers exactly the rows that exist).
    CHECK_EQ(rows.size(), twoRows.rectsForRange(0, 1000).size());

    // Count clamping: a range running past the end covers the tail only.
    const std::vector<core::Rect> tail = twoRows.rectsForRange(3, 100);
    CHECK_EQ(tail.size(), std::size_t{1});
    CHECK(core::Rect::nearlyEqual(tail[0], rows[1]));

    // Empty and out-of-range ranges produce nothing.
    CHECK(twoRows.rectsForRange(0, 0).empty());
    CHECK(twoRows.rectsForRange(6, 2).empty());
    CHECK(twoRows.rectsForRange(100, 2).empty());

    // Zero-area chars (line breaks) contribute nothing and do not split a
    // row's rect when they sit inside one; a break between rows separates
    // the row rects.
    std::vector<TextChar> withBreak;
    const std::u32string content{U'a', U'b', 0x0D, U'c', U'd'};
    for (std::size_t i = 0; i < content.size(); ++i) {
        TextChar ch;
        ch.unicode = content[i];
        ch.index = static_cast<std::uint32_t>(i);
        if (content[i] == 0x0D) {
            ch.bounds = core::Rect{}; // generated line break: zero area
        } else {
            const std::size_t row = i < 3 ? 0 : 1;
            const std::size_t col = i < 3 ? i : i - 3;
            ch.bounds = core::Rect{core::Point{col * 10.0, row * 12.0}, core::Size{8.0, 10.0}};
        }
        ch.fontSize = 10.0;
        withBreak.push_back(ch);
    }
    const PdfTextPage broken(withBreak);
    const std::vector<core::Rect> brokenRects = broken.rectsForRange(0, 5);
    CHECK_EQ(brokenRects.size(), std::size_t{2});
    CHECK(core::Rect::nearlyEqual(brokenRects[0], core::Rect{0.0, 0.0, 18.0, 10.0}));
    CHECK(core::Rect::nearlyEqual(brokenRects[1], core::Rect{0.0, 12.0, 18.0, 10.0}));
}

// charIndexAtPoint picks the nearest row (ties -> earlier), then the nearest
// x-center within the row (ties -> earlier), including the rotated-page case
// where rows run vertically.
RIVET_TEST(pdfTextCharIndexAtPoint) {
    const PdfTextPage line = makeGridPage(U"abc", 3);
    // Char x-centers: 4, 14, 24; row y-center 5.
    CHECK_EQ(line.charIndexAtPoint(core::Point{2.0, 5.0}).value(), 0u);
    CHECK_EQ(line.charIndexAtPoint(core::Point{14.0, 5.0}).value(), 1u);
    CHECK_EQ(line.charIndexAtPoint(core::Point{23.0, 5.0}).value(), 2u);
    // Between two glyphs, equidistant from both x-centers -> earlier char.
    CHECK_EQ(line.charIndexAtPoint(core::Point{9.0, 5.0}).value(), 0u);
    CHECK_EQ(line.charIndexAtPoint(core::Point{19.0, 5.0}).value(), 1u);
    // Far left / far right of the line still hit the nearest end chars.
    CHECK_EQ(line.charIndexAtPoint(core::Point{-50.0, 5.0}).value(), 0u);
    CHECK_EQ(line.charIndexAtPoint(core::Point{1000.0, 5.0}).value(), 2u);
    // Off-row vertically: nearest row wins regardless of x.
    CHECK_EQ(line.charIndexAtPoint(core::Point{14.0, 100.0}).value(), 1u);

    const PdfTextPage twoRows = makeGridPage(U"abcdef", 3);
    // Row y-centers: 5 and 17. Equidistant point -> earlier row.
    CHECK_EQ(twoRows.charIndexAtPoint(core::Point{2.0, 11.0}).value(), 0u);
    // Closer to the second row -> its first char.
    CHECK_EQ(twoRows.charIndexAtPoint(core::Point{2.0, 11.5}).value(), 3u);
    CHECK_EQ(twoRows.charIndexAtPoint(core::Point{22.0, 20.0}).value(), 5u);

    // Rotated-page case: a CW90 page's line runs vertically, so each char is
    // its own row and the hit test effectively walks down the line.
    const PdfTextPage vertical = makeVerticalPage(U"abcd");
    CHECK_EQ(vertical.charIndexAtPoint(core::Point{5.0, 3.0}).value(), 0u);
    CHECK_EQ(vertical.charIndexAtPoint(core::Point{5.0, 21.0}).value(), 1u);
    CHECK_EQ(vertical.charIndexAtPoint(core::Point{5.0, 45.0}).value(), 3u);
    CHECK_EQ(vertical.charIndexAtPoint(core::Point{5.0, 500.0}).value(), 3u);

    // No selectable chars -> nullopt.
    CHECK(!PdfTextPage().charIndexAtPoint(core::Point{1.0, 1.0}).has_value());
    std::vector<TextChar> onlyEmpty(1);
    onlyEmpty[0].unicode = 0x0D;
    onlyEmpty[0].index = 0;
    onlyEmpty[0].bounds = core::Rect{};
    const PdfTextPage emptyOnly(onlyEmpty);
    CHECK(!emptyOnly.charIndexAtPoint(core::Point{1.0, 1.0}).has_value());
}

// searchTextPage: matches in page order, Unicode case-insensitive folding,
// case-sensitive misses, whole-word boundaries, and non-overlapping scans.
RIVET_TEST(pdfTextSearchBasics) {
    const PdfTextPage page = makeGridPage(U"Hello Rivet, says Rivet", 24);

    // Plain match, two hits in page order.
    std::vector<TextSearchResult> hits = rivet::pdf::searchTextPage(page, "Rivet");
    CHECK_EQ(hits.size(), std::size_t{2});
    CHECK_EQ(hits[0], (TextSearchResult{6, 5}));
    CHECK_EQ(hits[1], (TextSearchResult{18, 5}));

    // Case-insensitive needle matches through folding.
    hits = rivet::pdf::searchTextPage(page, "rivet");
    CHECK_EQ(hits.size(), std::size_t{2});
    CHECK_EQ(hits[0].startIndex, 6u);
    CHECK_EQ(hits[1].startIndex, 18u);

    // Uppercase needle: case-sensitive misses, folded search finds both.
    CHECK(rivet::pdf::searchTextPage(page, "RIVET", TextSearchOptions{true, false}).empty());
    hits = rivet::pdf::searchTextPage(page, "RIVET", TextSearchOptions{false, false});
    CHECK_EQ(hits.size(), std::size_t{2});

    // Empty needle: no results.
    CHECK(rivet::pdf::searchTextPage(page, "").empty());

    // Needle longer than the text: no results.
    CHECK(rivet::pdf::searchTextPage(makeGridPage(U"abc", 3), "abcdef").empty());

    // Non-overlapping: "aa" in "aaa" matches once.
    const PdfTextPage aaa = makeGridPage(U"aaa", 3);
    hits = rivet::pdf::searchTextPage(aaa, "aa");
    CHECK_EQ(hits.size(), std::size_t{1});
    CHECK_EQ(hits[0], (TextSearchResult{0, 2}));
}

RIVET_TEST(pdfTextSearchWholeWord) {
    const PdfTextPage page = makeGridPage(U"Rivets Rivet", 12);
    // Without wholeWord: both occurrences.
    std::vector<TextSearchResult> hits = rivet::pdf::searchTextPage(page, "Rivet");
    CHECK_EQ(hits.size(), std::size_t{2});
    CHECK_EQ(hits[0].startIndex, 0u);
    CHECK_EQ(hits[1].startIndex, 7u);
    // With wholeWord: only the standalone word (the first is followed by 's').
    hits = rivet::pdf::searchTextPage(page, "Rivet", TextSearchOptions{false, true});
    CHECK_EQ(hits.size(), std::size_t{1});
    CHECK_EQ(hits[0], (TextSearchResult{7, 5}));
    // Word at the very start/end counts as bounded.
    const PdfTextPage solo = makeGridPage(U"Rivet", 5);
    hits = rivet::pdf::searchTextPage(solo, "Rivet", TextSearchOptions{false, true});
    CHECK_EQ(hits.size(), std::size_t{1});
    CHECK_EQ(hits[0].startIndex, 0u);
}

// Cyrillic folding drives real Unicode case-insensitive matching, not just
// ASCII.
RIVET_TEST(pdfTextSearchCyrillicCaseFolding) {
    // U"Привет": П U+041F, р U+0440, и U+0438, в U+0432, е U+0435, т U+0442.
    const std::u32string privet{0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442};
    const PdfTextPage page = makeGridPage(privet, 6);

    // Lowercase needle "ривет" matches inside "Привет" through folding.
    const std::u32string lowerNeedle{0x0440, 0x0438, 0x0432, 0x0435, 0x0442};
    std::vector<TextSearchResult> hits = rivet::pdf::searchTextPage(page, utf8(lowerNeedle));
    CHECK_EQ(hits.size(), std::size_t{1});
    CHECK_EQ(hits[0], (TextSearchResult{1, 5}));

    // Uppercase needle "ПРИВЕТ" matches case-insensitively at index 0.
    const std::u32string upperNeedle{0x041F, 0x0420, 0x0418, 0x0412, 0x0415, 0x0422};
    hits = rivet::pdf::searchTextPage(page, utf8(upperNeedle));
    CHECK_EQ(hits.size(), std::size_t{1});
    CHECK_EQ(hits[0], (TextSearchResult{0, 6}));

    // Case-sensitive uppercase: no match.
    CHECK(rivet::pdf::searchTextPage(page, utf8(upperNeedle), TextSearchOptions{true, false}).empty());
}

// The PdfDocument::textPage default implementation: engineless fakes (and the
// null backend) keep compiling and report NotAvailable.
RIVET_TEST(pdfTextDefaultImplIsNotAvailable) {
    FakeDocument doc;
    auto page = doc.textPage(0);
    CHECK(!page.has_value());
    CHECK_EQ(page.error().code, core::ErrorCode::NotAvailable);
    CHECK_EQ(page.error().message, std::string("this backend has no text support"));
    CHECK_EQ(page.error().subsystem, std::string("pdf"));
}
