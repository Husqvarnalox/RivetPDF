// SPDX-License-Identifier: MPL-2.0
#include "app/SignaturePad.hpp"
#include "ui/Theme.hpp"

#include "ui/Path.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kMinPointDistance = 0.75; // logical points between captured samples

ui::Color toUi(const pdf::PdfColor& c) {
    return ui::Color::rgba(static_cast<double>(c.r), static_cast<double>(c.g), static_cast<double>(c.b), 1.0);
}

} // namespace

void SignaturePad::setMode(Mode mode) {
    mode_ = mode;
    drawing_ = false;
    invalidate();
}

void SignaturePad::clear() {
    strokes_.clear();
    points_ = 0;
    drawing_ = false;
    invalidate();
    if (onChanged_) onChanged_();
}

void SignaturePad::setPreview(pdf::PdfSignatureAppearancePtr signature) {
    preview_ = std::move(signature);
    invalidate();
}

bool SignaturePad::onMouse(const ui::PointerEvent& event) {
    if (mode_ != Mode::Capture) {
        return event.type == ui::PointerEventType::Down || event.type == ui::PointerEventType::Up;
    }
    const core::Rect box = bounds();
    const pdf::PdfPoint p{std::clamp(event.position.x, 0.0, box.size.width),
                          std::clamp(event.position.y, 0.0, box.size.height)};
    switch (event.type) {
    case ui::PointerEventType::Down:
        if (event.button != 1) return true;
        if (strokes_.size() >= pdf::kMaxSignatureStrokes || points_ >= pdf::kMaxSignaturePoints) return true;
        strokes_.push_back({p});
        ++points_;
        drawing_ = true;
        invalidate();
        if (onChanged_) onChanged_();
        return true;
    case ui::PointerEventType::Move: {
        if (!drawing_ || event.button == 0 || strokes_.empty()) return drawing_;
        const pdf::PdfPoint& last = strokes_.back().back();
        if (std::hypot(p.x - last.x, p.y - last.y) < kMinPointDistance) return true;
        if (points_ >= pdf::kMaxSignaturePoints) return true; // cap reached: the stroke stops growing
        strokes_.back().push_back(p);
        ++points_;
        invalidate();
        return true;
    }
    case ui::PointerEventType::Up:
        if (!drawing_) return false;
        drawing_ = false;
        if (onChanged_) onChanged_();
        return true;
    default:
        return false;
    }
}

void SignaturePad::paintSelf(ui::PaintContext& context) const {
    const core::Rect box = bounds();
    context.fillRect(box, ui::colors().editorPaper);
    context.strokeRect(box, ui::colors().editorPaperBorder, 1.0);
    // Signing line.
    const double lineY = box.size.height * 0.78;
    context.drawLine(core::Point{kInset, lineY}, core::Point{box.size.width - kInset, lineY}, ui::colors().pagePlaceholder,
                     1.0);

    if (mode_ == Mode::Preview) {
        paintPreview(context);
    } else {
        for (const auto& stroke : strokes_) {
            ui::Path path;
            path.moveTo(core::Point{stroke.front().x, stroke.front().y});
            if (stroke.size() == 1) path.lineTo(core::Point{stroke.front().x + 0.01, stroke.front().y});
            for (std::size_t i = 1; i < stroke.size(); ++i) path.lineTo(core::Point{stroke[i].x, stroke[i].y});
            context.strokePath(path, toUi(inkColor_), kPenWidth, ui::LineCap::Round, ui::LineJoin::Round);
        }
    }
    const bool blank = mode_ == Mode::Capture ? strokes_.empty() : preview_ == nullptr;
    if (blank && !hint_.empty()) {
        context.drawText(hint_, core::Rect{0.0, 0.0, box.size.width, lineY - 6.0}, ui::Font{13.0},
                         ui::colors().formFieldEditedBorder, ui::TextAlign::Center);
    }
}

void SignaturePad::paintPreview(ui::PaintContext& context) const {
    if (preview_ == nullptr) return;
    const core::Rect box = bounds();
    const double height = box.size.height;
    // The pad as a y-up page: placement box inset from the edges.
    pdf::PdfAnnotationData data;
    data.kind = pdf::PdfAnnotationKind::Signature;
    data.rect = pdf::PdfBox{kInset, kInset, std::max(kInset + 1.0, box.size.width - kInset),
                            std::max(kInset + 1.0, height - kInset)};
    data.color = inkColor_;
    data.signature = preview_;
    const pdf::PdfAppearance appearance = pdf::buildAppearance(data);
    const auto toWidget = [height](const pdf::PdfPoint& p) { return core::Point{p.x, height - p.y}; };
    for (const pdf::PdfAppearanceImage& image : appearance.images) {
        if (image.bitmap == nullptr || !image.bitmap->isValid()) continue;
        context.drawBitmap(*image.bitmap, core::Rect{image.box.left, height - image.box.top, image.box.width(),
                                                     image.box.height()});
    }
    for (const pdf::PdfAppearancePath& source : appearance.paths) {
        ui::Path path;
        for (const pdf::PdfPathSegment& seg : source.segments) {
            switch (seg.op) {
            case pdf::PdfPathSegment::Op::MoveTo: path.moveTo(toWidget(seg.p)); break;
            case pdf::PdfPathSegment::Op::LineTo: path.lineTo(toWidget(seg.p)); break;
            case pdf::PdfPathSegment::Op::CubicTo:
                path.cubicTo(toWidget(seg.c1), toWidget(seg.c2), toWidget(seg.p));
                break;
            case pdf::PdfPathSegment::Op::Close: path.close(); break;
            }
        }
        if (source.fill) context.fillPath(path, toUi(*source.fill));
        if (source.stroke) {
            context.strokePath(path, toUi(*source.stroke), static_cast<double>(source.strokeWidth),
                               ui::LineCap::Round, source.roundJoins ? ui::LineJoin::Round : ui::LineJoin::Miter);
        }
    }
}

} // namespace rivet::app
