// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <cstdint>

namespace rivet::app {

// The annotation tools (ADR-0013). Select is the neutral tool: it selects,
// moves and resizes existing annotations and lets text selection and links
// work. The others create annotations of one kind. (Kept free of editor and
// pdf includes: the pure interaction state machine depends on it.)
enum class AnnotationTool : std::uint8_t {
    Select,
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
};

inline constexpr std::size_t kAnnotationToolCount = 11;

// Highlight / Underline / StrikeOut: made from the text selection.
inline constexpr bool isMarkupTool(AnnotationTool tool) {
    return tool == AnnotationTool::Highlight || tool == AnnotationTool::Underline ||
           tool == AnnotationTool::StrikeOut;
}

} // namespace rivet::app
