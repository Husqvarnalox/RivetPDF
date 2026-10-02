// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationTool.hpp"
#include "editor/Annotations.hpp"
#include "pdf/PdfAnnotation.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace rivet::app {

// The annotation kind a creation tool makes (nullopt for Select).
inline std::optional<pdf::PdfAnnotationKind> annotationKindForTool(AnnotationTool tool) {
    using Kind = pdf::PdfAnnotationKind;
    switch (tool) {
    case AnnotationTool::Select: return std::nullopt;
    case AnnotationTool::Highlight: return Kind::Highlight;
    case AnnotationTool::Underline: return Kind::Underline;
    case AnnotationTool::StrikeOut: return Kind::StrikeOut;
    case AnnotationTool::Note: return Kind::Note;
    case AnnotationTool::Ink: return Kind::Ink;
    case AnnotationTool::Rectangle: return Kind::Square;
    case AnnotationTool::Ellipse: return Kind::Circle;
    case AnnotationTool::Line: return Kind::Line;
    case AnnotationTool::Arrow: return Kind::Arrow;
    case AnnotationTool::Stamp: return Kind::Stamp;
    }
    return std::nullopt;
}

inline const char* annotationToolLabel(AnnotationTool tool) {
    switch (tool) {
    case AnnotationTool::Select: return "Select";
    case AnnotationTool::Highlight: return "Highlight";
    case AnnotationTool::Underline: return "Underline";
    case AnnotationTool::StrikeOut: return "Strike";
    case AnnotationTool::Note: return "Note";
    case AnnotationTool::Ink: return "Ink";
    case AnnotationTool::Rectangle: return "Rect";
    case AnnotationTool::Ellipse: return "Ellipse";
    case AnnotationTool::Line: return "Line";
    case AnnotationTool::Arrow: return "Arrow";
    case AnnotationTool::Stamp: return "Stamp";
    }
    return "";
}

// Which style fields apply to a tool (what the toolbar offers for it).
inline bool toolUsesWidth(AnnotationTool tool) {
    return tool == AnnotationTool::Ink || tool == AnnotationTool::Rectangle ||
           tool == AnnotationTool::Ellipse || tool == AnnotationTool::Line || tool == AnnotationTool::Arrow;
}
inline bool toolUsesFill(AnnotationTool tool) {
    return tool == AnnotationTool::Rectangle || tool == AnnotationTool::Ellipse;
}
inline bool toolUsesOpacity(AnnotationTool tool) {
    // Notes and stamps are drawn opaque by the appearance builder.
    return tool != AnnotationTool::Select && tool != AnnotationTool::Note && tool != AnnotationTool::Stamp;
}
inline bool toolUsesStampName(AnnotationTool tool) { return tool == AnnotationTool::Stamp; }

// The remembered style of one creation tool.
struct ToolStyle {
    editor::AnnotationStyle style;
    pdf::PdfStampName stampName = pdf::PdfStampName::Approved;
};

inline constexpr std::size_t kPresetColorCount = 6;
// Yellow, red, green, blue, orange, black.
inline constexpr std::array<pdf::PdfColor, kPresetColorCount> kPresetColors = {{
    {1.0F, 0.85F, 0.0F},
    {0.90F, 0.15F, 0.15F},
    {0.20F, 0.70F, 0.30F},
    {0.15F, 0.40F, 0.90F},
    {1.0F, 0.55F, 0.0F},
    {0.0F, 0.0F, 0.0F},
}};
inline constexpr std::array<float, 4> kOpacitySteps = {0.25F, 0.5F, 0.75F, 1.0F};
inline constexpr std::array<float, 4> kWidthSteps = {1.0F, 2.0F, 4.0F, 8.0F};
inline constexpr std::array<pdf::PdfStampName, 4> kStampNames = {
    pdf::PdfStampName::Approved, pdf::PdfStampName::Draft, pdf::PdfStampName::Confidential,
    pdf::PdfStampName::Final};

// The factory default of a tool: highlight yellow at 0.4 opacity, underline
// red and strikeout blue (opaque), ink and shapes red 2 pt, notes yellow,
// stamps red "Approved".
inline ToolStyle defaultToolStyle(AnnotationTool tool) {
    ToolStyle result;
    result.style.opacity = 1.0F;
    result.style.borderWidth = 2.0F;
    switch (tool) {
    case AnnotationTool::Highlight:
        result.style.color = kPresetColors[0];
        result.style.opacity = 0.4F;
        break;
    case AnnotationTool::Underline:
        result.style.color = kPresetColors[1];
        break;
    case AnnotationTool::StrikeOut:
        result.style.color = kPresetColors[3];
        break;
    case AnnotationTool::Note:
        result.style.color = kPresetColors[0];
        break;
    default: // Ink, shapes, lines, stamp (and Select, unused)
        result.style.color = kPresetColors[1];
        break;
    }
    return result;
}

} // namespace rivet::app
