// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "markdown/MarkdownModel.hpp"

#include <cstddef>
#include <memory>
#include <string_view>

namespace rivet::markdown {

// Resource limits applied to untrusted Markdown. Exceeding a limit never
// fails the parse: the document degrades gracefully and a Diagnostic says how.
struct ParseLimits {
    // Source longer than this is truncated (at a UTF-8 boundary). Hard-capped
    // at 2 GiB regardless of the value given.
    std::size_t maxSourceBytes = 64u * 1024u * 1024u;
    // Container nesting (quotes + lists). Deeper containers are flattened into
    // the deepest allowed one; their leaf blocks are kept.
    std::size_t maxNestingDepth = 32;
    // Inline nesting (emphasis/link/...). Deeper spans are flattened.
    std::size_t maxInlineDepth = 32;
    // Total blocks (including list items and table rows). Parsing stops at the limit.
    std::size_t maxBlocks = 1'000'000;
    // Per table: cells beyond this are dropped (rows are dropped whole).
    std::size_t maxTableCells = 200'000;
    // Per table: columns beyond this are dropped.
    std::size_t maxTableColumns = 256;
    // Per code block / HTML block: text beyond this is cut.
    std::size_t maxCodeBlockBytes = 4u * 1024u * 1024u;
};

// Parsing is a pure function of the immutable source snapshot: implementations
// are re-entrant and thread-safe, keep no state between calls, never throw,
// and never retain `source` after returning (the result owns its strings).
//
// Source ranges in the result are byte offsets into `source` with a leading
// UTF-8 BOM removed (MarkdownDocument::sourceOffsetBase says how much), so
// callers should keep the same snapshot (or re-add the base) to slice it.
class IMarkdownParser {
public:
    virtual ~IMarkdownParser() = default;
    virtual MarkdownDocument parse(std::string_view source, const ParseLimits& limits = {}) const noexcept = 0;
};

// The default parser (CommonMark + GFM tables, task lists, strikethrough and
// permissive autolinks; raw HTML is kept as plain text, never interpreted).
std::unique_ptr<IMarkdownParser> makeMarkdownParser();

} // namespace rivet::markdown
