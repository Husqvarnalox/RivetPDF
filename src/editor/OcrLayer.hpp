// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Size.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

// Pure planning of an OCR text layer (ADR-0027): recognized words, located in
// the DISPLAY space of a page view, become invisible text runs in the source
// page's USER space. No PDF engine, no platform code.
//
// Display space is the one PdfPageGeometry.hpp defines (points, origin at the
// top-left of the displayed page, y-down, /Rotate and crop applied). The
// recognizer works in the pixels of a bitmap that covers the whole displayed
// page; ocrWordFromBitmap() maps those pixels to display space, and
// planInvisibleText() maps display space to user space through the page view,
// so rotation and CropBox offsets are handled by the one central mapping.

namespace rivet::editor {

// One recognized word in display space. The quad's vertices are in READING
// order of the word as it appears on the displayed page: top-left, top-right,
// bottom-right, bottom-left. The baseline direction is bottomLeft ->
// bottomRight, so slightly rotated or skewed lines keep their angle.
struct OcrWord {
    std::string text; // UTF-8
    std::array<core::Point, 4> quad{};
};

// Maps a word box given in pixels of a bitmap of `bitmapWidth` x
// `bitmapHeight` that covers the whole displayed page of `displaySize`
// (points). The vertex order is the one of OcrWord::quad.
OcrWord ocrWordFromBitmap(std::string text, const std::array<core::Point, 4>& bitmapQuad,
                          std::uint32_t bitmapWidth, std::uint32_t bitmapHeight,
                          const core::Size& displaySize);

struct InvisibleTextPlan {
    std::vector<pdf::PdfInvisibleTextRun> runs;
    // Words that produced no run (empty after dropping characters the
    // bundled face cannot write, degenerate boxes, too many words).
    std::size_t skipped = 0;
};

// Words are kept in the given order, at most kMaxInvisibleRunsPerPage runs.
// Characters the bundled Sans face cannot write and control/space characters
// are dropped from a word; a word shorter than half a point or longer than
// kMaxInvisibleRunBytes after that is skipped. Each run sits on the word's
// baseline (a fifth of the box height above the bottom edge), is sized to the
// box height, and is stretched horizontally so its natural advance equals the
// box width.
InvisibleTextPlan planInvisibleText(const pdf::PdfPageView& view, std::span<const OcrWord> words);

} // namespace rivet::editor
