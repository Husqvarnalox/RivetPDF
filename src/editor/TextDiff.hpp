// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace rivet::editor {

// Portable, pure text diff (ADR-0029): Myers' O(ND) algorithm over word
// tokens. No document, UI or platform types; safe to run on any thread.

// One word. `line` is the index of the text line the word sits on (equal for
// words of the same line, non-decreasing); it is only used by the per-line
// fallback.
struct DiffToken {
    std::string text;
    std::uint32_t line = 0;
};

// One maximal changed region: tokens a[aBegin, aEnd) of the old sequence were
// deleted and b[bBegin, bEnd) of the new sequence inserted (either range may
// be empty; never both).
struct DiffHunk {
    std::size_t aBegin = 0;
    std::size_t aEnd = 0;
    std::size_t bBegin = 0;
    std::size_t bEnd = 0;

    bool operator==(const DiffHunk&) const = default;
};

// Resource bounds. A page larger than maxTokens, or one whose word-level edit
// distance exceeds maxEditDistance (memory is O(D^2)), is diffed per line;
// when the line-level diff exceeds the bounds as well, the whole differing
// middle is reported as ONE replaced region.
struct DiffLimits {
    std::size_t maxTokens = 20000;
    std::size_t maxEditDistance = 3000;
};

enum class DiffGranularity : std::uint8_t { Word, Line, Coarse };

struct TextDiffResult {
    // Ascending, non-overlapping, in token indexes of the ORIGINAL sequences.
    std::vector<DiffHunk> hunks;
    DiffGranularity granularity = DiffGranularity::Word;
    // The callback reported cancellation: hunks are empty and meaningless.
    bool cancelled = false;
};

// Diffs `a` (old / left) against `b` (new / right). `cancelled` (optional) is
// polled periodically.
TextDiffResult diffTokens(std::span<const DiffToken> a, std::span<const DiffToken> b,
                          const DiffLimits& limits = {},
                          const std::function<bool()>& cancelled = {});

} // namespace rivet::editor
