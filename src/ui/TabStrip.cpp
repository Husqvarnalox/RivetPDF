// SPDX-License-Identifier: MPL-2.0
#include "ui/TabStrip.hpp"
#include "ui/Theme.hpp"

#include "core/geometry/Insets.hpp"
#include "ui/Drawing.hpp"
#include "ui/Icons.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Font kTabFont{13.0, Font::Weight::Regular};


constexpr double kDirtyDotSize = 6.0;
constexpr double kTabCornerRadius = 7.0;

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
    double x = kLeadingInset;
    for (const Tab& tab : tabs_) {
        const double width = tabWidthForTitle(tab.title, context);
        tabRects_.push_back(core::Rect{core::Point{x, kTabTopInset}, core::Size{width, kTabHeight - kTabTopInset}});
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
                    tab.minY() + (tab.size.height - kCloseButtonSize) / 2.0},
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
    case PointerEventType::Exited: {
        // Hover shows the close button; moves are never consumed.
        const std::optional<std::size_t> hovered =
            event.type == PointerEventType::Exited ? std::nullopt : tabIndexAt(event.position);
        const bool overClose = hovered.has_value() && closeButtonRect(*hovered).contains(event.position);
        if (hovered != hoveredTab_ || overClose != hoveredClose_) {
            hoveredTab_ = hovered;
            hoveredClose_ = overClose;
            invalidate();
        }
        return false;
    }
    case PointerEventType::Scroll:
        return false;
    }
    return false;
}

void TabStrip::paintSelf(PaintContext& context) const {
    const Palette& palette = colors();
    const core::Rect area = bounds();
    context.fillRect(area, palette.tabStripBackground);
    layoutTabs(context);
    // Bottom hairline, interrupted under the active tab (it joins the toolbar).
    drawBottomHairline(context, area, palette.chromeSeparator);

    context.pushClip(area);
    for (std::size_t index = 0; index < visibleCount_; ++index) {
        const bool active = activeIndex_ && *activeIndex_ == index;
        const bool hovered = hoveredTab_ && *hoveredTab_ == index;
        const core::Rect tab = tabRects_[index];
        if (active) {
            // Rounded top corners; the bottom runs into the toolbar.
            const core::Rect body{tab.minX(), tab.minY(), tab.size.width, tab.size.height + kTabCornerRadius + 2.0};
            strokeRoundedRect(context, body.inset(core::Insets::uniform(-0.5)), palette.chromeSeparator, 1.0,
                              kTabCornerRadius + 0.5);
            context.fillRoundedRect(body, palette.chromeActiveTab, kTabCornerRadius);
        } else if (hovered) {
            context.fillRoundedRect(tab.inset(core::Insets{2.0, 0.0, 2.0, 4.0}), palette.controlFillHover,
                                    kTabCornerRadius);
        }
        const core::Rect close = closeButtonRect(index);
        const bool dirty = tabs_[index].dirty;
        const bool showClose = hovered || (active && !dirty);
        if (showClose) {
            if (hovered && hoveredClose_) {
                context.fillRoundedRect(close, palette.controlFillPressed, kCloseButtonSize / 2.0);
            }
            drawIcon(context, Icon::Close, close.inset(core::Insets::uniform(3.0)), palette.textSecondary);
        } else if (dirty) {
            // The dirty dot sits where the close button appears on hover.
            context.fillRoundedRect(
                core::Rect{close.center().x - kDirtyDotSize / 2.0, close.center().y - kDirtyDotSize / 2.0, kDirtyDotSize,
                           kDirtyDotSize},
                palette.textSecondary, kDirtyDotSize / 2.0);
        }
        const double titleWidth = tab.size.width - 2.0 * kTabLeftPadding - kCloseButtonSize;
        const core::Rect titleRect{core::Point{tab.minX() + kTabLeftPadding, tab.minY()},
                                   core::Size{std::max(0.0, titleWidth), tab.size.height}};
        context.drawText(displayTitle(tabs_[index].title, tab.size.width, context), titleRect, kTabFont,
                         active ? palette.textPrimary : palette.textSecondary, TextAlign::Left);
    }
    context.popClip();
}

} // namespace rivet::ui
