#pragma once

#include "ui/Theme.hpp"
#include "ui/Widget.hpp"

namespace rivet::ui {

// A theme token: the palette entry a widget reads at paint time.
using ColorToken = Color Palette::*;

// Generic container: paints children and forwards input/hit-testing through
// the Widget defaults. layout() remains the default no-op, so children keep
// their manually assigned frames.
class Container : public Widget {
public:
    // Fills the background with the token's current color (or the explicit
    // color); fully transparent by default paints nothing.
    void paintSelf(PaintContext& context) const override {
        const Color color = backgroundToken_ != nullptr ? colors().*backgroundToken_ : backgroundColor_;
        if (color.a > 0.0) context.fillRect(bounds(), color);
    }

    // Background from a theme token (follows theme switches).
    void setBackground(ColorToken token) {
        if (backgroundToken_ == token) return;
        backgroundToken_ = token;
        invalidate();
    }
    // Background as a fixed color (document-derived colors only; chrome uses
    // setBackground(token)).
    void setBackgroundColor(const Color& color) {
        if (backgroundColor_ == color && backgroundToken_ == nullptr) return;
        backgroundToken_ = nullptr;
        backgroundColor_ = color;
        invalidate();
    }
    Color backgroundColor() const {
        return backgroundToken_ != nullptr ? colors().*backgroundToken_ : backgroundColor_;
    }

private:
    ColorToken backgroundToken_ = nullptr;
    Color backgroundColor_{0.0, 0.0, 0.0, 0.0}; // transparent by default
};

} // namespace rivet::ui
