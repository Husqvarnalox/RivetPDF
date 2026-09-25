// SPDX-License-Identifier: MPL-2.0
#include "ui/OutlinePanel.hpp"

#include "core/geometry/Insets.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Color kBackgroundColor = Color::gray(0.949);
constexpr Color kRowTextColor = Color::rgba(0.10, 0.10, 0.10, 1.0);
constexpr Color kRowHoverText = Color::rgba(0.0, 0.0, 0.0, 1.0);
constexpr Color kSelectionFill = Color::rgba(0.0, 0.0, 0.0, 0.12);
constexpr Color kMarkerGlyphColor = Color::gray(0.35);
constexpr Font kRowFont{13.0, Font::Weight::Regular};
constexpr Font kMarkerFont{11.0, Font::Weight::Regular};
constexpr const char* kEmptyStateText = "No outline";
constexpr Font kEmptyStateFont{13.0, Font::Weight::Regular};
// Disclosure glyphs (Unicode triangles, UTF-8).
constexpr const char* kExpandedGlyph = "\xE2\x96\xBE"; // ▾
constexpr const char* kCollapsedGlyph = "\xE2\x96\xB8"; // ▸

} // namespace

OutlinePanel::OutlinePanel() {
    auto bar = std::make_unique<ScrollBar>(ScrollOrientation::Vertical);
    scrollBar_ = bar.get();
    addChild(std::move(bar));
    scrollBar_->setOnScroll([this](double offset) { setScrollOffset(offset); });
}

OutlinePanel::~OutlinePanel() = default;

void OutlinePanel::setRows(std::vector<OutlineRow> rows) {
    rows_ = std::move(rows);
    if (selectedIndex_ && *selectedIndex_ >= rows_.size()) selectedIndex_.reset();
    syncScrollbar();
    invalidate();
}

void OutlinePanel::setSelectedIndex(std::optional<std::size_t> index) {
    if (index && *index >= rows_.size()) index.reset();
    if (selectedIndex_ == index) return;
    selectedIndex_ = index;
    invalidate();
}

void OutlinePanel::setOnRowActivated(std::function<void(std::size_t)> onRowActivated) {
    onRowActivated_ = std::move(onRowActivated);
}

void OutlinePanel::setOnExpansionToggled(std::function<void(std::size_t, bool)> onExpansionToggled) {
    onExpansionToggled_ = std::move(onExpansionToggled);
}

double OutlinePanel::contentHeight() const {
    return static_cast<double>(rows_.size()) * kRowHeight;
}

core::Rect OutlinePanel::rowRect(std::size_t index) const {
    return core::Rect{0.0, static_cast<double>(index) * kRowHeight,
                      std::max(0.0, bounds().size.width), kRowHeight};
}

core::Rect OutlinePanel::markerRect(std::size_t index) const {
    const int depth = rows_[index].depth;
    const double x = kPadding + static_cast<double>(depth) * kIndent;
    return core::Rect{x, static_cast<double>(index) * kRowHeight +
                             (kRowHeight - kMarkerWidth) / 2.0,
                      kMarkerWidth, kMarkerWidth};
}

std::pair<std::size_t, std::size_t> OutlinePanel::visibleRowRange() const {
    const std::size_t count = rows_.size();
    if (count == 0) return {0, 0};
    const double viewTop = scrollOffset_;
    const double viewBottom = scrollOffset_ + bounds().size.height;
    // Uniform rows: direct index math, clamped.
    std::size_t first = viewTop > 0.0 ? static_cast<std::size_t>(std::floor(viewTop / kRowHeight)) : 0;
    std::size_t last =
        viewBottom > 0.0 ? static_cast<std::size_t>(std::ceil(viewBottom / kRowHeight)) : 0;
    first = std::min(first, count - 1);
    last = std::min(last, count == 0 ? 0 : count - 1);
    if (last < first) last = first;
    return {first, last};
}

std::optional<std::size_t> OutlinePanel::rowIndexAt(const core::Point& localPoint) const {
    if (rows_.empty()) return std::nullopt;
    const double y = localPoint.y + scrollOffset_;
    if (y < 0.0) return std::nullopt;
    const auto index = static_cast<std::size_t>(std::floor(y / kRowHeight));
    if (index >= rows_.size()) return std::nullopt;
    return index;
}

void OutlinePanel::revealRow(std::size_t index) {
    if (index >= rows_.size()) return;
    const double top = static_cast<double>(index) * kRowHeight;
    const double bottom = top + kRowHeight;
    if (top < scrollOffset_) {
        setScrollOffset(top);
    } else if (bottom > scrollOffset_ + bounds().size.height) {
        setScrollOffset(bottom - bounds().size.height);
    }
}

void OutlinePanel::setScrollOffset(double offset) {
    const double maxOffset = std::max(0.0, contentHeight() - bounds().size.height);
    const double clamped = std::isfinite(offset) ? std::clamp(offset, 0.0, maxOffset) : 0.0;
    if (clamped == scrollOffset_) return;
    scrollOffset_ = clamped;
    scrollBar_->setOffset(scrollOffset_);
    invalidate();
}

void OutlinePanel::syncScrollbar() {
    scrollBar_->setExtents(bounds().size.height, contentHeight());
    scrollBar_->setOffset(scrollOffset_);
}

void OutlinePanel::positionScrollbar() {
    const core::Rect area = bounds();
    const double thickness = ScrollBar::kThickness;
    scrollBar_->setFrame(core::Rect{area.maxX() - thickness - 2.0, 2.0, thickness,
                                    std::max(0.0, area.size.height - 4.0)});
}

void OutlinePanel::layout() {
    positionScrollbar();
    syncScrollbar();
}

bool OutlinePanel::onMouse(const PointerEvent& event) {
    if (event.type == PointerEventType::Scroll) {
        setScrollOffset(scrollOffset_ + event.scrollDelta.y);
        event.accepted = true;
        return true;
    }
    if (event.type == PointerEventType::Down) {
        const core::Point contentPoint{event.position.x, event.position.y + scrollOffset_};
        const std::optional<std::size_t> index = rowIndexAt(contentPoint);
        if (!index.has_value()) return false;
        event.accepted = true;

        // A click on the disclosure marker toggles expansion; the owner owns
        // the expansion state and rebuilds the rows.
        if (rows_[*index].hasChildren && markerRect(*index).contains(contentPoint)) {
            if (onExpansionToggled_) onExpansionToggled_(*index, !rows_[*index].expanded);
            return true;
        }
        if (selectedIndex_ != index) {
            selectedIndex_ = index;
            invalidate();
        }
        if (onRowActivated_) onRowActivated_(*index);
        return true;
    }
    return Widget::onMouse(event);
}

void OutlinePanel::paintSelf(PaintContext& context) const {
    context.fillRect(bounds(), kBackgroundColor);
    context.pushClip(bounds());

    if (rows_.empty()) {
        context.drawText(kEmptyStateText, bounds(), kEmptyStateFont, Color::gray(0.45),
                         TextAlign::Left);
        context.popClip();
        return;
    }

    const core::Point scrollShift{0.0, -scrollOffset_};
    const auto [first, last] = visibleRowRange();
    for (std::size_t i = first; i <= last; ++i) {
        const OutlineRow& rowData = rows_[i];
        const core::Rect row = rowRect(i).translated(scrollShift);
        if (selectedIndex_ && *selectedIndex_ == i) {
            context.fillRoundedRect(row.inset(core::Insets::horizontal(3.0)), kSelectionFill, 5.0);
        }

        // Disclosure marker (children only).
        if (rowData.hasChildren) {
            context.drawText(rowData.expanded ? kExpandedGlyph : kCollapsedGlyph,
                             markerRect(i).translated(scrollShift), kMarkerFont,
                             kMarkerGlyphColor, TextAlign::Center);
        }

        // Title, indented past the marker.
        const double textX =
            kPadding + static_cast<double>(rowData.depth) * kIndent + kMarkerWidth + 4.0;
        context.drawText(rowData.title,
                         core::Rect{textX, row.minY(),
                                    std::max(0.0, bounds().size.width - textX), kRowHeight},
                         kRowFont, (selectedIndex_ && *selectedIndex_ == i) ? kRowHoverText
                                                                            : kRowTextColor,
                         TextAlign::Left);
    }
    context.popClip();
}

} // namespace rivet::ui
