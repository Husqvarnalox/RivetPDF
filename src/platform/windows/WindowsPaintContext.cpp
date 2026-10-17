// SPDX-License-Identifier: MPL-2.0
#include "WindowsPaintContext.hpp"

#include "WindowsTextShaper.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace rivet::platform {

namespace {

D2D1_RECT_F toRect(const core::Rect& rect) {
    return D2D1_RECT_F{static_cast<FLOAT>(rect.minX()), static_cast<FLOAT>(rect.minY()),
                       static_cast<FLOAT>(rect.maxX()), static_cast<FLOAT>(rect.maxY())};
}

D2D1_POINT_2F toPoint(double x, double y) {
    return D2D1_POINT_2F{static_cast<FLOAT>(x), static_cast<FLOAT>(y)};
}

// Row-vector convention (D2D): the result applies `first`, then `second`.
D2D1_MATRIX_3X2_F multiply(const D2D1_MATRIX_3X2_F& first, const D2D1_MATRIX_3X2_F& second) {
    D2D1_MATRIX_3X2_F r{};
    r._11 = first._11 * second._11 + first._12 * second._21;
    r._12 = first._11 * second._12 + first._12 * second._22;
    r._21 = first._21 * second._11 + first._22 * second._21;
    r._22 = first._21 * second._12 + first._22 * second._22;
    r._31 = first._31 * second._11 + first._32 * second._21 + second._31;
    r._32 = first._31 * second._12 + first._32 * second._22 + second._32;
    return r;
}

D2D1_MATRIX_3X2_F translation(double x, double y) {
    D2D1_MATRIX_3X2_F m{};
    m._11 = 1.0F;
    m._22 = 1.0F;
    m._31 = static_cast<FLOAT>(x);
    m._32 = static_cast<FLOAT>(y);
    return m;
}

D2D1_MATRIX_3X2_F scaling(double s) {
    D2D1_MATRIX_3X2_F m{};
    m._11 = static_cast<FLOAT>(s);
    m._22 = static_cast<FLOAT>(s);
    return m;
}

// Clockwise on screen (y-down) by `turns` quarter turns.
D2D1_MATRIX_3X2_F quarterRotation(int turns) {
    static constexpr FLOAT kCos[] = {1.0F, 0.0F, -1.0F, 0.0F};
    static constexpr FLOAT kSin[] = {0.0F, 1.0F, 0.0F, -1.0F};
    D2D1_MATRIX_3X2_F m{};
    m._11 = kCos[turns];
    m._12 = kSin[turns];
    m._21 = -kSin[turns];
    m._22 = kCos[turns];
    return m;
}

bool isOpaque(const unsigned char* pixels, std::uint32_t width, std::uint32_t height, std::size_t stride) {
    for (std::uint32_t y = 0; y < height; ++y) {
        const unsigned char* row = pixels + static_cast<std::size_t>(y) * stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            if (row[static_cast<std::size_t>(x) * 4 + 3] != 255) return false;
        }
    }
    return true;
}

} // namespace

WindowsPaintContext::WindowsPaintContext(ID2D1RenderTarget* target, double backingScale)
    : target_(target), backingScale_(backingScale > 0.0 ? backingScale : 1.0) {
    target_->GetFactory(factory_.put());
    const D2D1_COLOR_F black{0.0F, 0.0F, 0.0F, 1.0F};
    target_->CreateSolidColorBrush(black, brush_.put());
}

WindowsPaintContext::~WindowsPaintContext() {
    // Unbalanced pushes (a widget bug) must not leak into EndDraw.
    while (!clipStack_.empty()) popClip();
}

ID2D1SolidColorBrush* WindowsPaintContext::brush(const ui::Color& color) {
    if (brush_) {
        // Straight alpha: D2D brushes take a non-premultiplied color.
        brush_->SetColor(D2D1_COLOR_F{static_cast<FLOAT>(color.r), static_cast<FLOAT>(color.g),
                                      static_cast<FLOAT>(color.b), static_cast<FLOAT>(color.a)});
    }
    return brush_.get();
}

void WindowsPaintContext::pushClip(const core::Rect& logicalRect) {
    // Clip in the CURRENT space, then move the origin to the rect's origin
    // (the same order as the CoreGraphics implementation).
    D2D1_MATRIX_3X2_F current{};
    target_->GetTransform(&current);
    clipStack_.push_back(current);
    target_->PushAxisAlignedClip(toRect(logicalRect), D2D1_ANTIALIAS_MODE_ALIASED);
    target_->SetTransform(multiply(translation(logicalRect.minX(), logicalRect.minY()), current));
}

void WindowsPaintContext::popClip() {
    if (clipStack_.empty()) return;
    target_->PopAxisAlignedClip();
    target_->SetTransform(clipStack_.back());
    clipStack_.pop_back();
}

void WindowsPaintContext::fillRect(const core::Rect& rect, const ui::Color& color) {
    if (rect.isEmpty()) return;
    if (ID2D1SolidColorBrush* b = brush(color)) target_->FillRectangle(toRect(rect), b);
}

void WindowsPaintContext::fillRoundedRect(const core::Rect& rect, const ui::Color& color, double cornerRadius) {
    if (rect.isEmpty()) return;
    const double radius = std::clamp(cornerRadius, 0.0, std::min(rect.size.width, rect.size.height) / 2.0);
    ID2D1SolidColorBrush* b = brush(color);
    if (b == nullptr) return;
    if (radius <= 0.0) {
        target_->FillRectangle(toRect(rect), b);
        return;
    }
    const D2D1_ROUNDED_RECT rounded{toRect(rect), static_cast<FLOAT>(radius), static_cast<FLOAT>(radius)};
    target_->FillRoundedRectangle(rounded, b);
}

void WindowsPaintContext::strokeRect(const core::Rect& rect, const ui::Color& color, double strokeWidth) {
    if (rect.isEmpty()) return;
    if (ID2D1SolidColorBrush* b = brush(color)) {
        target_->DrawRectangle(toRect(rect), b, static_cast<FLOAT>(strokeWidth), pathStyle(ui::LineCap::Butt,
                                                                                            ui::LineJoin::Miter));
    }
}

void WindowsPaintContext::drawLine(core::Point from, core::Point to, const ui::Color& color, double strokeWidth) {
    if (ID2D1SolidColorBrush* b = brush(color)) {
        target_->DrawLine(toPoint(from.x, from.y), toPoint(to.x, to.y), b, static_cast<FLOAT>(strokeWidth));
    }
}

ID2D1StrokeStyle* WindowsPaintContext::dashStyle(double ratio) {
    auto& slot = dashStyles_[ratio];
    if (!slot && factory_) {
        D2D1_STROKE_STYLE_PROPERTIES properties{};
        properties.startCap = D2D1_CAP_STYLE_FLAT;
        properties.endCap = D2D1_CAP_STYLE_FLAT;
        properties.dashCap = D2D1_CAP_STYLE_FLAT;
        properties.lineJoin = D2D1_LINE_JOIN_MITER;
        properties.miterLimit = 10.0F;
        properties.dashStyle = D2D1_DASH_STYLE_CUSTOM;
        properties.dashOffset = 0.0F;
        const FLOAT dashes[] = {static_cast<FLOAT>(ratio), static_cast<FLOAT>(ratio)};
        factory_->CreateStrokeStyle(properties, dashes, 2, slot.put());
    }
    return slot.get();
}

ID2D1StrokeStyle* WindowsPaintContext::pathStyle(ui::LineCap cap, ui::LineJoin join) {
    const bool roundCap = cap == ui::LineCap::Round;
    const bool roundJoin = join == ui::LineJoin::Round;
    auto& slot = pathStyles_[{roundCap ? 1 : 0, roundJoin ? 1 : 0}];
    if (!slot && factory_) {
        D2D1_STROKE_STYLE_PROPERTIES properties{};
        properties.startCap = roundCap ? D2D1_CAP_STYLE_ROUND : D2D1_CAP_STYLE_FLAT;
        properties.endCap = properties.startCap;
        properties.dashCap = properties.startCap;
        properties.lineJoin = roundJoin ? D2D1_LINE_JOIN_ROUND : D2D1_LINE_JOIN_MITER;
        properties.miterLimit = 10.0F; // CoreGraphics / cairo default
        properties.dashStyle = D2D1_DASH_STYLE_SOLID;
        factory_->CreateStrokeStyle(properties, nullptr, 0, slot.put());
    }
    return slot.get();
}

void WindowsPaintContext::strokeDashedRect(const core::Rect& rect, const ui::Color& color, double strokeWidth,
                                           double dash) {
    if (rect.isEmpty() || !(strokeWidth > 0.0)) return;
    ID2D1SolidColorBrush* b = brush(color);
    if (b == nullptr) return;
    // D2D dash lengths are in multiples of the stroke width.
    ID2D1StrokeStyle* style = dash > 0.0 ? dashStyle(dash / strokeWidth) : nullptr;
    target_->DrawRectangle(toRect(rect), b, static_cast<FLOAT>(strokeWidth), style);
}

win::ComPtr<ID2D1PathGeometry> WindowsPaintContext::buildPath(const ui::Path& path) {
    win::ComPtr<ID2D1PathGeometry> geometry;
    if (!factory_ || FAILED(factory_->CreatePathGeometry(geometry.put()))) return {};
    win::ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.put()))) return {};
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);

    using Op = ui::PathSegment::Op;
    bool open = false;          // a figure is being built
    bool closedCurrent = false; // after Close: the current point is the subpath start
    D2D1_POINT_2F start{};
    const auto begin = [&](D2D1_POINT_2F at) {
        if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->BeginFigure(at, D2D1_FIGURE_BEGIN_FILLED);
        start = at;
        open = true;
        closedCurrent = false;
    };
    // Like CoreGraphics/cairo: drawing without a current point starts a
    // subpath at `fallback`; after Close it continues from the subpath start.
    const auto ensureOpen = [&](D2D1_POINT_2F fallback) {
        if (!open) begin(closedCurrent ? start : fallback);
    };
    for (const ui::PathSegment& segment : path.segments) {
        switch (segment.op) {
        case Op::MoveTo: begin(toPoint(segment.p.x, segment.p.y)); break;
        case Op::LineTo: {
            const D2D1_POINT_2F to = toPoint(segment.p.x, segment.p.y);
            const bool fresh = !open && !closedCurrent;
            ensureOpen(to);
            if (!fresh) sink->AddLine(to);
            break;
        }
        case Op::CubicTo:
            ensureOpen(toPoint(segment.c1.x, segment.c1.y));
            sink->AddBezier(D2D1_BEZIER_SEGMENT{toPoint(segment.c1.x, segment.c1.y),
                                                toPoint(segment.c2.x, segment.c2.y),
                                                toPoint(segment.p.x, segment.p.y)});
            break;
        case Op::Close:
            if (open) {
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                open = false;
                closedCurrent = true;
            }
            break;
        }
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return {};
    return geometry;
}

void WindowsPaintContext::fillPath(const ui::Path& path, const ui::Color& color) {
    if (path.empty()) return;
    // D2D fills open figures as if closed (the nonzero rule is set in buildPath).
    const auto geometry = buildPath(path);
    ID2D1SolidColorBrush* b = brush(color);
    if (geometry && b != nullptr) target_->FillGeometry(geometry.get(), b, nullptr);
}

void WindowsPaintContext::strokePath(const ui::Path& path, const ui::Color& color, double strokeWidth,
                                     ui::LineCap cap, ui::LineJoin join) {
    if (path.empty()) return;
    const auto geometry = buildPath(path);
    ID2D1SolidColorBrush* b = brush(color);
    if (geometry && b != nullptr) {
        target_->DrawGeometry(geometry.get(), b, static_cast<FLOAT>(strokeWidth), pathStyle(cap, join));
    }
}

void WindowsPaintContext::drawBitmap(const core::Bitmap& bitmap, const core::Rect& destLogicalRect) {
    if (!bitmap.isValid() || bitmap.width() == 0 || bitmap.height() == 0 || destLogicalRect.isEmpty()) return;
    const UINT32 maxSide = target_->GetMaximumBitmapSize();
    if (bitmap.width() > maxSide || bitmap.height() > maxSide) return;

    const auto* pixels = reinterpret_cast<const unsigned char*>(bitmap.data());
    const std::size_t stride = bitmap.stride();
    if (stride > 0xFFFFFFFFu) return;

    D2D1_BITMAP_PROPERTIES properties{};
    properties.dpiX = 96.0F;
    properties.dpiY = 96.0F;
    properties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    win::ComPtr<ID2D1Bitmap> d2dBitmap;
    const D2D1_SIZE_U size{bitmap.width(), bitmap.height()};
    if (isOpaque(pixels, bitmap.width(), bitmap.height(), stride)) {
        // Opaque tiles (pages are filled white): straight == premultiplied,
        // the rows upload as-is.
        properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        target_->CreateBitmap(size, pixels, static_cast<UINT32>(stride), properties, d2dBitmap.put());
    } else {
        properties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        const std::size_t rowBytes = static_cast<std::size_t>(bitmap.width()) * 4;
        std::vector<unsigned char> premultiplied(rowBytes * bitmap.height());
        for (std::uint32_t y = 0; y < bitmap.height(); ++y) {
            const unsigned char* src = pixels + static_cast<std::size_t>(y) * stride;
            unsigned char* dst = premultiplied.data() + static_cast<std::size_t>(y) * rowBytes;
            for (std::uint32_t x = 0; x < bitmap.width(); ++x, src += 4, dst += 4) {
                const unsigned a = src[3];
                dst[0] = static_cast<unsigned char>((src[0] * a + 127u) / 255u);
                dst[1] = static_cast<unsigned char>((src[1] * a + 127u) / 255u);
                dst[2] = static_cast<unsigned char>((src[2] * a + 127u) / 255u);
                dst[3] = static_cast<unsigned char>(a);
            }
        }
        target_->CreateBitmap(size, premultiplied.data(), static_cast<UINT32>(rowBytes), properties,
                              d2dBitmap.put());
    }
    if (!d2dBitmap) return;
    const D2D1_RECT_F dest = toRect(destLogicalRect);
    // Aliased edges: adjacent page tiles (rects snapped to device pixels by
    // the viewport) must not blend with what lies below at their shared
    // edge, or thin seams show through solid fills (QA #18).
    const D2D1_ANTIALIAS_MODE previous = target_->GetAntialiasMode();
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    target_->DrawBitmap(d2dBitmap.get(), &dest, 1.0F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
    target_->SetAntialiasMode(previous);
}

core::Size WindowsPaintContext::measureText(std::string_view text, const ui::Font& font) const {
    if (text.empty()) return core::Size{0.0, 0.0};
    const auto line = text::layoutLine(text, font);
    return core::Size{line->width, line->ascent + line->descent};
}

void WindowsPaintContext::drawText(std::string_view text, const core::Rect& rect, const ui::Font& font,
                                   const ui::Color& color, ui::TextAlign align) {
    if (text.empty() || rect.isEmpty()) return;
    const auto line = text::layoutLine(text, font);
    if (!line->layout) return;

    double x = rect.minX();
    switch (align) {
    case ui::TextAlign::Left: break;
    case ui::TextAlign::Center: x = rect.minX() + (rect.size.width - line->width) / 2.0; break;
    case ui::TextAlign::Right: x = rect.maxX() - line->width; break;
    }
    // Same vertical centering as the CoreText path.
    const double lineBox = line->ascent + line->descent;
    const double baseline = rect.minY() + (rect.size.height - lineBox) / 2.0 + line->ascent;

    ID2D1SolidColorBrush* b = brush(color);
    if (b == nullptr) return;
    target_->DrawTextLayout(toPoint(x, baseline - line->layoutBaseline), line->layout.get(), b,
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void WindowsPaintContext::drawTextInBox(std::string_view text, const core::Rect& box, int quarterTurns,
                                        const ui::Font& font, const ui::Color& color) {
    if (text.empty() || box.isEmpty()) return;

    ui::Font boldFont = font;
    boldFont.weight = ui::Font::Weight::Bold;
    const auto line = text::layoutLine(text, boldFont);
    const double lineHeight = line->ascent + line->descent;
    if (line->width <= 0.0 || lineHeight <= 0.0) return;

    const int turns = ((quarterTurns % 4) + 4) % 4;
    const bool swapped = (turns % 2) != 0;
    const double availWidth = (swapped ? box.size.height : box.size.width) * 0.9;
    const double availHeight = (swapped ? box.size.width : box.size.height) * 0.9;
    const double scale = std::fmin(availWidth / line->width, availHeight / lineHeight);
    if (!(scale > 0.0)) return;

    const core::Point center = box.center();
    D2D1_MATRIX_3X2_F saved{};
    target_->GetTransform(&saved);
    // Scale, then rotate (clockwise in y-down space), then move to the box
    // center, then the current space.
    target_->SetTransform(
        multiply(multiply(multiply(scaling(scale), quarterRotation(turns)), translation(center.x, center.y)),
                 saved));
    drawText(text,
             core::Rect{core::Point{-line->width / 2.0, -lineHeight / 2.0}, core::Size{line->width, lineHeight}},
             boldFont, color, ui::TextAlign::Left);
    target_->SetTransform(saved);
}

} // namespace rivet::platform
