// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "markdown/MarkdownLayout.hpp"

#include <cstddef>

namespace rivet::markdown {

// Position mapping between the Markdown source and its rendered layout, for
// scroll synchronisation. Pure, O(log N).
//
// Granularity is the layout block (paragraph, heading, code block, table...),
// refined by a linear interpolation over the block's source bytes for tall
// blocks, so a long code block scrolls smoothly instead of jumping. Both
// functions are monotonic non-decreasing and clamp to the document.

// Document-space y in `layout` that corresponds to the source byte `offset`
// (offsets past a block map to the block's bottom edge; before the first
// block to 0). Returns 0 for an empty layout.
double previewYForSourceOffset(const MarkdownLayout& layout, std::size_t offset);

// Source byte offset of the content at document-space y (clamped). Returns 0
// for an empty layout. The result lies inside the block found at y.
std::size_t sourceOffsetForPreviewY(const MarkdownLayout& layout, double y);

} // namespace rivet::markdown
