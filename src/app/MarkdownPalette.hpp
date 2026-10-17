// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Theme.hpp"
#include "ui/UiTypes.hpp"

namespace rivet::app {

// Colours of the rendered Markdown view, derived from the ui theme tokens
// (ADR-0033 §6): the preview, its diagrams and formulas follow dark mode.
// The PDF exporter prints with the light palette through the same role
// mapping (MarkdownGraphics.hpp), so paper always looks like light mode.
// Every entry is opaque except the selection/match tints.
struct MarkdownPreviewPalette {
    // The light palette (export, tests).
    MarkdownPreviewPalette() : MarkdownPreviewPalette(ui::Theme::light()) {}
    explicit MarkdownPreviewPalette(const ui::Theme& theme) {
        const ui::Palette& c = theme.colors;
        const auto solid = [&c](const ui::Color& color) { return ui::composite(color, c.windowBackground); };
        background = c.windowBackground;
        text = c.textPrimary;
        heading = c.textPrimary;
        quoteText = c.textSecondary;
        quoteBar = solid(c.controlBorder);
        link = c.linkText;
        linkHover = c.linkHover;
        codeText = c.textPrimary;
        codeBackground = solid(c.codeBackground);
        inlineCodeBackground = solid(c.inlineCodeBackground);
        rule = solid(c.controlBorder);
        tableGrid = solid(c.controlBorder);
        tableHeaderBackground = solid(c.codeBackground);
        marker = c.textSecondary;
        placeholderBackground = solid(c.codeBackground);
        placeholderText = c.textSecondary;
        selection = c.selectionHighlight;
        selectionInactive = c.selectionInactive;
        match = c.searchHighlight;
        currentMatch = c.searchHighlightCurrent;
        overflowIndicator = c.controlBorder;
        error = c.danger;
    }

    ui::Color background;
    ui::Color text;
    ui::Color heading;
    ui::Color quoteText;
    ui::Color quoteBar;
    ui::Color link;
    ui::Color linkHover;
    ui::Color codeText;
    ui::Color codeBackground;
    ui::Color inlineCodeBackground;
    ui::Color rule;
    ui::Color tableGrid;
    ui::Color tableHeaderBackground;
    ui::Color marker;
    ui::Color placeholderBackground;
    ui::Color placeholderText;
    ui::Color selection;
    ui::Color selectionInactive;
    ui::Color match;
    ui::Color currentMatch;
    ui::Color overflowIndicator;
    // Unknown TeX commands and the "could not be rendered" notices.
    ui::Color error;
};

} // namespace rivet::app
