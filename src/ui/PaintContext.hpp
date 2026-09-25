#pragma once

#include "core/Bitmap.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/UiTypes.hpp"

#include <string_view>

namespace rivet::ui {

// Abstract painter implemented by the platform layer (CoreGraphics on macOS).
//
// COORDINATE CONTRACT (the macOS backend must implement exactly this):
//   - All geometry passed to this interface is in LOGICAL coordinates
//     (points). Implementations map to device pixels via backingScale().
//   - With no clip pushed, geometry is relative to the top-left of the view
//     being painted.
//   - pushClip(rect) clips all subsequent drawing to `rect` AND translates
//     the coordinate origin so that rect.origin becomes (0, 0); popClip()
//     restores the previous clip and origin. Widget::paintChildren() uses
//     this to paint every child in its own local coordinate space, so
//     widget paint code always works in bounds()-relative coordinates.
//   - drawBitmap scales the source bitmap to fit destLogicalRect exactly;
//     the bitmap is straight-alpha BGRA8888 (core::Bitmap format).
//   - drawText draws the string vertically centered in rect, horizontally
//     aligned per `align`; measureText returns the laid-out extents for the
//     same font, so the two must agree.
class PaintContext {
public:
    virtual ~PaintContext() = default;

    virtual double backingScale() const = 0;

    virtual void pushClip(const core::Rect& logicalRect) = 0;
    virtual void popClip() = 0;

    virtual void fillRect(const core::Rect& rect, const Color& color) = 0;
    virtual void fillRoundedRect(const core::Rect& rect, const Color& color, double cornerRadius) = 0;
    virtual void strokeRect(const core::Rect& rect, const Color& color, double strokeWidth) = 0;
    virtual void drawLine(core::Point from, core::Point to, const Color& color, double strokeWidth) = 0;

    // Draws the bitmap into the logical rect, scaled to fit exactly.
    virtual void drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) = 0;

    virtual core::Size measureText(std::string_view text, const Font& font) const = 0;

    // Draws text vertically centered in rect, horizontally per align.
    virtual void drawText(std::string_view text, const core::Rect& rect, const Font& font,
                          const Color& color, TextAlign align) = 0;
};

} // namespace rivet::ui
