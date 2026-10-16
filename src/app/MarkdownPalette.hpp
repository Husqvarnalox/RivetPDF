// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/UiTypes.hpp"

namespace rivet::app {

// Colours of the rendered Markdown view. The PDF exporter prints diagrams and
// formulas with the default (light) palette through the same role mapping
// (MarkdownGraphics.hpp), so screen and paper agree.
struct MarkdownPreviewPalette {
    ui::Color background = ui::Color::white();
    ui::Color text = ui::Color::gray(0.13);
    ui::Color heading = ui::Color::gray(0.08);
    ui::Color quoteText = ui::Color::gray(0.38);
    ui::Color quoteBar = ui::Color::gray(0.78);
    ui::Color link = ui::Color::rgba(0.05, 0.36, 0.78, 1.0);
    ui::Color linkHover = ui::Color::rgba(0.0, 0.22, 0.55, 1.0);
    ui::Color codeText = ui::Color::gray(0.18);
    ui::Color codeBackground = ui::Color::gray(0.955);
    ui::Color inlineCodeBackground = ui::Color::gray(0.93);
    ui::Color rule = ui::Color::gray(0.8);
    ui::Color tableGrid = ui::Color::gray(0.8);
    ui::Color tableHeaderBackground = ui::Color::gray(0.94);
    ui::Color marker = ui::Color::gray(0.35);
    ui::Color placeholderBackground = ui::Color::gray(0.94);
    ui::Color placeholderText = ui::Color::gray(0.4);
    ui::Color selection = ui::Color::rgba(0.25, 0.5, 0.95, 0.3);
    ui::Color match = ui::Color::rgba(1.0, 0.85, 0.0, 0.45);
    ui::Color currentMatch = ui::Color::rgba(1.0, 0.55, 0.0, 0.6);
    ui::Color overflowIndicator = ui::Color::rgba(0.0, 0.0, 0.0, 0.25);
    // Unknown TeX commands and the "could not be rendered" notices.
    ui::Color error = ui::Color::rgba(0.78, 0.12, 0.1, 1.0);
};

} // namespace rivet::app
