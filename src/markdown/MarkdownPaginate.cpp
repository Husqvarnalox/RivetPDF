// SPDX-License-Identifier: MPL-2.0

#include "markdown/MarkdownPaginate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace rivet::markdown {

namespace {

// Floating-point residue below this never starts another page.
constexpr double kEpsilon = 1.0e-6;

struct Span {
    double top = 0.0;
    double bottom = 0.0;
};

bool isBackground(DecorationKind kind) {
    switch (kind) {
    case DecorationKind::CodeBackground:
    case DecorationKind::QuoteBar:
        return true;
    case DecorationKind::ListBullet:
    case DecorationKind::ListNumber:
    case DecorationKind::TaskCheckbox:
    case DecorationKind::Rule:
    case DecorationKind::ImagePlaceholder:
        return false;
    }
    return false;
}

// The atoms as disjoint OPEN intervals (top, bottom), sorted: overlapping
// atoms merge (a cut inside the union would be inside one of them); atoms
// that merely touch stay separate (the shared edge is a valid cut).
std::vector<Span> forbiddenSpans(const MarkdownLayout& layout) {
    std::vector<Span> atoms;
    atoms.reserve(layout.lines.size() + layout.decorations.size());
    const auto add = [&](double top, double height) {
        if (std::isfinite(top) && std::isfinite(height) && height > 0.0) atoms.push_back({top, top + height});
    };
    for (const LayoutLine& line : layout.lines) add(line.top, line.height);
    for (const LayoutBlock& block : layout.blocks) {
        if (block.kind == LayoutBlockKind::Image) add(block.rect.origin.y, block.rect.size.height);
    }
    for (const Decoration& decoration : layout.decorations) {
        if (!isBackground(decoration.kind)) add(decoration.rect.origin.y, decoration.rect.size.height);
    }
    std::sort(atoms.begin(), atoms.end(), [](const Span& a, const Span& b) { return a.top < b.top; });
    std::vector<Span> merged;
    for (const Span& atom : atoms) {
        if (!merged.empty() && atom.top < merged.back().bottom) {
            merged.back().bottom = std::max(merged.back().bottom, atom.bottom);
        } else {
            merged.push_back(atom);
        }
    }
    return merged;
}

} // namespace

std::vector<PageSlice> paginateLayout(const MarkdownLayout& layout, double pageHeight) {
    const double total = std::isfinite(layout.contentHeight) ? std::max(0.0, layout.contentHeight) : 0.0;
    if (total <= 0.0) return {PageSlice{0.0, 0.0}};
    if (!std::isfinite(pageHeight) || pageHeight <= 0.0) return {PageSlice{0.0, total}};

    const std::vector<Span> spans = forbiddenSpans(layout);
    std::vector<PageSlice> slices;
    double cur = 0.0;
    while (true) {
        const double limit = cur + pageHeight;
        if (limit >= total - kEpsilon) {
            slices.push_back({cur, total});
            break;
        }
        // The merged span strictly containing `limit`, if any: the first span
        // whose bottom is > limit, when its top is < limit.
        const auto it = std::upper_bound(spans.begin(), spans.end(), limit,
                                         [](double value, const Span& span) { return value < span.bottom; });
        double cut = limit;
        if (it != spans.end() && it->top < limit) {
            // The largest valid cut <= limit is the span's top, if it lies
            // beyond `cur`; otherwise no valid cut exists in (cur, limit].
            cut = it->top > cur + kEpsilon ? it->top : limit;
        }
        slices.push_back({cur, cut});
        cur = cut;
    }
    return slices;
}

} // namespace rivet::markdown
