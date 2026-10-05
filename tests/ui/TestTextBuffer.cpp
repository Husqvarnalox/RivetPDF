// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "ui/TextBuffer.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>
#include <string>
#include <string_view>

using rivet::ui::TextBuffer;

namespace {

// Reference line starts computed from scratch.
std::vector<std::size_t> referenceStarts(const std::string& text) {
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') starts.push_back(i + 1);
    }
    return starts;
}

bool indexMatches(const TextBuffer& buffer) {
    const auto reference = referenceStarts(buffer.text());
    if (reference.size() != buffer.lineCount()) return false;
    for (std::size_t i = 0; i < reference.size(); ++i) {
        if (buffer.lineStart(i) != reference[i]) return false;
    }
    return true;
}

} // namespace

RIVET_TEST(textBufferEmptyHasOneLine) {
    TextBuffer buffer;
    CHECK_EQ(buffer.lineCount(), std::size_t{1});
    CHECK_EQ(buffer.lineStart(0), std::size_t{0});
    CHECK_EQ(buffer.lineEnd(0), std::size_t{0});
    CHECK_EQ(buffer.lineOfOffset(0), std::size_t{0});
}

RIVET_TEST(textBufferLinesAndTrailingNewline) {
    TextBuffer buffer("one\ntwo\n");
    CHECK_EQ(buffer.lineCount(), std::size_t{3}); // last line is empty
    CHECK_EQ(buffer.lineEnd(0), std::size_t{3});
    CHECK_EQ(buffer.lineStart(1), std::size_t{4});
    CHECK_EQ(buffer.lineStart(2), std::size_t{8});
    CHECK_EQ(buffer.lineEnd(2), std::size_t{8});
    CHECK_EQ(buffer.lineOfOffset(3), std::size_t{0}); // the '\n' belongs to line 0
    CHECK_EQ(buffer.lineOfOffset(4), std::size_t{1});
    CHECK_EQ(buffer.lineOfOffset(8), std::size_t{2});
    CHECK_EQ(buffer.lineOfOffset(9999), std::size_t{2});
}

RIVET_TEST(textBufferInsertEraseKeepsIndex) {
    TextBuffer buffer("alpha\nbeta\ngamma");
    CHECK(buffer.insert(5, "\nx\ny"));
    CHECK_EQ(buffer.text(), std::string("alpha\nx\ny\nbeta\ngamma"));
    CHECK(indexMatches(buffer));
    CHECK(buffer.erase(5, 4)); // "\nx\ny"
    CHECK_EQ(buffer.text(), std::string("alpha\nbeta\ngamma"));
    CHECK(indexMatches(buffer));
    CHECK(buffer.replace(3, 9, "")); // span of two '\n'
    CHECK_EQ(buffer.text(), std::string("alpamma"));
    CHECK(indexMatches(buffer));
    CHECK_EQ(buffer.lineCount(), std::size_t{1});
}

RIVET_TEST(textBufferRandomEditsMatchReference) {
    std::mt19937 rng(12345);
    TextBuffer buffer("seed\nline\n\nпривет мир\n");
    const std::string pieces[] = {"a", "\n", "\n\n", "слово", "😀", "xyz\nq", ""};
    for (int step = 0; step < 3000; ++step) {
        const std::size_t size = buffer.size();
        std::size_t offset = std::uniform_int_distribution<std::size_t>(0, size)(rng);
        while (!buffer.isBoundary(offset)) --offset;
        std::size_t end = std::min(size, offset + std::uniform_int_distribution<std::size_t>(0, 6)(rng));
        while (!buffer.isBoundary(end)) ++end;
        const std::string& piece = pieces[std::uniform_int_distribution<std::size_t>(0, 6)(rng)];
        CHECK(buffer.replace(offset, end - offset, piece));
        if (step % 50 == 0) CHECK(indexMatches(buffer));
    }
    CHECK(indexMatches(buffer));
    CHECK(!TextBuffer::needsNormalization(buffer.text()));
}

RIVET_TEST(textBufferCyrillicAndEmojiOffsets) {
    // "я" 2 bytes, "😀" 4 bytes.
    TextBuffer buffer("яя\n😀x");
    CHECK_EQ(buffer.size(), std::size_t{4 + 1 + 4 + 1});
    CHECK(buffer.isBoundary(2));
    CHECK(!buffer.isBoundary(1));
    CHECK(!buffer.isBoundary(6)); // inside the emoji
    CHECK(!buffer.insert(1, "x")); // mid code point: refused
    CHECK(!buffer.erase(5, 1));    // would split the emoji
    CHECK_EQ(buffer.text(), std::string("яя\n😀x"));

    const auto pos = buffer.positionOfOffset(9); // before "x"
    CHECK_EQ(pos.line, std::size_t{1});
    CHECK_EQ(pos.column, std::size_t{4});
    CHECK_EQ(buffer.offsetOfPosition(pos), std::size_t{9});
    // A column inside the emoji floors to its start.
    CHECK_EQ(buffer.offsetOfPosition({1, 2}), std::size_t{5});
    // Clamped to the line.
    CHECK_EQ(buffer.offsetOfPosition({0, 100}), std::size_t{4});
    CHECK_EQ(buffer.offsetOfPosition({99, 0}), std::size_t{5});
}

RIVET_TEST(textBufferNormalizesLineEndings) {
    TextBuffer buffer("a\r\nb\rc\n");
    CHECK_EQ(buffer.text(), std::string("a\nb\nc\n"));
    CHECK(buffer.insert(0, "x\r\ny\rz"));
    CHECK_EQ(buffer.text(), std::string("x\ny\nza\nb\nc\n"));
    std::size_t stored = 0;
    CHECK(buffer.replace(0, 0, "\r\n\r\n", &stored));
    CHECK_EQ(stored, std::size_t{2});
    CHECK(indexMatches(buffer));
}

RIVET_TEST(textBufferReplacesInvalidUtf8) {
    const std::string bad = std::string("ok") + "\xFF" + "\xC0\xAF" + "\xED\xA0\x80" + "\xE2\x82" + "end";
    TextBuffer buffer(bad);
    CHECK(!TextBuffer::needsNormalization(buffer.text()));
    CHECK(buffer.text().find("ok") == 0);
    CHECK(buffer.text().find("end") != std::string::npos);
    CHECK(buffer.text().find("\xEF\xBF\xBD") != std::string::npos);
    CHECK(buffer.insert(0, "\x80\x80"));
    CHECK(!TextBuffer::needsNormalization(buffer.text()));
    // Valid multi-byte input is left alone.
    CHECK_EQ(TextBuffer::normalize("привет 😀 世界"), std::string("привет 😀 世界"));
}

RIVET_TEST(textBufferRejectsBadRanges) {
    TextBuffer buffer("abc");
    CHECK(!buffer.replace(4, 0, "x"));
    CHECK(!buffer.replace(1, 5, "x"));
    CHECK(buffer.replace(3, 0, "d")); // at the end is fine
    CHECK_EQ(buffer.text(), std::string("abcd"));
    CHECK_EQ(buffer.slice(1, 100), std::string("bcd"));
    CHECK_EQ(buffer.slice(100, 1), std::string());
    CHECK_EQ(buffer.snapshot(), std::string("abcd"));
}

RIVET_TEST(textBufferRevisionAndLongestLine) {
    TextBuffer buffer("a\nlonger line\nb");
    const std::uint64_t before = buffer.revision();
    CHECK_EQ(buffer.longestLine(), std::size_t{1});
    CHECK_EQ(buffer.longestLineBytes(), std::size_t{11});
    CHECK(buffer.insert(0, "x"));
    CHECK(buffer.revision() != before);
    CHECK(buffer.insert(0, "")); // no-op: no new revision needed, still fine
    buffer.insert(buffer.size(), std::string(40, 'z'));
    CHECK_EQ(buffer.longestLine(), std::size_t{2});
    CHECK_EQ(buffer.longestLineBytes(), std::size_t{41});
}

RIVET_TEST(textBufferLargeDocumentEditsStayConsistent) {
    // 5 MB smoke: many edits in the middle keep the index exact. No timing
    // assertion (see PerfMarkdown for numbers); this only guards against
    // pathological behaviour such as a rebuild per edit hanging the suite.
    std::string big;
    big.reserve(5u << 20);
    while (big.size() < (5u << 20)) big += "Строка markdown текста number one two three\n";
    TextBuffer buffer(big);
    const std::size_t lines = buffer.lineCount();
    for (int i = 0; i < 400; ++i) {
        const std::size_t offset = buffer.lineStart(buffer.lineCount() / 2);
        CHECK(buffer.insert(offset, i % 7 == 0 ? "новая\nстрока\n" : "я"));
    }
    CHECK(buffer.lineCount() > lines);
    CHECK(indexMatches(buffer));
}
