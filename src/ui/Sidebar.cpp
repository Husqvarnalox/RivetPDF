#include "ui/Sidebar.hpp"

#include "core/geometry/Insets.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Font kRowFont{13.0, Font::Weight::Regular};
constexpr Color kBackgroundColor = Color::gray(0.949); // ~#F2F2F2
constexpr Color kRowTextColor = Color::rgba(0.10, 0.10, 0.10, 1.0);
constexpr Color kSelectionFill = Color::rgba(0.0, 0.0, 0.0, 0.12);

} // namespace

void Sidebar::setItems(std::vector<std::string> items) {
    items_ = std::move(items);
    if (selectedIndex_ && *selectedIndex_ >= items_.size()) selectedIndex_.reset();
    invalidate();
}

void Sidebar::setSelectedIndex(std::optional<std::size_t> index) {
    if (index && *index >= items_.size()) index.reset();
    if (selectedIndex_ == index) return;
    selectedIndex_ = index;
    invalidate();
}

void Sidebar::setOnSelectionChanged(std::function<void(std::size_t)> onSelectionChanged) {
    onSelectionChanged_ = std::move(onSelectionChanged);
}

std::optional<std::size_t> Sidebar::rowIndexAt(const core::Point& localPoint) const {
    if (items_.empty()) return std::nullopt;
    const core::Rect area = bounds();
    if (localPoint.x < area.minX() || localPoint.x > area.maxX()) return std::nullopt;
    const double relative = localPoint.y - kPadding;
    if (relative < 0.0) return std::nullopt;
    const auto index = static_cast<std::size_t>(relative / kRowHeight);
    if (index >= items_.size()) return std::nullopt;
    return index;
}

bool Sidebar::onMouse(const PointerEvent& event) {
    if (event.type != PointerEventType::Down) return false;
    const std::optional<std::size_t> index = rowIndexAt(event.position);
    if (!index) return false;
    if (selectedIndex_ != index) {
        selectedIndex_ = index;
        invalidate();
        if (onSelectionChanged_) onSelectionChanged_(*index);
    }
    event.accepted = true;
    return true;
}

core::Rect Sidebar::rowRect(std::size_t index) const {
    const double width = std::max(0.0, frame().size.width - 2.0 * kPadding);
    return core::Rect{kPadding, kPadding + static_cast<double>(index) * kRowHeight, width, kRowHeight};
}

void Sidebar::paintSelf(PaintContext& context) const {
    context.fillRect(bounds(), kBackgroundColor);
    context.pushClip(bounds());
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const core::Rect row = rowRect(i);
        if (row.minY() > bounds().maxY()) break; // below the visible area
        if (selectedIndex_ && *selectedIndex_ == i) {
            context.fillRoundedRect(row, kSelectionFill, kRowCornerRadius);
        }
        context.drawText(items_[i], row.inset(core::Insets::horizontal(kRowTextInset)), kRowFont,
                         kRowTextColor, TextAlign::Left);
    }
    context.popClip();
}

} // namespace rivet::ui
