// SPDX-License-Identifier: MPL-2.0
#include "app/AnnotationLayer.hpp"

#include "app/AnnotationPainter.hpp"

#include <chrono>
#include <cmath>

namespace rivet::app {

namespace {

using Intent = AnnotationInteraction::Intent;

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool isPrimary(const ui::PointerEvent& event) { return event.button == 1; }

} // namespace

AnnotationLayer::AnnotationLayer(AnnotationLayerClient& client, Clock clock)
    : client_(client), clock_(clock ? std::move(clock) : Clock(steadySeconds)) {}

int AnnotationLayer::clickCountFor(const ui::PointerEvent& event) {
    const double now = clock_();
    const bool close = std::hypot(event.position.x - lastDownPoint_.x, event.position.y - lastDownPoint_.y) <=
                       kDoubleClickDistance;
    const bool quick = now - lastDownTime_ <= kDoubleClickSeconds;
    lastClickCount_ = (quick && close) ? lastClickCount_ + 1 : 1;
    lastDownTime_ = now;
    lastDownPoint_ = event.position;
    return lastClickCount_;
}

AnnotationInteraction::PointerInput AnnotationLayer::inputFor(const ui::ViewportToolHost& host, std::size_t page,
                                                              core::Point viewportPoint,
                                                              const ui::PointerEvent& event, int clickCount) const {
    AnnotationInteraction::PointerInput input;
    input.page = page;
    input.shift = event.modifiers.shift;
    input.clickCount = clickCount;
    const double zoom = host.zoomFactor();
    input.zoom = zoom > 0.0 ? zoom : 1.0;
    if (const std::optional<core::Rect> rect = host.pageRectInViewport(page); rect.has_value()) {
        input.point = core::Point{(viewportPoint.x - rect->origin.x) / input.zoom,
                                  (viewportPoint.y - rect->origin.y) / input.zoom};
        input.pageSize = core::Size{rect->size.width / input.zoom, rect->size.height / input.zoom};
    }
    return input;
}

bool AnnotationLayer::onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    if (!inputEnabled_) return false;
    AnnotationInteraction& machine = client_.interaction();
    switch (event.type) {
    case ui::PointerEventType::Down: {
        if (!isPrimary(event) || event.modifiers.command || event.modifiers.control || event.modifiers.option) {
            return false;
        }
        client_.pointerPressed();
        machine.setSelection(client_.currentSelection());
        const std::optional<std::pair<std::size_t, core::Point>> hit = host.pageAt(event.position);
        if (!hit.has_value()) {
            // Between pages: deselect, the viewport keeps the event.
            if (machine.selection().has_value()) {
                Intent clear;
                clear.kind = Intent::Kind::ClearSelection;
                clear.consumed = false;
                machine.setSelection(std::nullopt);
                client_.applyIntent(clear);
            }
            return false;
        }
        const int clicks = clickCountFor(event);
        const Intent intent = machine.pointerDown(inputFor(host, hit->first, event.position, event, clicks));
        client_.applyIntent(intent);
        host.requestRepaint();
        return intent.consumed;
    }
    case ui::PointerEventType::Move: {
        if (!machine.gestureActive()) return false;
        const std::optional<std::size_t> page = machine.gesturePage();
        if (!page.has_value()) return false;
        const Intent intent = machine.pointerMove(inputFor(host, *page, event.position, event, 1));
        host.requestRepaint();
        return intent.consumed;
    }
    case ui::PointerEventType::Up: {
        if (!machine.gestureActive()) return false;
        const std::optional<std::size_t> page = machine.gesturePage();
        if (!page.has_value()) return false;
        const Intent intent = machine.pointerUp(inputFor(host, *page, event.position, event, 1));
        client_.applyIntent(intent);
        host.requestRepaint();
        return intent.consumed;
    }
    default:
        return false;
    }
}

void AnnotationLayer::afterMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    if (!inputEnabled_) return;
    if (event.type != ui::PointerEventType::Up || !isPrimary(event)) return;
    const Intent intent = client_.interaction().afterUnconsumedUp(client_.textSelectionNonEmpty());
    if (intent.kind == Intent::Kind::ConvertTextSelection) {
        client_.applyIntent(intent);
        host.requestRepaint();
    }
}

bool AnnotationLayer::onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) {
    if (!inputEnabled_) return false;
    if (event.modifiers.command || event.modifiers.control || event.modifiers.option) return false;
    AnnotationInteraction::KeyInput input;
    switch (event.key) {
    case ui::Key::Escape: input = AnnotationInteraction::KeyInput::Escape; break;
    case ui::Key::Delete: input = AnnotationInteraction::KeyInput::Delete; break;
    case ui::Key::Backspace: input = AnnotationInteraction::KeyInput::Backspace; break;
    case ui::Key::Enter: input = AnnotationInteraction::KeyInput::Enter; break;
    default: return false;
    }
    AnnotationInteraction& machine = client_.interaction();
    machine.setSelection(client_.currentSelection());
    const Intent intent = machine.key(input);
    client_.applyIntent(intent);
    host.requestRepaint();
    return intent.consumed;
}

void AnnotationLayer::paintPage(const ui::ViewportToolHost& host, std::size_t pageIndex,
                                const core::Rect& pageRectInViewport, ui::PaintContext& context) const {
    const auto annotations = client_.pageAnnotations(pageIndex);
    if (!annotations) return;
    const double zoom = host.zoomFactor();
    for (const editor::AnnotationView& view : *annotations) {
        AnnotationPainter::paintAnnotation(context, view, pageRectInViewport, zoom);
    }
}

void AnnotationLayer::paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const {
    const double zoom = host.zoomFactor();
    context.pushClip(host.viewportBounds());
    const AnnotationInteraction::Preview preview = client_.interaction().preview();
    const std::optional<AnnotationInteraction::Selected> selected = client_.currentSelection();
    // While the selection itself is being moved/resized only the preview
    // outline shows (the handles would lie about the geometry).
    if (selected.has_value() && !(preview.kind != AnnotationInteraction::Preview::Kind::None && preview.editing)) {
        if (const std::optional<core::Rect> pageRect = host.pageRectInViewport(selected->page);
            pageRect.has_value()) {
            AnnotationPainter::paintSelection(context, *selected, *pageRect, zoom);
        }
    }
    if (preview.kind != AnnotationInteraction::Preview::Kind::None) {
        if (const std::optional<core::Rect> pageRect = host.pageRectInViewport(preview.page); pageRect.has_value()) {
            AnnotationPainter::paintPreview(context, preview, client_.previewStyle(), *pageRect, zoom);
        }
    }
    context.popClip();
}

} // namespace rivet::app
