// SPDX-License-Identifier: MPL-2.0
#include "app/RedactionLayer.hpp"
#include "ui/Theme.hpp"

#include <utility>

namespace rivet::app {

namespace {

constexpr double kMarkStroke = 1.5;
constexpr double kSelectedStroke = 3.0;

core::Rect toViewport(const core::Rect& pageRect, double zoom, const core::Rect& display) {
    return core::Rect{pageRect.origin.x + display.origin.x * zoom, pageRect.origin.y + display.origin.y * zoom,
                      display.size.width * zoom, display.size.height * zoom};
}

core::Point toDisplay(const core::Rect& pageRect, double zoom, core::Point viewport) {
    if (!(zoom > 0.0)) return viewport;
    return core::Point{(viewport.x - pageRect.origin.x) / zoom, (viewport.y - pageRect.origin.y) / zoom};
}

} // namespace

bool RedactionLayer::onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    if (!client_.active()) return false;
    RedactionInteraction& machine = client_.interaction();
    switch (event.type) {
    case ui::PointerEventType::Down: {
        if (event.button != 1 || event.modifiers.command || event.modifiers.control || event.modifiers.option) {
            return false;
        }
        const std::optional<std::pair<std::size_t, core::Point>> hit = host.pageAt(event.position);
        if (!hit.has_value()) return false;
        const std::optional<core::Size> size = client_.pageSize(hit->first);
        const RedactionInteraction::Intent intent =
            machine.press(hit->first, hit->second, size.value_or(core::Size{}));
        host.requestRepaint();
        return intent.consumed;
    }
    case ui::PointerEventType::Move: {
        const std::optional<std::pair<std::size_t, core::Rect>> preview = machine.preview();
        if (!preview.has_value()) return false;
        const std::optional<core::Rect> pageRect = host.pageRectInViewport(preview->first);
        if (!pageRect.has_value()) return true;
        machine.drag(toDisplay(*pageRect, host.zoomFactor(), event.position));
        host.requestRepaint();
        return true;
    }
    case ui::PointerEventType::Up: {
        const std::optional<std::pair<std::size_t, core::Rect>> preview = machine.preview();
        if (!preview.has_value()) return false;
        const std::optional<core::Rect> pageRect = host.pageRectInViewport(preview->first);
        RedactionInteraction::Intent intent;
        if (pageRect.has_value()) {
            intent = machine.release(toDisplay(*pageRect, host.zoomFactor(), event.position));
        } else {
            machine.cancel();
        }
        client_.applyIntent(intent);
        host.requestRepaint();
        return true;
    }
    case ui::PointerEventType::Exited:
    case ui::PointerEventType::Entered:
    case ui::PointerEventType::Scroll:
        break;
    }
    return false;
}

void RedactionLayer::afterMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    (void)host;
    (void)event;
}

bool RedactionLayer::onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) {
    if (!client_.active() || event.modifiers.command || event.modifiers.control) return false;
    const RedactionInteraction::Intent intent = client_.interaction().key(event.key);
    if (!intent.consumed) return false;
    client_.applyIntent(intent);
    host.requestRepaint();
    return true;
}

void RedactionLayer::paintPage(const ui::ViewportToolHost& host, std::size_t pageIndex,
                               const core::Rect& pageRectInViewport, ui::PaintContext& context) const {
    const std::vector<core::Rect> marks = client_.marks(pageIndex);
    if (marks.empty()) return;
    const double zoom = host.zoomFactor();
    const std::optional<RedactionInteraction::MarkRef>& selected = client_.interaction().selection();
    const ui::Color color = markColor();
    const ui::Color tint = ui::withAlpha(color, 0.18);
    for (std::size_t i = 0; i < marks.size(); ++i) {
        const core::Rect rect = toViewport(pageRectInViewport, zoom, marks[i]);
        const bool isSelected = client_.active() && selected.has_value() && selected->page == pageIndex &&
                                selected->index == i;
        context.fillRect(rect, tint);
        context.strokeRect(rect, color, isSelected ? kSelectedStroke : kMarkStroke);
    }
}

void RedactionLayer::paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const {
    const std::optional<std::pair<std::size_t, core::Rect>> preview = client_.interaction().preview();
    if (!preview.has_value()) return;
    const std::optional<core::Rect> pageRect = host.pageRectInViewport(preview->first);
    if (!pageRect.has_value()) return;
    const core::Rect rect = toViewport(*pageRect, host.zoomFactor(), preview->second);
    const ui::Color color = markColor();
    context.fillRect(rect, ui::withAlpha(color, 0.12));
    context.strokeDashedRect(rect, color, kMarkStroke, 4.0);
}

} // namespace rivet::app
