// SPDX-License-Identifier: MPL-2.0
#include "app/AnnotationPainter.hpp"

#include "ui/Path.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::app {

namespace {

using Handle = AnnotationInteraction::Handle;

constexpr double kChromeWidth = 1.0;
constexpr double kDash = 4.0;

ui::Path toPath(const std::vector<editor::DisplayPathSegment>& segments, const core::Rect& pageRect, double zoom) {
    ui::Path path;
    for (const editor::DisplayPathSegment& segment : segments) {
        switch (segment.op) {
        case pdf::PdfPathSegment::Op::MoveTo:
            path.moveTo(AnnotationPainter::toViewport(pageRect, zoom, segment.p));
            break;
        case pdf::PdfPathSegment::Op::LineTo:
            path.lineTo(AnnotationPainter::toViewport(pageRect, zoom, segment.p));
            break;
        case pdf::PdfPathSegment::Op::CubicTo:
            path.cubicTo(AnnotationPainter::toViewport(pageRect, zoom, segment.c1),
                         AnnotationPainter::toViewport(pageRect, zoom, segment.c2),
                         AnnotationPainter::toViewport(pageRect, zoom, segment.p));
            break;
        case pdf::PdfPathSegment::Op::Close:
            path.close();
            break;
        }
    }
    return path;
}

void paintHandle(ui::PaintContext& context, core::Point center) {
    const double size = AnnotationInteraction::kHandleSize;
    const core::Rect box{center.x - size / 2.0, center.y - size / 2.0, size, size};
    context.fillRect(box, ui::Color::white());
    context.strokeRect(box, AnnotationPainter::accentColor(), kChromeWidth);
}

void paintArrowHead(ui::PaintContext& context, core::Point from, core::Point to, const ui::Color& color,
                    double width) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double length = std::hypot(dx, dy);
    if (length < 1e-6) return;
    const double ux = dx / length;
    const double uy = dy / length;
    const double head = std::clamp(std::max(8.0, width * 4.0), 8.0, std::max(8.0, length * 0.5));
    const core::Point base{to.x - ux * head, to.y - uy * head};
    const double half = head * 0.4;
    ui::Path path;
    path.moveTo(to);
    path.lineTo({base.x - uy * half, base.y + ux * half});
    path.lineTo({base.x + uy * half, base.y - ux * half});
    path.close();
    context.fillPath(path, color);
}

} // namespace

ui::Color AnnotationPainter::accentColor() { return ui::Color::rgba(0.10, 0.45, 0.95, 1.0); }

ui::Color AnnotationPainter::toColor(const pdf::PdfColor& color, double alpha) {
    return ui::Color::rgba(static_cast<double>(color.r), static_cast<double>(color.g),
                           static_cast<double>(color.b), std::clamp(alpha, 0.0, 1.0));
}

core::Point AnnotationPainter::toViewport(const core::Rect& pageRect, double zoom, core::Point displayPoint) {
    return core::Point{pageRect.origin.x + displayPoint.x * zoom, pageRect.origin.y + displayPoint.y * zoom};
}

core::Rect AnnotationPainter::toViewport(const core::Rect& pageRect, double zoom, const core::Rect& displayRect) {
    return core::Rect{pageRect.origin.x + displayRect.origin.x * zoom, pageRect.origin.y + displayRect.origin.y * zoom,
                      displayRect.size.width * zoom, displayRect.size.height * zoom};
}

void AnnotationPainter::paintAnnotation(ui::PaintContext& context, const editor::AnnotationView& view,
                                        const core::Rect& pageRect, double zoom) {
    if (!view.drawnByOverlay || !view.appearance) return;
    const editor::DisplayAppearance& appearance = *view.appearance;
    const double alpha = static_cast<double>(appearance.opacity);
    for (const editor::DisplayPath& displayPath : appearance.paths) {
        if (displayPath.segments.empty()) continue;
        const ui::Path path = toPath(displayPath.segments, pageRect, zoom);
        if (displayPath.fill) context.fillPath(path, toColor(*displayPath.fill, alpha));
        if (displayPath.stroke) {
            context.strokePath(path, toColor(*displayPath.stroke, alpha),
                               static_cast<double>(displayPath.strokeWidth) * zoom, ui::LineCap::Round,
                               displayPath.roundJoins ? ui::LineJoin::Round : ui::LineJoin::Miter);
        }
    }
    for (const editor::DisplayText& text : appearance.texts) {
        ui::Font font;
        font.size = std::max(1.0, text.box.size.height * zoom * 0.6);
        font.weight = text.bold ? ui::Font::Weight::Bold : ui::Font::Weight::Regular;
        context.drawTextInBox(text.text, toViewport(pageRect, zoom, text.box), (text.rotation / 90) % 4, font,
                              toColor(text.color, alpha));
    }
}

void AnnotationPainter::paintSelection(ui::PaintContext& context, const AnnotationInteraction::Selected& selected,
                                       const core::Rect& pageRect, double zoom) {
    const AnnotationInteraction::HitInfo& info = selected.info;
    const core::Rect bounds = toViewport(pageRect, zoom, info.bounds);
    if (info.isLine) {
        const core::Point a = toViewport(pageRect, zoom, info.lineStart);
        const core::Point b = toViewport(pageRect, zoom, info.lineEnd);
        context.strokeDashedRect(bounds, accentColor(), kChromeWidth, kDash);
        if (info.canResize) {
            paintHandle(context, a);
            paintHandle(context, b);
        }
        return;
    }
    context.strokeDashedRect(bounds, accentColor(), kChromeWidth, kDash);
    if (!info.canResize) return;
    for (const Handle handle : {Handle::TopLeft, Handle::Top, Handle::TopRight, Handle::Right, Handle::BottomRight,
                                Handle::Bottom, Handle::BottomLeft, Handle::Left}) {
        paintHandle(context, AnnotationInteraction::handlePoint(bounds, handle));
    }
}

void AnnotationPainter::paintPreview(ui::PaintContext& context, const AnnotationInteraction::Preview& preview,
                                     const editor::AnnotationStyle& style, const core::Rect& pageRect, double zoom) {
    using Kind = AnnotationInteraction::Preview::Kind;
    if (preview.kind == Kind::None) return;
    const double width = std::max(1.0, static_cast<double>(style.borderWidth) * zoom);
    const double alpha = static_cast<double>(style.opacity);
    const ui::Color color = toColor(style.color, alpha);

    if (preview.editing) {
        // The edited annotation keeps its own look; only an outline shows
        // where it will land.
        if (preview.kind == Kind::Bounds) {
            context.strokeDashedRect(toViewport(pageRect, zoom, preview.rect), accentColor(), kChromeWidth, kDash);
        } else if (preview.kind == Kind::Line) {
            context.drawLine(toViewport(pageRect, zoom, preview.a), toViewport(pageRect, zoom, preview.b),
                             accentColor(), kChromeWidth);
        }
        return;
    }

    switch (preview.kind) {
    case Kind::Ink: {
        ui::Path path;
        bool first = true;
        for (const core::Point& point : preview.points) {
            const core::Point p = toViewport(pageRect, zoom, point);
            if (first) path.moveTo(p);
            else path.lineTo(p);
            first = false;
        }
        context.strokePath(path, color, width, ui::LineCap::Round, ui::LineJoin::Round);
        break;
    }
    case Kind::Rect: {
        const core::Rect box = toViewport(pageRect, zoom, preview.rect);
        if (preview.tool == AnnotationTool::Stamp) {
            context.strokeDashedRect(box, color, kChromeWidth, kDash);
            break;
        }
        ui::Path path;
        if (preview.tool == AnnotationTool::Ellipse) path.addEllipse(box);
        else path.addRect(box);
        if (style.interiorColor) context.fillPath(path, toColor(*style.interiorColor, alpha));
        context.strokePath(path, color, width, ui::LineCap::Butt, ui::LineJoin::Miter);
        break;
    }
    case Kind::Line: {
        const core::Point a = toViewport(pageRect, zoom, preview.a);
        const core::Point b = toViewport(pageRect, zoom, preview.b);
        context.drawLine(a, b, color, width);
        if (preview.tool == AnnotationTool::Arrow) paintArrowHead(context, a, b, color, width);
        break;
    }
    case Kind::Bounds:
    case Kind::None:
        break;
    }
}

} // namespace rivet::app
