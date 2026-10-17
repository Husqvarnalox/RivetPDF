// SPDX-License-Identifier: MPL-2.0
#include "ui/Theme.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::ui {
namespace {

// The ONLY place raw colors live (ADR-0033): every palette entry below.
constexpr Color hex(unsigned rgb, double alpha = 1.0) {
    return Color{static_cast<double>((rgb >> 16U) & 0xFFU) / 255.0, static_cast<double>((rgb >> 8U) & 0xFFU) / 255.0,
                 static_cast<double>(rgb & 0xFFU) / 255.0, alpha};
}

constexpr Color kWhite = hex(0xFFFFFF);
constexpr Color kBlack = hex(0x000000);

double channel(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

Palette lightPalette(const Color& accentIn) {
    const Color accent = withAlpha(accentIn, 1.0);
    Palette p;
    p.windowBackground = kWhite;
    p.chromeBackground = hex(0xF2F2F2);
    p.chromeSeparator = hex(0x000000, 0.11);
    p.chromeActiveTab = hex(0xF2F2F2);
    p.tabStripBackground = hex(0xE2E2E2);
    p.canvasBackground = hex(0xE3E3E3);
    p.pageBackground = kWhite;
    p.pageBorder = hex(0x000000, 0.16);
    p.pageShadow = hex(0x000000, 0.10);
    p.pagePlaceholder = hex(0xEBEBEB);
    p.presentationBackground = kBlack;
    p.panelBackground = hex(0xF7F7F7);
    p.popoverBackground = kWhite;
    p.popoverBorder = hex(0x000000, 0.14);
    p.cardBorder = hex(0x000000, 0.16);
    p.cardShadow = hex(0x000000, 0.16);
    p.tooltipBackground = hex(0x2B2B2B, 0.96);
    p.tooltipText = hex(0xF5F5F5);
    p.scrim = hex(0x000000, 0.35);
    p.hudBackground = hex(0x262626, 0.90);
    p.hudText = kWhite;

    p.controlFill = hex(0x000000, 0.05);
    p.controlFillHover = hex(0x000000, 0.08);
    p.controlFillPressed = hex(0x000000, 0.13);
    p.controlBorder = hex(0x000000, 0.14);
    p.controlSelectedFill = kWhite;
    p.fieldBackground = kWhite;
    p.fieldBorder = hex(0x000000, 0.18);
    p.scrollTrack = hex(0x000000, 0.04);
    p.scrollThumb = hex(0x000000, 0.28);
    p.scrollThumbActive = hex(0x000000, 0.42);
    p.listSelection = hex(0x000000, 0.09);

    p.textPrimary = hex(0x1D1D1F);
    p.textSecondary = hex(0x5E5E62);
    p.textDisabled = hex(0xA6A6A9);

    p.accent = accent;
    p.accentText = ensureContrast(accent, p.chromeBackground, 4.6);
    p.accentSubtle = withAlpha(accent, 0.16);
    p.accentFill = ensureContrast(accent, kWhite, 4.6);
    p.onAccent = kWhite;
    p.focusRing = withAlpha(accent, 0.55);

    p.danger = hex(0xD70015);
    p.success = hex(0x248A3D);
    p.warning = hex(0xB25000);

    p.searchHighlight = hex(0xFFD60A, 0.45);
    p.searchHighlightCurrent = hex(0xFF9500, 0.60);
    p.selectionHighlight = withAlpha(accent, 0.30);
    p.selectionInactive = hex(0x808080, 0.24);
    p.linkText = ensureContrast(accent, p.windowBackground, 4.6);
    p.linkOutline = withAlpha(accent, 0.80);
    p.linkHover = ensureContrast(accent, p.windowBackground, 7.0);
    p.codeBackground = hex(0xF4F4F5);
    p.inlineCodeBackground = hex(0x000000, 0.06);

    p.comparisonDeleted = hex(0xE52626, 0.30);
    p.comparisonDeletedSelected = hex(0xE52626, 0.55);
    p.comparisonDeletedPage = hex(0xE52626, 0.12);
    p.comparisonInserted = hex(0x1AA633, 0.30);
    p.comparisonInsertedSelected = hex(0x1AA633, 0.55);
    p.comparisonInsertedPage = hex(0x1AA633, 0.12);
    p.formFieldHighlight = hex(0x8CB3FF, 0.25);
    p.formFieldHover = withAlpha(accent, 0.45);
    p.formFieldEditedBorder = hex(0x8C8C8C);
    p.formOptionHighlight = withAlpha(accent, 0.18);
    p.redactionMark = hex(0xDB1A1A);
    p.editorPaper = kWhite;
    p.editorInk = kBlack;
    p.editorPaperBorder = hex(0x999999);
    p.objectReadOnly = hex(0x808080);
    return p;
}

Palette darkPalette(const Color& accentIn) {
    const Color accent = withAlpha(accentIn, 1.0);
    Palette p = lightPalette(accent); // document overlays stay the same
    p.windowBackground = hex(0x1E1E1E);
    p.chromeBackground = hex(0x2B2B2B);
    p.chromeSeparator = hex(0x000000, 0.55);
    p.chromeActiveTab = hex(0x2B2B2B);
    p.tabStripBackground = hex(0x1D1D1D);
    p.canvasBackground = hex(0x161616);
    p.pageBorder = hex(0x000000, 0.60);
    p.pageShadow = hex(0x000000, 0.45);
    p.panelBackground = hex(0x2F2F2F);
    p.popoverBackground = hex(0x333333);
    p.popoverBorder = hex(0xFFFFFF, 0.12);
    p.cardBorder = hex(0xFFFFFF, 0.14);
    p.cardShadow = hex(0x000000, 0.50);
    p.tooltipBackground = hex(0x3C3C3C, 0.97);
    p.tooltipText = hex(0xF0F0F0);
    p.scrim = hex(0x000000, 0.50);

    p.controlFill = hex(0xFFFFFF, 0.07);
    p.controlFillHover = hex(0xFFFFFF, 0.11);
    p.controlFillPressed = hex(0xFFFFFF, 0.18);
    p.controlBorder = hex(0xFFFFFF, 0.14);
    p.controlSelectedFill = hex(0x5A5A5E);
    p.fieldBackground = hex(0x1E1E1E);
    p.fieldBorder = hex(0xFFFFFF, 0.18);
    p.scrollTrack = hex(0xFFFFFF, 0.04);
    p.scrollThumb = hex(0xFFFFFF, 0.30);
    p.scrollThumbActive = hex(0xFFFFFF, 0.45);
    p.listSelection = hex(0xFFFFFF, 0.10);

    p.textPrimary = hex(0xEBEBEB);
    p.textSecondary = hex(0xA8A8AD);
    p.textDisabled = hex(0x67676B);

    p.accentText = ensureContrast(accent, p.chromeBackground, 4.6);
    p.accentSubtle = withAlpha(accent, 0.30);
    p.accentFill = ensureContrast(accent, kWhite, 4.6);
    p.focusRing = withAlpha(accent, 0.65);

    p.danger = hex(0xFF6961);
    p.success = hex(0x30D158);
    p.warning = hex(0xFFB340);

    p.searchHighlight = hex(0xFFD60A, 0.40);
    p.searchHighlightCurrent = hex(0xFF9F0A, 0.60);
    p.selectionHighlight = withAlpha(accent, 0.40);
    p.selectionInactive = hex(0x9A9A9A, 0.28);
    p.linkText = ensureContrast(mix(accent, kWhite, 0.25), p.windowBackground, 4.6);
    p.linkHover = ensureContrast(mix(accent, kWhite, 0.45), p.windowBackground, 7.0);
    p.codeBackground = hex(0x262628);
    p.inlineCodeBackground = hex(0xFFFFFF, 0.08);
    return p;
}

Theme& mutableCurrent() {
    static Theme current = Theme::make(false);
    return current;
}

std::uint64_t& generation() {
    static std::uint64_t value = 1;
    return value;
}

} // namespace

Color Theme::defaultAccent(bool dark) { return dark ? hex(0x0A84FF) : hex(0x007AFF); }

Theme Theme::make(bool dark, std::optional<Color> accent) {
    Theme theme;
    theme.dark = dark;
    const Color base = accent.value_or(defaultAccent(dark));
    theme.colors = dark ? darkPalette(base) : lightPalette(base);
    return theme;
}

const Theme& currentTheme() { return mutableCurrent(); }

void setCurrentTheme(const Theme& theme) {
    mutableCurrent() = theme;
    ++generation();
}

std::uint64_t themeGeneration() { return generation(); }

Color withAlpha(const Color& color, double alpha) { return Color{color.r, color.g, color.b, alpha}; }

Color composite(const Color& top, const Color& bottom) {
    const double a = std::clamp(top.a, 0.0, 1.0);
    return Color{top.r * a + bottom.r * (1.0 - a), top.g * a + bottom.g * (1.0 - a), top.b * a + bottom.b * (1.0 - a),
                 1.0};
}

Color mix(const Color& a, const Color& b, double t) {
    return Color{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

double relativeLuminance(const Color& color) {
    return 0.2126 * channel(color.r) + 0.7152 * channel(color.g) + 0.0722 * channel(color.b);
}

double contrastRatio(const Color& foreground, const Color& background) {
    const Color bg = withAlpha(background, 1.0);
    const Color fg = composite(foreground, bg);
    const double l1 = relativeLuminance(fg);
    const double l2 = relativeLuminance(bg);
    return (std::max(l1, l2) + 0.05) / (std::min(l1, l2) + 0.05);
}

Color ensureContrast(const Color& color, const Color& background, double ratio) {
    if (contrastRatio(color, background) >= ratio) return color;
    // Move toward the extreme that contrasts with the background.
    const Color target = relativeLuminance(withAlpha(background, 1.0)) > 0.18 ? kBlack : kWhite;
    Color result = color;
    for (int step = 1; step <= 40; ++step) {
        result = mix(withAlpha(color, 1.0), target, static_cast<double>(step) / 40.0);
        result.a = color.a;
        if (contrastRatio(result, background) >= ratio) break;
    }
    return result;
}

} // namespace rivet::ui
