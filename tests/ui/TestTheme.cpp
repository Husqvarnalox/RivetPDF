// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "ui/Theme.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Design tokens (ADR-0033): both palettes keep every text token at >= 4.5:1
// against the surfaces it is drawn on, for the default accent and for
// awkward platform accents (yellow, very light, very dark).

using rivet::ui::Color;
using rivet::ui::Palette;
using rivet::ui::Theme;

namespace {

struct Pair {
    const char* name;
    Color Palette::* text;
    Color Palette::* background;
    // A translucent background is composited over this opaque surface first.
    Color Palette::* under = nullptr;
};

const std::vector<Pair>& textPairs() {
    static const std::vector<Pair> pairs = {
        {"textPrimary/window", &Palette::textPrimary, &Palette::windowBackground},
        {"textPrimary/chrome", &Palette::textPrimary, &Palette::chromeBackground},
        {"textPrimary/canvas", &Palette::textPrimary, &Palette::canvasBackground},
        {"textPrimary/panel", &Palette::textPrimary, &Palette::panelBackground},
        {"textPrimary/popover", &Palette::textPrimary, &Palette::popoverBackground},
        {"textPrimary/field", &Palette::textPrimary, &Palette::fieldBackground},
        {"textPrimary/controlFill", &Palette::textPrimary, &Palette::controlFill, &Palette::chromeBackground},
        {"textPrimary/controlPressed", &Palette::textPrimary, &Palette::controlFillPressed,
         &Palette::chromeBackground},
        {"textPrimary/selectedSegment", &Palette::textPrimary, &Palette::controlSelectedFill},
        {"textPrimary/listSelection", &Palette::textPrimary, &Palette::listSelection, &Palette::chromeBackground},
        {"textSecondary/window", &Palette::textSecondary, &Palette::windowBackground},
        {"textSecondary/chrome", &Palette::textSecondary, &Palette::chromeBackground},
        {"textSecondary/canvas", &Palette::textSecondary, &Palette::canvasBackground},
        {"textSecondary/panel", &Palette::textSecondary, &Palette::panelBackground},
        {"textSecondary/popover", &Palette::textSecondary, &Palette::popoverBackground},
        {"textSecondary/controlFill", &Palette::textSecondary, &Palette::controlFill, &Palette::chromeBackground},
        {"accentText/chrome", &Palette::accentText, &Palette::chromeBackground},
        {"onAccent/accentFill", &Palette::onAccent, &Palette::accentFill},
        {"linkText/window", &Palette::linkText, &Palette::windowBackground},
        {"linkHover/window", &Palette::linkHover, &Palette::windowBackground},
        {"tooltipText/tooltip", &Palette::tooltipText, &Palette::tooltipBackground, &Palette::canvasBackground},
        {"hudText/hud", &Palette::hudText, &Palette::hudBackground, &Palette::pageBackground},
        {"editorInk/editorPaper", &Palette::editorInk, &Palette::editorPaper},
    };
    return pairs;
}

void checkPalette(const Theme& theme, const char* label) {
    for (const Pair& pair : textPairs()) {
        Color background = theme.colors.*(pair.background);
        if (pair.under != nullptr) background = rivet::ui::composite(background, theme.colors.*(pair.under));
        const double ratio = rivet::ui::contrastRatio(theme.colors.*(pair.text), background);
        if (ratio < 4.5) std::fprintf(stderr, "    %s %s: %.2f\n", label, pair.name, ratio);
        CHECK(ratio >= 4.5);
    }
}

} // namespace

RIVET_TEST(contrastRatioMatchesWcagReferenceValues) {
    CHECK_NEAR(rivet::ui::contrastRatio(Color{0, 0, 0, 1}, Color{1, 1, 1, 1}), 21.0, 1e-9);
    CHECK_NEAR(rivet::ui::contrastRatio(Color{1, 1, 1, 1}, Color{1, 1, 1, 1}), 1.0, 1e-9);
    // #777777 on white is the classic 4.48:1 just-fail.
    const double v = 0x77 / 255.0;
    CHECK_NEAR(rivet::ui::contrastRatio(Color{v, v, v, 1}, Color{1, 1, 1, 1}), 4.48, 0.01);
}

RIVET_TEST(textTokensMeetContrastInBothThemes) {
    checkPalette(Theme::make(false), "light");
    checkPalette(Theme::make(true), "dark");
}

RIVET_TEST(textTokensMeetContrastWithUnusualAccents) {
    const std::vector<std::pair<const char*, Color>> accents = {
        {"yellow", Color{1.0, 0.8, 0.0, 1.0}},   {"lightGray", Color{0.85, 0.85, 0.85, 1.0}},
        {"navy", Color{0.0, 0.05, 0.25, 1.0}},   {"green", Color{0.2, 0.75, 0.3, 1.0}},
        {"pink", Color{1.0, 0.18, 0.57, 1.0}},   {"graphite", Color{0.55, 0.55, 0.57, 1.0}},
    };
    for (const auto& [name, accent] : accents) {
        checkPalette(Theme::make(false, accent), name);
        checkPalette(Theme::make(true, accent), name);
    }
}

RIVET_TEST(themesDifferAndPagesStayWhite) {
    const Theme light = Theme::make(false);
    const Theme dark = Theme::make(true);
    CHECK(!light.dark);
    CHECK(dark.dark);
    CHECK(rivet::ui::relativeLuminance(light.colors.windowBackground) >
          rivet::ui::relativeLuminance(dark.colors.windowBackground));
    // PDF pages stay as authored in both themes.
    CHECK(light.colors.pageBackground == (Color{1, 1, 1, 1}));
    CHECK(dark.colors.pageBackground == (Color{1, 1, 1, 1}));
    // The accent is the platform's when given.
    const Color accent{0.5, 0.2, 0.7, 1.0};
    CHECK(Theme::make(false, accent).colors.accent == accent);
    CHECK(Theme::make(false).colors.accent == Theme::defaultAccent(false));
}

RIVET_TEST(currentThemeSwitchBumpsGeneration) {
    const auto before = rivet::ui::themeGeneration();
    rivet::ui::setCurrentTheme(Theme::make(true));
    CHECK(rivet::ui::currentTheme().dark);
    CHECK(rivet::ui::themeGeneration() > before);
    CHECK(rivet::ui::colors().windowBackground == Theme::make(true).colors.windowBackground);
    rivet::ui::setCurrentTheme(Theme::make(false));
    CHECK(!rivet::ui::currentTheme().dark);
}

RIVET_TEST(ensureContrastReachesTheRatio) {
    const Color bg{1, 1, 1, 1};
    const Color adjusted = rivet::ui::ensureContrast(Color{1.0, 0.85, 0.0, 1.0}, bg, 4.5);
    CHECK(rivet::ui::contrastRatio(adjusted, bg) >= 4.5);
    const Color darkBg{0.1, 0.1, 0.1, 1};
    const Color lifted = rivet::ui::ensureContrast(Color{0.0, 0.0, 0.4, 1.0}, darkBg, 4.5);
    CHECK(rivet::ui::contrastRatio(lifted, darkBg) >= 4.5);
}
