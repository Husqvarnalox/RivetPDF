#include "app/TextLabel.hpp"

namespace rivet::app {

TextLabel::TextLabel(std::string text, ui::Font font, ui::Color color, ui::TextAlign align)
    : text_(std::move(text)), font_(font), color_(color), align_(align) {}

void TextLabel::setText(std::string text) {
    if (text_ == text) return;
    text_ = std::move(text);
    invalidate();
}

void TextLabel::setColor(ui::Color color) {
    if (color_ == color) return;
    color_ = color;
    invalidate();
}

core::Size TextLabel::preferredSize(const ui::PaintContext& context) const {
    const core::Size measured = context.measureText(text_, font_);
    return core::Size{measured.width + 12.0, font_.size + 8.0};
}

core::Insets TextLabel::textInset() const {
    return core::Insets{6.0, 0.0, 6.0, 0.0};
}

void TextLabel::paintSelf(ui::PaintContext& context) const {
    context.drawText(text_, bounds().inset(textInset()), font_, color_, align_);
}

} // namespace rivet::app
