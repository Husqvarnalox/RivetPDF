// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "WinCommon.hpp"

#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <d2d1.h>

#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace rivet::platform {

// Direct2D painter behind ui::PaintContext. The render target's DPI must be
// 96 * backingScale so that one D2D DIP equals one Rivet logical point (the
// analogue of the Retina scale AppKit bakes into a flipped view's CTM); the
// target's transform at construction is the identity in that DIP space.
// Text goes through text::layoutLine() (DirectWrite), so measureText() and
// drawText() agree by construction. Main thread only; lives for one frame.
class WindowsPaintContext final : public ui::PaintContext {
public:
    WindowsPaintContext(ID2D1RenderTarget* target, double backingScale);
    ~WindowsPaintContext() override;
    WindowsPaintContext(const WindowsPaintContext&) = delete;
    WindowsPaintContext& operator=(const WindowsPaintContext&) = delete;

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
    ID2D1SolidColorBrush* brush(const ui::Color& color);
    ID2D1StrokeStyle* dashStyle(double ratio);
    ID2D1StrokeStyle* pathStyle(ui::LineCap cap, ui::LineJoin join);
    win::ComPtr<ID2D1PathGeometry> buildPath(const ui::Path& path);

    ID2D1RenderTarget* target_;
    win::ComPtr<ID2D1Factory> factory_;
    win::ComPtr<ID2D1SolidColorBrush> brush_;
    double backingScale_;
    std::vector<D2D1_MATRIX_3X2_F> clipStack_;
    std::map<double, win::ComPtr<ID2D1StrokeStyle>> dashStyles_;
    std::map<std::pair<int, int>, win::ComPtr<ID2D1StrokeStyle>> pathStyles_;
};

} // namespace rivet::platform
