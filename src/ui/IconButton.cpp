// SPDX-License-Identifier: MPL-2.0
#include "ui/IconButton.hpp"

#include "ui/Drawing.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {
namespace {

constexpr double kTextGap = 6.0;
constexpr double kTextPaddingRight = 10.0;

} // namespace

IconButton::IconButton(Icon icon, std::string tooltip) : icon_(icon) { setTooltip(std::move(tooltip)); }

void IconButton::setIcon(Icon icon) {
    if (icon_ == icon) return;
    icon_ = icon;
    invalidate();
}

void IconButton::setText(std::string text) {
    if (text_ == text) return;
    text_ = std::move(text);
    invalidate();
}

void IconButton::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    invalidate();
}

void IconButton::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) {
        armed_ = false;
        hoverFade_.reset(0.0);
    }
    invalidate();
}

void IconButton::activate() {
    if (enabled_ && onClick_) onClick_();
}

core::Size IconButton::preferredSize(const PaintContext& context) const {
    if (text_.empty()) return core::Size{kSize, kSize};
    const double textWidth = context.measureText(text_, fonts::kBody).width;
    return core::Size{kSize + kTextGap / 2.0 + textWidth + kTextPaddingRight, kSize};
}

bool IconButton::onMouse(const PointerEvent& event) {
    const bool inside = bounds().contains(event.position);
    switch (event.type) {
    case PointerEventType::Down:
        if (!inside) return false;
        armed_ = enabled_;
        hovered_ = true;
        break;
    case PointerEventType::Up: {
        const bool wasArmed = armed_;
        armed_ = false;
        hovered_ = inside;
        if (wasArmed) {
            if (inside) activate();
            event.accepted = true;
            invalidate();
            return true;
        }
        if (!inside) return false;
        break;
    }
    case PointerEventType::Move:
    case PointerEventType::Entered:
    case PointerEventType::Exited: {
        if (armed_ && event.button == 0) armed_ = false;
        const bool wasHovered = hovered_;
        hovered_ = inside && event.type != PointerEventType::Exited;
        if (!armed_ && !hovered_ && !wasHovered) return false;
        break;
    }
    case PointerEventType::Scroll: return false;
    }
    hoverFade_.setTarget(hovered_ && enabled_ ? 1.0 : 0.0);
    event.accepted = true;
    invalidate();
    return true;
}

void IconButton::paintSelf(PaintContext& context) const {
    const Palette& palette = colors();
    const core::Rect rect = bounds();
    if (active_) context.fillRoundedRect(rect, palette.accentSubtle, kCornerRadius);
    if (enabled_) {
        if (pressed()) {
            context.fillRoundedRect(rect, palette.controlFillPressed, kCornerRadius);
        } else if (const double hover = hoverFade_.value(); hover > 0.0) {
            const Color fill = palette.controlFillHover;
            context.fillRoundedRect(rect, withAlpha(fill, fill.a * hover), kCornerRadius);
        }
        if (hoverFade_.running()) invalidate(); // next fade frame
    }
    const Color color = !enabled_ ? palette.textDisabled : (active_ ? palette.accentText : palette.textPrimary);
    const double iconX = text_.empty() ? (rect.size.width - kIconSide) / 2.0 : (kSize - kIconSide) / 2.0;
    drawIcon(context, icon_, core::Rect{iconX, (rect.size.height - kIconSide) / 2.0, kIconSide, kIconSide}, color);
    if (!text_.empty()) {
        const double x = kSize - kTextGap / 2.0;
        context.drawText(text_, core::Rect{x, 0.0, std::max(0.0, rect.size.width - x), rect.size.height}, fonts::kBody,
                         color, TextAlign::Left);
    }
    if (isFocused()) drawFocusRing(context, rect, kCornerRadius);
}

} // namespace rivet::ui
