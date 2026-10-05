// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownHostView.hpp"

#include <algorithm>
#include <string_view>

namespace rivet::app {

namespace {

const char* modeName(MarkdownDisplayMode mode) {
    switch (mode) {
    case MarkdownDisplayMode::Rendered: return "Rendered";
    case MarkdownDisplayMode::Source: return "Source";
    case MarkdownDisplayMode::Split: return "Split";
    }
    return "";
}

// Placeholder: no parsing, no rendering. Shows which mode is active and the
// first source lines so the seam can be exercised end to end.
class PlaceholderMarkdownView final : public MarkdownHostView {
public:
    using MarkdownHostView::MarkdownHostView;

    void paintSelf(ui::PaintContext& context) const override {
        const core::Rect rect = bounds();
        context.fillRect(rect, ui::Color::white());
        if (state_ == nullptr) return;
        constexpr double kLineHeight = 18.0;
        constexpr double kPad = 16.0;
        const ui::Font font{13.0};
        context.drawText(std::string("Markdown tab — ") + modeName(state_->mode()) + " mode",
                         core::Rect{kPad, 8.0, std::max(0.0, rect.size.width - 2 * kPad), kLineHeight},
                         ui::Font{13.0, ui::Font::Weight::Semibold}, ui::Color::gray(0.35), ui::TextAlign::Left);
        std::string_view rest = state_->source();
        double y = 8.0 + 2 * kLineHeight;
        while (!rest.empty() && y + kLineHeight <= rect.size.height) {
            const std::size_t eol = rest.find('\n');
            const std::string_view line = rest.substr(0, eol);
            context.drawText(line, core::Rect{kPad, y, std::max(0.0, rect.size.width - 2 * kPad), kLineHeight},
                             font, ui::Color::gray(0.15), ui::TextAlign::Left);
            y += kLineHeight;
            if (eol == std::string_view::npos) break;
            rest.remove_prefix(eol + 1);
        }
    }
};

} // namespace

std::unique_ptr<MarkdownHostView> createMarkdownHostView(MarkdownHostEnvironment environment) {
    return std::make_unique<PlaceholderMarkdownView>(std::move(environment));
}

} // namespace rivet::app
