// SPDX-License-Identifier: MPL-2.0
#include "markdown/MarkdownSourceMap.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::markdown {

namespace {

bool interpolates(LayoutBlockKind kind) {
    return kind == LayoutBlockKind::Paragraph || kind == LayoutBlockKind::Code || kind == LayoutBlockKind::Html ||
           kind == LayoutBlockKind::Table;
}

} // namespace

double previewYForSourceOffset(const MarkdownLayout& layout, std::size_t offset) {
    if (layout.blocks.empty()) return 0.0;
    // The very start of the document shows the top padding, not the first block's edge.
    if (offset <= layout.blocks.front().source.start) return 0.0;
    const std::size_t index = layout.blockIndexAtSource(offset);
    if (index >= layout.blocks.size()) return 0.0;
    const LayoutBlock& block = layout.blocks[index];
    if (offset < block.source.start) return block.rect.origin.y;
    if (!interpolates(block.kind) || block.source.empty()) return block.rect.origin.y;
    const double fraction = std::clamp(static_cast<double>(offset - block.source.start) /
                                           static_cast<double>(block.source.size()),
                                       0.0, 1.0);
    return block.rect.origin.y + fraction * block.rect.size.height;
}

std::size_t sourceOffsetForPreviewY(const MarkdownLayout& layout, double y) {
    if (layout.blocks.empty() || !std::isfinite(y)) return 0;
    const std::size_t index = layout.blockIndexAtY(std::max(y, 0.0));
    if (index >= layout.blocks.size()) return 0;
    const LayoutBlock& block = layout.blocks[index];
    if (!interpolates(block.kind) || block.source.empty() || block.rect.size.height <= 0.0) {
        return block.source.start;
    }
    const double fraction = std::clamp((y - block.rect.origin.y) / block.rect.size.height, 0.0, 1.0);
    const std::size_t inside = static_cast<std::size_t>(fraction * static_cast<double>(block.source.size()));
    return block.source.start + std::min(inside, block.source.size() - 1);
}

} // namespace rivet::markdown
