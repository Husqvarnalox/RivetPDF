// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "markdown/MarkdownParser.hpp"

namespace rivet::markdown {

// IMarkdownParser backed by MD4C. This header deliberately exposes no MD4C
// types; md4c.h is included only by Md4cMarkdownParser.cpp.
//
// Source-range accuracy (MD4C reports no offsets; ranges are derived from the
// pointers MD4C passes to its text callbacks plus line scanning):
//  - Block ranges cover whole lines, [start of first line, end of last line)
//    excluding the line terminator. They include container prefixes ("> ",
//    "- ") of the first line, fence lines of fenced code, and setext
//    underlines. Blocks without text (rules, empty headings/items) are found
//    by scanning to the next non-blank line after the previous block.
//  - Inline ranges include their delimiters (*, **, ~~, `, [..](..), <..>)
//    when those can be located; when delimiters cannot be verified the range
//    shrinks to the covered text. Text inlines that span an escape or entity
//    cover the whole run; byte offsets inside such a text are not 1:1.
//  - Table cell ranges cover the cell text only (no pipes).
//  - A fenced block whose closing fence cannot be identified ends at its last
//    text line.
//  - All ranges are clamped to the source, sibling ranges are ordered.
class Md4cMarkdownParser final : public IMarkdownParser {
public:
    MarkdownDocument parse(std::string_view source, const ParseLimits& limits = {}) const noexcept override;
};

} // namespace rivet::markdown
