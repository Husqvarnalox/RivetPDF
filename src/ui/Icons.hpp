// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Path.hpp"
#include "ui/UiTypes.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

namespace rivet::ui {

// The Rivet icon set (ADR-0033 §3): portable vector icons drawn as code on a
// 20x20 grid with 1.5 px rounded strokes, crisp at any scale and identical on
// every platform. No image assets.
enum class Icon : std::uint8_t {
    Open,
    Save,
    Print,
    Sidebar,
    ZoomIn,
    ZoomOut,
    FitWidth,
    FitPage,
    ActualSize,
    Search,
    Previous,
    Next,
    Undo,
    Redo,
    RotateLeft,
    RotateRight,
    Select,
    Hand,
    Highlight,
    Underline,
    StrikeOut,
    Note,
    Ink,
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Stamp,
    Sign,
    EditContent,
    AddText,
    Crop,
    Redact,
    Form,
    Compare,
    Ocr,
    MarkdownRendered,
    MarkdownSource,
    MarkdownSplit,
    Close,
    ChevronLeft,
    ChevronRight,
    ChevronUp,
    ChevronDown,
    More,
    View,
    Bold,
    Italic,
    Code,
    Link,
    Heading,
    BulletList,
    NumberedList,
    TaskList,
    Quote,
    Document,
};

inline constexpr std::size_t kIconCount = static_cast<std::size_t>(Icon::Document) + 1;

// One drawing step of an icon, in 20x20 grid coordinates.
struct IconShape {
    Path path;
    bool fill = false; // filled (dots, bars) instead of stroked
};

// The icon's shapes on the 20x20 grid (exposed for tests).
std::vector<IconShape> iconShapes(Icon icon);
// Stable name for diagnostics ("open", "zoom-in", ...).
std::string_view iconName(Icon icon);

// Draws `icon` scaled into `box` (square boxes keep the grid's aspect; the
// icon is centered) with `color`. The stroke stays 1.5 px at 20 px and
// scales with the box.
void drawIcon(PaintContext& context, Icon icon, const core::Rect& box, const Color& color);

} // namespace rivet::ui
