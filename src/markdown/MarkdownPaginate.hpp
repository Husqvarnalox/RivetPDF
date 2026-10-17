// SPDX-License-Identifier: MPL-2.0
#pragma once

// Pagination of a MarkdownLayout into page-sized vertical slices (ADR-0023).
// Pure geometry over the layout's document space (y grows downward); no
// fonts, no PDF.
//
// "Atoms" are the parts of a layout that must never be cut by a page break:
//   - every LayoutLine [top, top + height) - paragraph, heading, list, quote
//     and code lines as well as the lines of table cells;
//   - every Image block's rect;
//   - every decoration that is not a background: Rule, ListBullet,
//     ListNumber, TaskCheckbox and ImagePlaceholder.
// Backgrounds may be cut (each page draws its clipped part): CodeBackground
// and QuoteBar, plus table cell rectangles (borders / header fills), which
// are not decorations and are therefore never atoms themselves.
//
// A cut y is valid iff no atom has top < y < bottom (an atom ending exactly
// at y, or starting exactly at y, does not prevent the cut).

#include "markdown/MarkdownLayout.hpp"

#include <vector>

namespace rivet::markdown {

// The layout y-range placed on one page; `bottom` is exclusive.
struct PageSlice {
    double top = 0.0;
    double bottom = 0.0;
};

// Splits [0, layout.contentHeight) into consecutive slices:
//   - greedy: from the current top `cur`, the slice ends at the largest valid
//     cut in (cur, cur + pageHeight]; when there is none (an atom taller than
//     what remains of the page straddles every candidate, i.e. an atom taller
//     than the page), it ends at cur + pageHeight, splitting that atom at the
//     page boundary;
//   - every slice is at most pageHeight tall (up to a 1e-6 tolerance that
//     absorbs floating-point residue at the very end);
//   - the last slice ends at contentHeight: never an empty trailing page.
// An empty layout (contentHeight <= 0) gives one slice {0, 0};
// pageHeight <= 0 (or not finite) gives one slice for everything.
std::vector<PageSlice> paginateLayout(const MarkdownLayout& layout, double pageHeight);

} // namespace rivet::markdown
