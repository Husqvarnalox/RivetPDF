// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Bitmap.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <cairo.h>

#include <string_view>

namespace rivet::platform {

// Cairo painter behind ui::PaintContext. `cr` must already map Rivet's
// logical coordinates (top-left origin, y-down, points) onto device pixels,
// i.e. carry cairo_scale(backingScale, backingScale) — the analogue of the
// Retina scale AppKit bakes into a flipped view's CTM. Text goes through
// LinuxTextShaper (fontconfig + HarfBuzz + cairo-ft), so measureText() and
// drawText() agree by construction.
class LinuxPaintContext final : public ui::PaintContext {
public:
    LinuxPaintContext(cairo_t* cr, double backingScale);

    double backingScale() const override { return backingScale_; }

    void pushClip(const core::Rect& logicalRect) override;
    void popClip() override;

    void fillRect(const core::Rect& rect, const ui::Color& color) override;
    void fillRoundedRect(const core::Rect& rect, const ui::Color& color, double cornerRadius) override;
    void strokeRect(const core::Rect& rect, const ui::Color& color, double strokeWidth) override;
    void drawLine(core::Point from, core::Point to, const ui::Color& color, double strokeWidth) override;
    void strokeDashedRect(const core::Rect& rect, const ui::Color& color, double strokeWidth,
                          double dash) override;
    void fillPath(const ui::Path& path, const ui::Color& color) override;
    void strokePath(const ui::Path& path, const ui::Color& color, double strokeWidth, ui::LineCap cap,
                    ui::LineJoin join) override;

    void drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) override;

    core::Size measureText(std::string_view text, const ui::Font& font) const override;
    void drawText(std::string_view text, const core::Rect& rect, const ui::Font& font,
                  const ui::Color& color, ui::TextAlign align) override;
    void drawTextInBox(std::string_view text, const core::Rect& box, int quarterTurns,
                       const ui::Font& font, const ui::Color& color) override;

private:
    cairo_t* cr_;
    double backingScale_;
};

// Straight-alpha BGRA8888 rows -> a new premultiplied CAIRO_FORMAT_ARGB32
// image surface (caller destroys). Returns nullptr on invalid geometry or
// allocation failure. Shared with the print path.
cairo_surface_t* createSurfaceFromStraightBgra(const unsigned char* pixels, int width, int height,
                                               std::size_t stride);

} // namespace rivet::platform
