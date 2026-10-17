// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Insets.hpp"
#include "core/geometry/Rect.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Path.hpp"
#include "ui/Theme.hpp"

namespace rivet::ui {

// Small drawing helpers shared by the themed controls.

// Rounded-rectangle outline centered on the rect inset by half the stroke
// (stays inside the rect, so a widget's clip never cuts it).
inline void strokeRoundedRect(PaintContext& context, const core::Rect& rect, const Color& color, double width,
                              double radius) {
    Path path;
    path.addRoundedRect(rect.inset(core::Insets::uniform(width / 2.0)), radius - width / 2.0);
    context.strokePath(path, color, width, LineCap::Butt, LineJoin::Round);
}

// The keyboard focus ring (ADR-0033 §4: keyboard focus only).
inline void drawFocusRing(PaintContext& context, const core::Rect& rect, double radius) {
    strokeRoundedRect(context, rect, colors().focusRing, 2.0, radius);
}

// A 1 px hairline along the bottom (or top) edge of `rect`.
inline void drawBottomHairline(PaintContext& context, const core::Rect& rect, const Color& color) {
    if (rect.size.height <= 0.0) return;
    context.fillRect(core::Rect{rect.minX(), rect.maxY() - 1.0, rect.size.width, 1.0}, color);
}
inline void drawTopHairline(PaintContext& context, const core::Rect& rect, const Color& color) {
    if (rect.size.height <= 0.0) return;
    context.fillRect(core::Rect{rect.minX(), rect.minY(), rect.size.width, 1.0}, color);
}

} // namespace rivet::ui
