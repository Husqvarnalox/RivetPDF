#pragma once

#include "ui/UiTypes.hpp"
#include "ui/Widget.hpp"

#include <string>

namespace rivet::app {

// Single-line static text label used by the shell (zoom display, status bar).
class TextLabel final : public ui::Widget {
public:
    TextLabel(std::string text, ui::Font font = {}, ui::Color color = ui::Color::gray(0.25),
              ui::TextAlign align = ui::TextAlign::Left);

    void setText(std::string text);
    const std::string& text() const { return text_; }
    void setColor(ui::Color color);

    core::Size preferredSize(const ui::PaintContext& context) const override;
    void paintSelf(ui::PaintContext& context) const override;

private:
    core::Insets textInset() const;

    std::string text_;
    ui::Font font_;
    ui::Color color_;
    ui::TextAlign align_;
};

} // namespace rivet::app
