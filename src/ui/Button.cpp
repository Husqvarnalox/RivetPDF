#include "ui/Button.hpp"

#include "core/geometry/Insets.hpp"
#include "ui/Drawing.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {

Button::Button(std::string label) : label_(std::move(label)), font_(fonts::kBody) {}

void Button::setLabel(std::string label) {
    if (label_ == label) return;
    label_ = std::move(label);
    invalidate();
}

void Button::setOnClick(std::function<void()> onClick) {
    onClick_ = std::move(onClick);
}

void Button::setFont(const Font& font) {
    if (font_ == font) return;
    font_ = font;
    invalidate();
}

core::Size Button::preferredSize(const PaintContext& context) const {
    const core::Size textSize = context.measureText(label_, font_);
    return core::Size{textSize.width + 2.0 * kHorizontalPadding,
                      std::max(textSize.height + 2.0 * kVerticalPadding, kMinHeight)};
}

Button::VisualState Button::visualState() const {
    if (!enabled_) return VisualState::Normal;
    if (armed_ && hovered_) return VisualState::Pressed;
    if (armed_ || hovered_) return VisualState::Hovered;
    return VisualState::Normal;
}

void Button::fireClick() {
    if (enabled_ && onClick_) onClick_();
}

void Button::activate() { fireClick(); }

bool Button::onMouse(const PointerEvent& event) {
    const bool inside = bounds().contains(event.position);
    switch (event.type) {
    case PointerEventType::Down:
        if (!inside) return false;
        armed_ = true;
        hovered_ = true;
        break;

    case PointerEventType::Up: {
        const bool wasArmed = armed_;
        armed_ = false;
        hovered_ = inside;
        if (wasArmed) {
            if (inside) fireClick();
            event.accepted = true;
            invalidate();
            return true;
        }
        // An unannounced up over the button is still ours to swallow.
        if (!inside) return false;
        break;
    }

    case PointerEventType::Move:
    case PointerEventType::Entered:
    case PointerEventType::Exited: {
        // A move while armed must report the held button (see PointerEvent).
        // Button-less moves mean the matching up was lost (e.g. released
        // elsewhere), so the press is disarmed defensively.
        if (armed_ && event.button == 0) armed_ = false;
        const bool wasHovered = hovered_;
        hovered_ = inside && event.type != PointerEventType::Exited;
        if (!armed_ && !hovered_ && !wasHovered) return false;
        break;
    }

    case PointerEventType::Scroll:
        return false;
    }

    event.accepted = true;
    invalidate();
    return true;
}

void Button::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    invalidate();
}

void Button::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) armed_ = false;
    invalidate();
}

void Button::setPrimary(bool primary) {
    if (primary_ == primary) return;
    primary_ = primary;
    invalidate();
}

void Button::setSwatch(std::optional<Color> swatch) {
    if (swatch_ == swatch) return;
    swatch_ = swatch;
    invalidate();
}

void Button::paint(PaintContext& context) const {
    paintSelf(context);
}

void Button::paintSelf(PaintContext& context) const {
    const Palette& palette = colors();
    const core::Rect rect = bounds();

    if (swatch_.has_value()) {
        // Swatch: the color chip; the current choice gets an accent ring.
        if (visualState() != VisualState::Normal) {
            context.fillRoundedRect(rect, palette.controlFillHover, kCornerRadius);
        }
        const core::Rect chip = rect.inset(core::Insets::uniform(5.0));
        context.fillRoundedRect(chip, *swatch_, chip.size.height / 2.0);
        strokeRoundedRect(context, chip, palette.controlBorder, 1.0, chip.size.height / 2.0);
        if (active_) strokeRoundedRect(context, rect.inset(core::Insets::uniform(1.0)), palette.accent, 2.0, kCornerRadius);
    } else {
        Color fill = palette.controlFill;
        Color text = enabled_ ? palette.textPrimary : palette.textDisabled;
        if (primary_ && enabled_) {
            fill = palette.accentFill;
            text = palette.onAccent;
        } else if (active_) {
            fill = palette.accentSubtle;
            text = enabled_ ? palette.accentText : palette.textDisabled;
        }
        switch (visualState()) {
        case VisualState::Pressed:
            fill = primary_ ? composite(palette.controlFillPressed, palette.accentFill) : palette.controlFillPressed;
            break;
        case VisualState::Hovered:
            if (!active_) fill = primary_ ? composite(palette.controlFillHover, palette.accentFill) : palette.controlFillHover;
            break;
        case VisualState::Normal: break;
        }
        context.fillRoundedRect(rect, fill, kCornerRadius);
        context.drawText(label_, rect, font_, text, TextAlign::Center);
    }
    if (isFocused()) {
        // Keyboard focus only (the toolbar's keyboard navigation).
        drawFocusRing(context, rect, kCornerRadius);
    }
}

} // namespace rivet::ui
