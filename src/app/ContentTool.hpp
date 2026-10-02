// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstdint>

namespace rivet::app {

// The content tools (ADR-0014/0015). They share the shell's ONE tool state
// with the annotation tools (AnnotationTool.hpp): a content tool other than
// None suspends annotation input (the annotation layer keeps painting), and
// choosing any annotation tool, Select included, returns to ContentTool::None.
enum class ContentTool : std::uint8_t {
    None,
    SelectObject, // select / move / resize / delete / retype existing content
    AddText,      // click or drag places a new text block
};

// Content commands the shell exposes to platform menus (integers on the
// platform side: keep the values stable).
enum class ContentCommand : std::uint8_t {
    ToolEdit = 0,     // toggles ContentTool::SelectObject
    ToolAddText = 1,  // toggles ContentTool::AddText
    EditText = 2,     // opens the inline editor on the selected block
    ReplaceImage = 3, // file dialog, decode on a worker, replaceImage
    BringToFront = 4,
    DeleteObject = 5,
};

} // namespace rivet::app
