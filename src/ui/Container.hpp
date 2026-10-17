#pragma once

#include "ui/Drawing.hpp"
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

// A floating card (ADR-0033): modal sheets, prompts, popovers and the find
// bar. Rounded (kRadiusCard) background from a token (panelBackground by
// default), a 1 px cardBorder outline and a soft cardShadow drop shadow
// painted outside its frame, so it separates from the page behind it.
class Card : public Container {
public:
    Card() { setBackground(&Palette::panelBackground); }

    void paintBehind(PaintContext& context) const override {
        const core::Rect box = frame();
        if (box.isEmpty()) return;
        const Color shadow = colors().cardShadow;
        constexpr int kLayers = 4;
        for (int layer = kLayers; layer >= 1; --layer) {
            const double grow = metrics::kCardShadowRadius * static_cast<double>(layer) / kLayers;
            const core::Rect halo{box.minX() - grow, box.minY() - grow + metrics::kCardShadowOffsetY,
                                  box.size.width + 2.0 * grow, box.size.height + 2.0 * grow};
            context.fillRoundedRect(halo, withAlpha(shadow, shadow.a / (1.0 + static_cast<double>(layer))),
                                    metrics::kRadiusCard + grow);
        }
    }
    void paintSelf(PaintContext& context) const override {
        const core::Rect rect = bounds();
        context.fillRoundedRect(rect, backgroundColor(), metrics::kRadiusCard);
        strokeRoundedRect(context, rect, colors().cardBorder, metrics::kHairline, metrics::kRadiusCard);
    }
};

// A chrome strip (context bar, status bar): the chrome background with
// optional 1 px separators along its top and bottom edges.
class ChromeBar : public Container {
public:
    ChromeBar(bool topHairline, bool bottomHairline) : top_(topHairline), bottom_(bottomHairline) {
        setBackground(&Palette::chromeBackground);
    }
    void paintSelf(PaintContext& context) const override {
        Container::paintSelf(context);
        const core::Rect rect = bounds();
        if (rect.size.height <= 0.0) return;
        if (top_) context.fillRect(core::Rect{rect.minX(), rect.minY(), rect.size.width, 1.0}, colors().chromeSeparator);
        if (bottom_) {
            context.fillRect(core::Rect{rect.minX(), rect.maxY() - 1.0, rect.size.width, 1.0}, colors().chromeSeparator);
        }
    }

private:
    bool top_;
    bool bottom_;
};

} // namespace rivet::ui
