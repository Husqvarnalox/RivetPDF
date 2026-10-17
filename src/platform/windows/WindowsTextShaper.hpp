// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "WinCommon.hpp"

#include "ui/UiTypes.hpp"

#include <dwrite.h>

#include <memory>
#include <string_view>

namespace rivet::platform::text {

// Single-line text layout for the Windows backend through DirectWrite.
// System UI family "Segoe UI" (monospace: "Consolas"), weight/italic from
// ui::Font (DirectWrite synthesizes an oblique when a family has no italic;
// font fallback covers characters the primary face lacks).
//
// Semantics mirror the CoreText path of MacosPaintContext:
//   - width  = advance width including trailing whitespace
//     (CTLineGetTypographicBounds width)
//   - ascent/descent = the primary face's typographic ascent/descent at the
//     font size (no line gap)
//   - empty text measures {0, 0}.
// measureText() and drawText() both consume layoutLine(), so they agree by
// construction. Sizes are in DIPs, which equal Rivet's logical points.
//
// Thread-safe (mutex-guarded caches over the shared, thread-safe DirectWrite
// factory). Rendering the returned layouts is main-thread only.

struct LineLayout {
    win::ComPtr<IDWriteTextLayout> layout; // null for empty text
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    // Distance from the layout box top to the first line's baseline, i.e.
    // where DrawTextLayout puts the baseline relative to its origin.
    double layoutBaseline = 0.0;
};

// Creates the shared DirectWrite factory. Returns false when DirectWrite is
// unavailable (text then measures and draws as empty).
bool initialize();
void shutdown();

std::shared_ptr<const LineLayout> layoutLine(std::string_view text, const ui::Font& font);

} // namespace rivet::platform::text
