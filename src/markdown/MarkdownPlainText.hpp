// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "markdown/MarkdownModel.hpp"

#include <string>
#include <vector>

namespace rivet::markdown {

// A piece of extracted plain text and the source range it came from.
struct PlainTextSpan {
    std::size_t textStart = 0; // byte offsets into PlainText::text
    std::size_t textEnd = 0;
    SourceRange source;        // range of the originating inline/block (approximate, see parser docs)
};

struct PlainText {
    std::string text;
    std::vector<PlainTextSpan> spans; // sorted by textStart; empty when not requested
};

// Plain text of the whole document for search: blocks are separated by '\n',
// table cells by '\t', soft breaks become ' ', hard breaks '\n'. Code and
// HTML text is included; link destinations are not.
PlainText extractPlainText(const MarkdownDocument& doc, bool withSpans = true);

// Plain text of a run of inlines (no separators added between blocks).
std::string inlinesToPlainText(const std::vector<Inline>& inlines);

// Source range of the plain-text byte range [textStart, textEnd): the union
// of spans overlapping it. Empty range when nothing overlaps.
SourceRange sourceRangeForText(const PlainText& plain, std::size_t textStart, std::size_t textEnd);

} // namespace rivet::markdown
