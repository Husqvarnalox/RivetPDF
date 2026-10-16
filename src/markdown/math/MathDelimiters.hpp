// SPDX-License-Identifier: MPL-2.0
#pragma once

// Where math starts and ends in Markdown text (ADR-0032).
//
// Display math is `$$...$$`. Inline math is `$...$` unless it looks like
// money (the Pandoc rule): the opening `$` must be followed by a non-space
// character, the closing `$` must be preceded by a non-space character and
// must not be followed by a digit. So "$5 and $10" and "costs $ 3" are text,
// "$x$" and "$1+1$" are math. A backslash escapes a dollar sign. Inline math
// never spans a blank line. The first `$` after an opener decides: when it
// cannot close, the opener is plain text.
//
// Pure functions on UTF-8 byte offsets.

#include <cstddef>
#include <string_view>
#include <vector>

namespace rivet::markdown::math {

// Whether the '$' at `open` and the '$' at `close` (byte offsets into
// `text`, open < close) delimit inline math under the rules above.
bool isInlineMathSpan(std::string_view text, std::size_t open, std::size_t close);

struct MathSpan {
    std::size_t begin = 0; // offset of the opening '$' (or "$$")
    std::size_t end = 0;   // one past the closing '$' (or "$$")
    bool display = false;

    // The TeX source between the delimiters.
    std::string_view content(std::string_view text) const {
        const std::size_t d = display ? 2 : 1;
        return text.substr(begin + d, end - begin - 2 * d);
    }
};

// All math spans of `text`, left to right, non-overlapping.
std::vector<MathSpan> findMathSpans(std::string_view text);

} // namespace rivet::markdown::math
