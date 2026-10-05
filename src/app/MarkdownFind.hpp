// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "markdown/MarkdownLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rivet::app {

// One occurrence of the query, as a range of layout text positions.
struct FindMatch {
    markdown::TextPosition begin;
    markdown::TextPosition end;
    std::uint32_t block = 0; // index into MarkdownLayout::blocks
};

// Plain-text find over the laid out document (the text the user sees: no
// markup, soft wraps count as one space, table cells are tab separated).
// Case-insensitive for ASCII; other characters match exactly (a valid UTF-8
// query only ever matches on code point boundaries). A match never spans two
// blocks. Results are in document order and capped at `maxMatches`.
// O(N) in the text size; pure.
std::vector<FindMatch> findInLayout(const markdown::MarkdownLayout& layout, std::string_view query,
                                    std::size_t maxMatches = 100'000);

// One occurrence of the query in plain text: byte range [begin, end).
struct TextMatch {
    std::size_t begin = 0;
    std::size_t end = 0;
};

// Plain-text find in raw text (the Markdown source): ASCII case-insensitive,
// other bytes exact, non-overlapping, in order, capped at `maxMatches`. An
// invalid-UTF-8 query matches nothing (so a match never splits a code point
// of valid UTF-8 text). O(N).
std::vector<TextMatch> findInText(std::string_view text, std::string_view query, std::size_t maxMatches = 100'000);

} // namespace rivet::app
