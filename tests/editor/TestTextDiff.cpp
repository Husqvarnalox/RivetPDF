// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "editor/TextDiff.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

using rivet::editor::DiffGranularity;
using rivet::editor::DiffHunk;
using rivet::editor::DiffLimits;
using rivet::editor::DiffToken;
using rivet::editor::TextDiffResult;
using rivet::editor::diffTokens;

namespace {

// "one two three" -> tokens; '|' starts a new line.
std::vector<DiffToken> words(const std::string& text) {
    std::vector<DiffToken> out;
    std::istringstream in(text);
    std::string word;
    std::uint32_t line = 0;
    while (in >> word) {
        if (word == "|") {
            ++line;
            continue;
        }
        out.push_back(DiffToken{word, line});
    }
    return out;
}

// Applies the hunks to `a`: equal runs come from a, inserted ranges from b.
std::vector<std::string> rebuild(const std::vector<DiffToken>& a, const std::vector<DiffToken>& b,
                                 const std::vector<DiffHunk>& hunks) {
    std::vector<std::string> out;
    std::size_t i = 0;
    for (const DiffHunk& hunk : hunks) {
        while (i < hunk.aBegin) out.push_back(a[i++].text);
        i = hunk.aEnd;
        for (std::size_t j = hunk.bBegin; j < hunk.bEnd; ++j) out.push_back(b[j].text);
    }
    while (i < a.size()) out.push_back(a[i++].text);
    return out;
}

bool rebuildsTo(const std::vector<DiffToken>& a, const std::vector<DiffToken>& b,
                const std::vector<DiffHunk>& hunks) {
    const std::vector<std::string> got = rebuild(a, b, hunks);
    if (got.size() != b.size()) return false;
    for (std::size_t i = 0; i < b.size(); ++i) {
        if (got[i] != b[i].text) return false;
    }
    return true;
}

std::size_t editCount(const std::vector<DiffHunk>& hunks) {
    std::size_t total = 0;
    for (const DiffHunk& hunk : hunks) total += (hunk.aEnd - hunk.aBegin) + (hunk.bEnd - hunk.bBegin);
    return total;
}

std::size_t lcsLength(const std::vector<DiffToken>& a, const std::vector<DiffToken>& b) {
    std::vector<std::vector<std::size_t>> table(a.size() + 1, std::vector<std::size_t>(b.size() + 1, 0));
    for (std::size_t i = 1; i <= a.size(); ++i) {
        for (std::size_t j = 1; j <= b.size(); ++j) {
            table[i][j] = a[i - 1].text == b[j - 1].text ? table[i - 1][j - 1] + 1
                                                         : std::max(table[i - 1][j], table[i][j - 1]);
        }
    }
    return table[a.size()][b.size()];
}

} // namespace

RIVET_TEST(textDiffEmptyAndIdentical) {
    CHECK(diffTokens({}, {}).hunks.empty());
    const auto a = words("alpha beta gamma");
    CHECK(diffTokens(a, a).hunks.empty());
    const auto onlyInsert = diffTokens({}, a);
    CHECK_EQ(onlyInsert.hunks.size(), std::size_t{1});
    CHECK((onlyInsert.hunks[0] == DiffHunk{0, 0, 0, 3}));
    const auto onlyDelete = diffTokens(a, {});
    CHECK_EQ(onlyDelete.hunks.size(), std::size_t{1});
    CHECK((onlyDelete.hunks[0] == DiffHunk{0, 3, 0, 0}));
}

RIVET_TEST(textDiffAllChanged) {
    const auto a = words("a b c");
    const auto b = words("x y z w");
    const TextDiffResult result = diffTokens(a, b);
    CHECK_EQ(result.hunks.size(), std::size_t{1});
    CHECK((result.hunks[0] == DiffHunk{0, 3, 0, 4}));
    CHECK(result.granularity == DiffGranularity::Word);
}

RIVET_TEST(textDiffInsertAndDeleteAtEnds) {
    {
        const auto a = words("b c d");
        const auto b = words("a b c d e");
        const TextDiffResult result = diffTokens(a, b);
        CHECK_EQ(result.hunks.size(), std::size_t{2});
        CHECK((result.hunks[0] == DiffHunk{0, 0, 0, 1}));
        CHECK((result.hunks[1] == DiffHunk{3, 3, 4, 5}));
        CHECK(rebuildsTo(a, b, result.hunks));
    }
    {
        const auto a = words("a b c d e");
        const auto b = words("b c d");
        const TextDiffResult result = diffTokens(a, b);
        CHECK_EQ(result.hunks.size(), std::size_t{2});
        CHECK((result.hunks[0] == DiffHunk{0, 1, 0, 0}));
        CHECK((result.hunks[1] == DiffHunk{4, 5, 3, 3}));
        CHECK(rebuildsTo(a, b, result.hunks));
    }
}

RIVET_TEST(textDiffReplaceInMiddle) {
    const auto a = words("the quick brown fox jumps");
    const auto b = words("the quick red fox jumps");
    const TextDiffResult result = diffTokens(a, b);
    CHECK_EQ(result.hunks.size(), std::size_t{1});
    CHECK((result.hunks[0] == DiffHunk{2, 3, 2, 3}));
}

RIVET_TEST(textDiffUnicodeWords) {
    const auto a = words("Привет мир 你好 世界 café");
    const auto b = words("Привет весь мир 你好 café");
    const TextDiffResult result = diffTokens(a, b);
    CHECK(rebuildsTo(a, b, result.hunks));
    CHECK_EQ(editCount(result.hunks), std::size_t{2}); // + весь, - 世界
}

RIVET_TEST(textDiffIsMinimalAndReconstructs) {
    // Deterministic pseudo-random small-alphabet sequences: the edit script
    // must reproduce b and have the minimal size n + m - 2 * LCS.
    std::uint32_t state = 12345;
    const auto next = [&state](std::uint32_t bound) {
        state = state * 1664525u + 1013904223u;
        return (state >> 16) % bound;
    };
    for (int round = 0; round < 200; ++round) {
        std::vector<DiffToken> a;
        std::vector<DiffToken> b;
        const std::uint32_t n = next(40);
        const std::uint32_t m = next(40);
        const std::uint32_t alphabet = 2 + next(6);
        for (std::uint32_t i = 0; i < n; ++i) a.push_back(DiffToken{std::string(1, 'a' + next(alphabet)), 0});
        for (std::uint32_t i = 0; i < m; ++i) b.push_back(DiffToken{std::string(1, 'a' + next(alphabet)), 0});
        const TextDiffResult result = diffTokens(a, b);
        CHECK(rebuildsTo(a, b, result.hunks));
        CHECK_EQ(editCount(result.hunks), a.size() + b.size() - 2 * lcsLength(a, b));
        // Hunks ascend and never touch.
        for (std::size_t h = 1; h < result.hunks.size(); ++h) {
            CHECK_GT(result.hunks[h].aBegin, result.hunks[h - 1].aEnd);
            CHECK_GT(result.hunks[h].bBegin, result.hunks[h - 1].bEnd);
        }
    }
}

RIVET_TEST(textDiffTokenBoundFallsBackToLines) {
    // Three lines of 4 words each; limit forces the per-line path.
    const auto a = words("a b c d | e f g h | i j k l");
    const auto b = words("a b c d | e f X h | i j k l");
    DiffLimits limits;
    limits.maxTokens = 8;
    const TextDiffResult result = diffTokens(a, b, limits);
    CHECK(result.granularity == DiffGranularity::Line);
    CHECK_EQ(result.hunks.size(), std::size_t{1});
    // The whole second line (4 words) is the hunk.
    CHECK((result.hunks[0] == DiffHunk{4, 8, 4, 8}));
    CHECK(rebuildsTo(a, b, result.hunks));
}

RIVET_TEST(textDiffLineBoundExceededIsCoarse) {
    std::string left;
    std::string right;
    for (int i = 0; i < 12; ++i) {
        left += "L" + std::to_string(i) + " | ";
        right += "R" + std::to_string(i) + " | ";
    }
    const auto a = words("same " + left + "tail");
    const auto b = words("same " + right + "tail");
    DiffLimits limits;
    limits.maxTokens = 5;
    const TextDiffResult result = diffTokens(a, b, limits);
    CHECK(result.granularity == DiffGranularity::Coarse);
    CHECK_EQ(result.hunks.size(), std::size_t{1});
    CHECK((result.hunks[0] == DiffHunk{1, 13, 1, 13}));
    CHECK(rebuildsTo(a, b, result.hunks));
}

RIVET_TEST(textDiffEditDistanceBoundFallsBack) {
    // Disjoint 30-word lines: word-level D = 60 > the bound -> line fallback.
    std::string left;
    std::string right;
    for (int i = 0; i < 30; ++i) {
        left += "x" + std::to_string(i) + " ";
        right += "y" + std::to_string(i) + " ";
    }
    const auto a = words("keep | " + left);
    const auto b = words("keep | " + right);
    DiffLimits limits;
    limits.maxEditDistance = 10;
    const TextDiffResult result = diffTokens(a, b, limits);
    CHECK(result.granularity == DiffGranularity::Line);
    CHECK_EQ(result.hunks.size(), std::size_t{1});
    CHECK((result.hunks[0] == DiffHunk{1, 31, 1, 31}));
}

RIVET_TEST(textDiffLargePageStaysWithinBounds) {
    // 20000 identical-prefix words with a few scattered edits: handled at word
    // level quickly (trimming + small D).
    std::vector<DiffToken> a;
    for (std::uint32_t i = 0; i < 20000; ++i) a.push_back(DiffToken{"w" + std::to_string(i), i / 10});
    std::vector<DiffToken> b = a;
    b[5000].text = "changed";
    b.erase(b.begin() + 12000);
    b.insert(b.begin() + 17000, DiffToken{"added", 1700});
    const TextDiffResult result = diffTokens(a, b);
    CHECK(result.granularity == DiffGranularity::Word);
    CHECK_EQ(result.hunks.size(), std::size_t{3});
    CHECK(rebuildsTo(a, b, result.hunks));

    // Above the bound: line fallback, still correct.
    a.push_back(DiffToken{"extra", 3000});
    b.push_back(DiffToken{"extra", 3000});
    b.push_back(DiffToken{"more", 3000});
    const TextDiffResult over = diffTokens(a, b);
    CHECK(over.granularity == DiffGranularity::Line);
    CHECK(!over.hunks.empty());
}

RIVET_TEST(textDiffCancellation) {
    std::vector<DiffToken> a;
    std::vector<DiffToken> b;
    for (int i = 0; i < 400; ++i) {
        a.push_back(DiffToken{"a" + std::to_string(i), 0});
        b.push_back(DiffToken{"b" + std::to_string(i), 0});
    }
    std::atomic<int> polls{0};
    const TextDiffResult result = diffTokens(a, b, {}, [&polls] {
        ++polls;
        return true;
    });
    CHECK(result.cancelled);
    CHECK(result.hunks.empty());
    CHECK_GE(polls.load(), 1);
}
