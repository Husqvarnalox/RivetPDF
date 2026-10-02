// SPDX-License-Identifier: MPL-2.0
#include "app/ContentLayer.hpp"

#include "app/AnnotationPainter.hpp"

#include <chrono>
#include <cmath>
#include <utility>

namespace rivet::app {

namespace {

using Intent = ContentInteraction::Intent;

constexpr double kChromeWidth = 1.5;
constexpr double kDash = 4.0;

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool isPrimary(const ui::PointerEvent& event) { return event.button == 1; }

bool noPlainModifiers(const ui::PointerEvent& event) {
    return !(event.modifiers.command || event.modifiers.control || event.modifiers.option);
}

core::Point toViewport(const core::Rect& pageRect, double zoom, core::Point p) {
    return AnnotationPainter::toViewport(pageRect, zoom, p);
}

void paintHandle(ui::PaintContext& context, core::Point center) {
    const double size = AnnotationInteraction::kHandleSize;
    const core::Rect box{center.x - size / 2.0, center.y - size / 2.0, size, size};
    context.fillRect(box, ui::Color::white());
    context.strokeRect(box, AnnotationPainter::accentColor(), kChromeWidth);
}

void strokeQuad(ui::PaintContext& context, const core::Rect& pageRect, double zoom,
                const std::array<core::Point, 4>& quad, const ui::Color& color) {
    // quad[0..3] is a closed loop in either winding.
    for (std::size_t i = 0; i < 4; ++i) {
        const core::Point a = toViewport(pageRect, zoom, quad[i]);
        const core::Point b = toViewport(pageRect, zoom, quad[(i + 1) % 4]);
        context.drawLine(a, b, color, kChromeWidth);
    }
}

// Text blocks are outlined by their (possibly rotated) quad, images and
// paths by their dashed axis-aligned bounds.
void paintOutline(ui::PaintContext& context, const core::Rect& pageRect, double zoom,
                  const ContentInteraction::HitInfo& info, const ui::Color& color) {
    if (info.kind == ContentInteraction::Kind::Text) {
        strokeQuad(context, pageRect, zoom, info.quad, color);
    } else {
        context.strokeDashedRect(AnnotationPainter::toViewport(pageRect, zoom, info.bounds), color, kChromeWidth,
                                 kDash);
    }
}

} // namespace

ContentLayer::ContentLayer(ContentLayerClient& client, Clock clock)
    : client_(client), clock_(clock ? std::move(clock) : Clock(steadySeconds)) {}

int ContentLayer::clickCountFor(const ui::PointerEvent& event) {
    const double now = clock_();
    const bool close = std::hypot(event.position.x - lastDownPoint_.x, event.position.y - lastDownPoint_.y) <=
                       kDoubleClickDistance;
    const bool quick = now - lastDownTime_ <= kDoubleClickSeconds;
    lastClickCount_ = (quick && close) ? lastClickCount_ + 1 : 1;
    lastDownTime_ = now;
    lastDownPoint_ = event.position;
    return lastClickCount_;
}

ContentInteraction::PointerInput ContentLayer::inputFor(const ui::ViewportToolHost& host, std::size_t page,
                                                        core::Point viewportPoint, const ui::PointerEvent& event,
                                                        int clickCount) const {
    ContentInteraction::PointerInput input;
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

bool ContentLayer::onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    if (!client_.active()) return false;
    ContentInteraction& machine = client_.interaction();
    switch (event.type) {
    case ui::PointerEventType::Down: {
        if (!isPrimary(event) || !noPlainModifiers(event)) return false;
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
                client_.applyIntent(clear, false);
            }
            return false;
        }
        const int clicks = clickCountFor(event);
        const Intent intent = machine.pointerDown(inputFor(host, hit->first, event.position, event, clicks));
        client_.applyIntent(intent, event.modifiers.shift);
        host.requestRepaint();
        return intent.consumed;
    }
    case ui::PointerEventType::Move: {
        if (machine.gestureActive()) {
            const std::optional<std::size_t> page = machine.gesturePage();
            if (!page.has_value()) return false;
            const Intent intent = machine.pointerMove(inputFor(host, *page, event.position, event, 1));
            host.requestRepaint();
            return intent.consumed;
        }
        // Hover.
        const std::optional<std::pair<std::size_t, core::Point>> hit = host.pageAt(event.position);
        const bool had = machine.hover().has_value();
        if (!hit.has_value()) {
            machine.clearHover();
            if (had) host.requestRepaint();
            return false;
        }
        const std::optional<ContentInteraction::Selected> before = machine.hover();
        machine.pointerMove(inputFor(host, hit->first, event.position, event, 1));
        const std::optional<ContentInteraction::Selected>& after = machine.hover();
        const bool changed = before.has_value() != after.has_value() ||
                             (before.has_value() && (before->info.id != after->info.id ||
                                                     before->info.isBlock != after->info.isBlock ||
                                                     before->page != after->page));
        if (changed) host.requestRepaint();
        return false;
    }
    case ui::PointerEventType::Up: {
        if (!machine.gestureActive()) return false;
        const std::optional<std::size_t> page = machine.gesturePage();
        if (!page.has_value()) return false;
        const Intent intent = machine.pointerUp(inputFor(host, *page, event.position, event, 1));
        client_.applyIntent(intent, event.modifiers.shift);
        host.requestRepaint();
        return true;
    }
    case ui::PointerEventType::Exited:
        if (machine.hover().has_value()) {
            machine.clearHover();
            host.requestRepaint();
        }
        return false;
    default:
        return false;
    }
}

void ContentLayer::afterMouse(ui::ViewportToolHost&, const ui::PointerEvent&) {}

bool ContentLayer::onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) {
    if (!client_.active()) return false;
    if (event.modifiers.command || event.modifiers.control || event.modifiers.option) return false;
    ContentInteraction::KeyInput input;
    switch (event.key) {
    case ui::Key::Escape: input = ContentInteraction::KeyInput::Escape; break;
    case ui::Key::Delete: input = ContentInteraction::KeyInput::Delete; break;
    case ui::Key::Backspace: input = ContentInteraction::KeyInput::Backspace; break;
    case ui::Key::Enter: input = ContentInteraction::KeyInput::Enter; break;
    case ui::Key::Left: input = ContentInteraction::KeyInput::Left; break;
    case ui::Key::Right: input = ContentInteraction::KeyInput::Right; break;
    case ui::Key::Up: input = ContentInteraction::KeyInput::Up; break;
    case ui::Key::Down: input = ContentInteraction::KeyInput::Down; break;
    default: return false;
    }
    ContentInteraction& machine = client_.interaction();
    machine.setSelection(client_.currentSelection());
    const Intent intent = machine.key(input);
    client_.applyIntent(intent, event.modifiers.shift);
    host.requestRepaint();
    return intent.consumed;
}

void ContentLayer::paintPage(const ui::ViewportToolHost&, std::size_t, const core::Rect&, ui::PaintContext&) const {}

void ContentLayer::paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const {
    client_.placeEditor(host);
    const std::optional<ContentLayerClient::EditingOutline> editing = client_.editingOutline();
    const bool active = client_.active();
    if (!active && !editing.has_value()) return;
    const double zoom = host.zoomFactor();
    context.pushClip(host.viewportBounds());

    if (editing.has_value()) {
        if (const std::optional<core::Rect> pageRect = host.pageRectInViewport(editing->page); pageRect.has_value()) {
            strokeQuad(context, *pageRect, zoom, editing->quad, AnnotationPainter::accentColor());
        }
    }
    if (!active) {
        context.popClip();
        return;
    }

    const ContentInteraction& machine = client_.interaction();
    const ContentInteraction::Preview preview = machine.preview();
    const std::optional<ContentInteraction::Selected> selected = client_.currentSelection();
    const ui::Color accent = AnnotationPainter::accentColor();

    // Hover highlight: not over the selection and not while a gesture runs.
    if (const auto& hover = machine.hover();
        hover.has_value() && !machine.gestureActive() &&
        !(selected.has_value() && selected->page == hover->page && selected->info.id == hover->info.id)) {
        if (const std::optional<core::Rect> pageRect = host.pageRectInViewport(hover->page); pageRect.has_value()) {
            const ui::Color wash = ui::Color::rgba(accent.r, accent.g, accent.b, 0.12);
            if (hover->info.kind != ContentInteraction::Kind::Text) {
                context.fillRect(AnnotationPainter::toViewport(*pageRect, zoom, hover->info.bounds), wash);
            }
            paintOutline(context, *pageRect, zoom, hover->info, ui::Color::rgba(accent.r, accent.g, accent.b, 0.7));
        }
    }

    // Selection chrome. While the selection itself is being dragged only the
    // preview outline shows (the handles would lie about the geometry).
    const bool previewing = preview.kind != ContentInteraction::Preview::Kind::None &&
                            preview.kind != ContentInteraction::Preview::Kind::AddBox;
    if (selected.has_value() && !previewing) {
        if (const std::optional<core::Rect> pageRect = host.pageRectInViewport(selected->page); pageRect.has_value()) {
            const ContentInteraction::HitInfo& info = selected->info;
            const ui::Color color = info.capability == editor::ContentCapability::ReadOnly ? ui::Color::gray(0.5) : accent;
            paintOutline(context, *pageRect, zoom, info, color);
            if (info.canWrap) {
                paintHandle(context, toViewport(*pageRect, zoom, ContentInteraction::wrapHandlePoint(info)));
            } else if (info.canResize && info.kind != ContentInteraction::Kind::Text) {
                const core::Rect bounds = AnnotationPainter::toViewport(*pageRect, zoom, info.bounds);
                using Handle = AnnotationInteraction::Handle;
                for (const Handle handle : {Handle::TopLeft, Handle::Top, Handle::TopRight, Handle::Right,
                                            Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left}) {
                    paintHandle(context, AnnotationInteraction::handlePoint(bounds, handle));
                }
            }
        }
    }

    // Gesture preview.
    if (preview.kind != ContentInteraction::Preview::Kind::None) {
        if (const std::optional<core::Rect> pageRect = host.pageRectInViewport(preview.page); pageRect.has_value()) {
            using PreviewKind = ContentInteraction::Preview::Kind;
            switch (preview.kind) {
            case PreviewKind::Move:
                if (selected.has_value() && selected->info.kind == ContentInteraction::Kind::Text) {
                    strokeQuad(context, *pageRect, zoom, preview.quad, accent);
                } else {
                    context.strokeDashedRect(AnnotationPainter::toViewport(*pageRect, zoom, preview.rect), accent,
                                             kChromeWidth, kDash);
                }
                break;
            case PreviewKind::Resize:
            case PreviewKind::AddBox:
                context.strokeDashedRect(AnnotationPainter::toViewport(*pageRect, zoom, preview.rect), accent,
                                         kChromeWidth, kDash);
                break;
            case PreviewKind::Wrap:
                strokeQuad(context, *pageRect, zoom, preview.quad, accent);
                break;
            case PreviewKind::None:
                break;
            }
        }
    }
    context.popClip();
}

} // namespace rivet::app
