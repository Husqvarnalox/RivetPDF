#pragma once

#include "core/Bitmap.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <CoreGraphics/CoreGraphics.h>

namespace rivet::platform {

// CoreGraphics painter behind ui::PaintContext. `context` must be the
// CGContext of a FLIPPED NSView ([NSGraphicsContext currentContext].CGContext
// inside drawRect), so Rivet's logical coordinates (top-left origin, y-down,
// in points) map 1:1 onto CG points; Retina backing is handled by the system
// scale already baked into the CTM.
class MacosPaintContext final : public rivet::ui::PaintContext {
public:
    MacosPaintContext(CGContextRef context, double backingScale);

    double backingScale() const override { return backingScale_; }

    void pushClip(const core::Rect& logicalRect) override;
    void popClip() override;

    void fillRect(const core::Rect& rect, const rivet::ui::Color& color) override;
    void fillRoundedRect(const core::Rect& rect, const rivet::ui::Color& color,
                         double cornerRadius) override;
    void strokeRect(const core::Rect& rect, const rivet::ui::Color& color,
                    double strokeWidth) override;
    void drawLine(core::Point from, core::Point to, const rivet::ui::Color& color,
                  double strokeWidth) override;

    void drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) override;

    core::Size measureText(std::string_view text, const rivet::ui::Font& font) const override;
    void drawText(std::string_view text, const core::Rect& rect, const rivet::ui::Font& font,
                  const rivet::ui::Color& color, rivet::ui::TextAlign align) override;

private:
    CGContextRef context_;
    double backingScale_;
};

} // namespace rivet::platform
