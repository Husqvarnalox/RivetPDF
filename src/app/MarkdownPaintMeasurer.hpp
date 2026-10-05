// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "markdown/MarkdownLayout.hpp"
#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace rivet::app {

// ITextMeasurer backed by a ui::PaintContext.
//
// A PaintContext only exists while a frame is painted, but hit-testing and
// selection run from mouse events. So the measurer works in two modes:
//   - bound (inside paint): every measurement goes to the context (exact);
//   - unbound (input handling): widths are the sum of per-code-point advances
//     recorded while bound (warm()), which is what caret mapping needs (the
//     same approach as ui::TextField). Code points never warmed fall back to
//     a size-proportional estimate.
// Vertical metrics are derived from the context's line height (ascent 80%,
// descent 20%), which is also how the preview positions drawn text, so layout
// and painting agree by construction.
//
// Call clear() when the font configuration or the display scale changes
// (the owner also drops its MeasureCache then). Main thread only.
class MarkdownPaintMeasurer final : public markdown::ITextMeasurer {
public:
    static ui::Font fontFor(const markdown::TextStyle& style);

    // Binds `context` until unbind(); not owning.
    void bind(const ui::PaintContext* context) { context_ = context; }
    void unbind() { context_ = nullptr; }
    bool bound() const { return context_ != nullptr; }

    markdown::TextMetrics measure(std::string_view utf8, const markdown::TextStyle& style) const override;

    // Records per-code-point advances of `utf8` in `style` (needs a bound context).
    void warm(std::string_view utf8, const markdown::TextStyle& style) const;

    // Line height last measured for `style` (no measuring; style.size*1.2 when never measured).
    double lineHeight(const markdown::TextStyle& style) const { return dataFor(style).lineHeight; }

    void clear();

private:
    struct StyleData {
        double lineHeight = 0.0;
        std::unordered_map<char32_t, double> advances;
    };
    static std::uint64_t styleKey(const markdown::TextStyle& style);
    StyleData& dataFor(const markdown::TextStyle& style) const;

    const ui::PaintContext* context_ = nullptr;
    mutable std::unordered_map<std::uint64_t, StyleData> styles_;
};

} // namespace rivet::app
