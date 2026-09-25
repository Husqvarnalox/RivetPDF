// SPDX-License-Identifier: MPL-2.0
#include "ui/TabStrip.hpp"

#include "core/geometry/Insets.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Font kTabFont{13.0, Font::Weight::Regular};
constexpr Font kCloseFont{11.0, Font::Weight::Regular};

constexpr Color kStripBackground = Color::gray(0.949); // like Sidebar
constexpr Color kActiveTabFill = Color::white();
constexpr Color kInactiveTabFill = Color::gray(0.9);
constexpr Color kActiveTabBorder = Color::rgba(0.0, 0.0, 0.0, 0.25);
constexpr Color kInactiveTabBorder = Color::rgba(0.0, 0.0, 0.0, 0.12);
constexpr Color kTitleColor = Color::rgba(0.10, 0.10, 0.10, 1.0);
constexpr Color kCloseGlyphColor = Color::gray(0.45);

constexpr double kTabCornerRadius = 5.0;
constexpr double kStrokeWidth = 1.0;

// "×" multiplication sign (U+00D7), UTF-8.
constexpr const char* kCloseGlyph = "\xC3\x97";
// "…" horizontal ellipsis (U+2026), UTF-8.
constexpr const char* kEllipsis = "\xE2\x80\xA6";

bool isContinuationByte(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

// Byte offset of the first byte of the last code point of `s`.
std::size_t lastCodePointStart(const std::string& s) {
    if (s.empty()) return 0;
    std::size_t index = s.size() - 1;
    while (index > 0 && isContinuationByte(s[index])) --index;
    return index;
}

// Shortens `title` by dropping trailing code points (respecting UTF-8
// boundaries) and appending an ellipsis until it fits `available` points.
// Deterministic given the measureText of the context.
std::string ellipsizedTitle(const std::string& title, double available,
                            const PaintContext& context) {
    if (context.measureText(title, kTabFont).width <= available) return title;
    std::string shortened = title;
    while (!shortened.empty()) {
        shortened.resize(lastCodePointStart(shortened));
        if (context.measureText(shortened + kEllipsis, kTabFont).width <= available) {
            return shortened + kEllipsis;
        }
    }
    return kEllipsis; // degenerate: not even the ellipsis fits
}

} // namespace

void TabStrip::setTabs(std::vector<Tab> tabs, std::optional<std::size_t> activeIndex) {
    tabs_ = std::move(tabs);
    if (activeIndex && *activeIndex >= tabs_.size()) activeIndex.reset();
    activeIndex_ = activeIndex;
    // Widths may change with new titles: drop the layout cache until the
    // next paint re-measures.
    tabRects_.clear();
    visibleCount_ = 0;
    invalidate();
}

void TabStrip::setActiveIndex(std::optional<std::size_t> index) {
    if (index && *index >= tabs_.size()) index.reset();
    if (activeIndex_ == index) return;
    activeIndex_ = index;
    invalidate();
}

void TabStrip::setOnTabActivated(std::function<void(std::size_t)> onTabActivated) {
    onTabActivated_ = std::move(onTabActivated);
}

void TabStrip::setOnTabCloseRequested(std::function<void(std::size_t)> onTabCloseRequested) {
    onTabCloseRequested_ = std::move(onTabCloseRequested);
}

double TabStrip::tabWidthForTitle(const std::string& title, const PaintContext& context) const {
    const double natural =
        context.measureText(title, kTabFont).width + 2.0 * kTabLeftPadding + kCloseButtonSize;
    return std::clamp(natural, kMinTabWidth, kMaxTabWidth);
}

core::Size TabStrip::preferredSize(const PaintContext& context) const {
    double width = 0.0;
    for (const Tab& tab : tabs_) width += tabWidthForTitle(tab.title, context) + kTabGap;
    if (!tabs_.empty()) width -= kTabGap;
    return core::Size{width, kTabHeight};
}

void TabStrip::layoutTabs(const PaintContext& context) const {
    tabRects_.clear();
    visibleCount_ = 0;
    const double limitX = bounds().maxX();
    double x = 0.0;
    for (const Tab& tab : tabs_) {
        const double width = tabWidthForTitle(tab.title, context);
        tabRects_.push_back(core::Rect{core::Point{x, 0.0}, core::Size{width, kTabHeight}});
        // Tabs are laid out left to right with strictly increasing x, so once
        // one tab overflows, every later tab does too: the visible count only
        // grows.
        if (x + width <= limitX) visibleCount_ = tabRects_.size();
        x += width + kTabGap;
    }
}

std::string TabStrip::displayTitle(const std::string& title, double tabWidth,
                                   const PaintContext& context) const {
    const double available = tabWidth - 2.0 * kTabLeftPadding - kCloseButtonSize;
    return ellipsizedTitle(title, std::max(0.0, available), context);
}

std::optional<std::size_t> TabStrip::tabIndexAt(const core::Point& localPoint) const {
    if (tabRects_.empty()) return std::nullopt; // never painted
    for (std::size_t index = 0; index < visibleCount_; ++index) {
        if (tabRects_[index].contains(localPoint)) return index;
    }
    return std::nullopt;
}

core::Rect TabStrip::closeButtonRect(std::size_t index) const {
    if (index >= tabRects_.size()) return core::Rect{}; // not laid out yet
    const core::Rect tab = tabRects_[index];
    return core::Rect{
        core::Point{tab.maxX() - kTabLeftPadding - kCloseButtonSize,
                    tab.minY() + (kTabHeight - kCloseButtonSize) / 2.0},
        core::Size{kCloseButtonSize, kCloseButtonSize}};
}

bool TabStrip::onMouse(const PointerEvent& event) {
    switch (event.type) {
    case PointerEventType::Down: {
        const std::optional<std::size_t> index = tabIndexAt(event.position);
        if (!index) return false;
        if (closeButtonRect(*index).contains(event.position)) {
            pressedClose_ = index; // arm; fires on up inside
        } else if (onTabActivated_) {
            onTabActivated_(*index);
        }
        event.accepted = true;
        invalidate();
        return true;
    }

    case PointerEventType::Up: {
        if (!pressedClose_) return false;
        const std::size_t index = *pressedClose_;
        pressedClose_.reset();
        // Button-style: the close fires only when released inside the same
        // button (drag-out cancels); the press is still consumed.
        if (closeButtonRect(index).contains(event.position) && onTabCloseRequested_) {
            onTabCloseRequested_(index);
        }
        event.accepted = true;
        invalidate();
        return true;
    }

    case PointerEventType::Move:
    case PointerEventType::Entered:
    case PointerEventType::Exited:
    case PointerEventType::Scroll:
        // No hover visuals in Phase 2: moves are never consumed.
        return false;
    }
    return false;
}

void TabStrip::paintSelf(PaintContext& context) const {
    const core::Rect area = bounds();
    context.fillRect(area, kStripBackground);
    layoutTabs(context);

    context.pushClip(area);
    for (std::size_t index = 0; index < visibleCount_; ++index) {
        const bool active = activeIndex_ && *activeIndex_ == index;
        const core::Rect tab = tabRects_[index];
        context.fillRoundedRect(tab, active ? kActiveTabFill : kInactiveTabFill, kTabCornerRadius);
        context.strokeRect(tab.inset(core::Insets::uniform(0.5)),
                           active ? kActiveTabBorder : kInactiveTabBorder, kStrokeWidth);
        context.drawText(kCloseGlyph, closeButtonRect(index), kCloseFont, kCloseGlyphColor,
                         TextAlign::Center);
        const double titleWidth = tab.size.width - 2.0 * kTabLeftPadding - kCloseButtonSize;
        const core::Rect titleRect{
            core::Point{tab.minX() + kTabLeftPadding, tab.minY()},
            core::Size{std::max(0.0, titleWidth), kTabHeight}};
        context.drawText(displayTitle(tabs_[index].title, tab.size.width, context), titleRect,
                         kTabFont, kTitleColor, TextAlign::Left);
    }
    context.popClip();
}

} // namespace rivet::ui
