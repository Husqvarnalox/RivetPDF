// SPDX-License-Identifier: MPL-2.0
#include "app/FormLayer.hpp"

#include <optional>
#include <utility>

namespace rivet::app {

bool FormLayer::onMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    if (!client_.interactive()) return false;
    switch (event.type) {
    case ui::PointerEventType::Down: {
        if (event.button != 1) return false;
        if (client_.popupOpen()) {
            client_.popupPress(host, event.position);
            host.requestRepaint();
            return true;
        }
        const std::optional<std::pair<std::size_t, core::Point>> hit = host.pageAt(event.position);
        if (!hit.has_value()) {
            client_.pressOutside();
            host.requestRepaint();
            return false;
        }
        const bool consumed = client_.press(hit->first, hit->second);
        host.requestRepaint();
        return consumed;
    }
    case ui::PointerEventType::Move: {
        const std::optional<std::pair<std::size_t, core::Point>> hit = host.pageAt(event.position);
        client_.hover(hit.has_value(), hit.has_value() ? hit->first : 0, hit.has_value() ? hit->second : core::Point{});
        return false;
    }
    case ui::PointerEventType::Exited:
        client_.hover(false, 0, core::Point{});
        return false;
    case ui::PointerEventType::Up:
    case ui::PointerEventType::Entered:
    case ui::PointerEventType::Scroll:
        break;
    }
    return false;
}

void FormLayer::afterMouse(ui::ViewportToolHost& host, const ui::PointerEvent& event) {
    (void)host;
    (void)event;
}

bool FormLayer::onKey(ui::ViewportToolHost& host, const ui::KeyEvent& event) {
    if (!client_.interactive()) return false;
    const bool consumed = client_.key(event);
    if (consumed) host.requestRepaint();
    return consumed;
}

void FormLayer::paintPage(const ui::ViewportToolHost& host, std::size_t pageIndex,
                          const core::Rect& pageRectInViewport, ui::PaintContext& context) const {
    client_.paintPage(host, pageIndex, pageRectInViewport, context);
}

void FormLayer::paintAbove(const ui::ViewportToolHost& host, ui::PaintContext& context) const {
    client_.placeEditor(host);
    client_.paintAbove(host, context);
}

} // namespace rivet::app
