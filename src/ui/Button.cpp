#include "ui/Button.hpp"

#include "core/geometry/Insets.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Font kLabelFont{13.0, Font::Weight::Semibold};

constexpr Color kNormalFill = Color::rgba(231.0 / 255.0, 231.0 / 255.0, 231.0 / 255.0, 1.0); // #E7E7E7
constexpr Color kHoverFill = Color::rgba(220.0 / 255.0, 220.0 / 255.0, 220.0 / 255.0, 1.0);  // #DCDCDC
constexpr Color kPressedFill = Color::rgba(201.0 / 255.0, 201.0 / 255.0, 201.0 / 255.0, 1.0); // #C9C9C9
constexpr Color kBorder = Color::rgba(0.0, 0.0, 0.0, 0.15);
constexpr Color kLabelColor = Color::black();

} // namespace

Button::Button(std::string label) : label_(std::move(label)) {}

void Button::setLabel(std::string label) {
    if (label_ == label) return;
    label_ = std::move(label);
    invalidate();
}

void Button::setOnClick(std::function<void()> onClick) {
    onClick_ = std::move(onClick);
}

core::Size Button::preferredSize(const PaintContext& context) const {
    const core::Size textSize = context.measureText(label_, kLabelFont);
    return core::Size{textSize.width + 2.0 * kHorizontalPadding,
                      std::max(textSize.height + 2.0 * kVerticalPadding, kMinHeight)};
}

Button::VisualState Button::visualState() const {
    if (armed_ && hovered_) return VisualState::Pressed;
    if (armed_ || hovered_) return VisualState::Hovered;
    return VisualState::Normal;
}

void Button::fireClick() {
    if (onClick_) onClick_();
}

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
        hovered_ = inside;
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

void Button::paint(PaintContext& context) const {
    paintSelf(context);
}

void Button::paintSelf(PaintContext& context) const {
    const core::Rect rect = bounds();

    Color fill = kNormalFill;
    switch (visualState()) {
    case VisualState::Pressed: fill = kPressedFill; break;
    case VisualState::Hovered: fill = kHoverFill; break;
    case VisualState::Normal: break;
    }

    context.fillRoundedRect(rect, fill, kCornerRadius);
    // Inset half a stroke width so the border stays inside the clip.
    context.strokeRect(rect.inset(core::Insets::uniform(0.5)), kBorder, 1.0);
    context.drawText(label_, rect, kLabelFont, kLabelColor, TextAlign::Center);
}

} // namespace rivet::ui
