// SPDX-License-Identifier: MPL-2.0
#include "app/RedactionInteraction.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::app {

namespace {

core::Rect spanning(core::Point a, core::Point b) {
    return core::Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y)};
}

} // namespace

core::Point RedactionInteraction::clamp(core::Point point, core::Size pageSize) const {
    if (!(pageSize.width > 0.0) || !(pageSize.height > 0.0)) return point;
    return core::Point{std::clamp(point.x, 0.0, pageSize.width), std::clamp(point.y, 0.0, pageSize.height)};
}

RedactionInteraction::Intent RedactionInteraction::press(std::size_t page, core::Point display, core::Size pageSize) {
    if (!display.isFinite()) {
        Intent ignored;
        ignored.consumed = false;
        return ignored;
    }
    const core::Point start = clamp(display, pageSize);
    drag_ = Drag{page, start, start, pageSize};
    return Intent{};
}

void RedactionInteraction::drag(core::Point display) {
    if (!drag_.has_value() || !display.isFinite()) return;
    drag_->current = clamp(display, drag_->pageSize);
}

RedactionInteraction::Intent RedactionInteraction::release(core::Point display) {
    Intent intent;
    if (!drag_.has_value()) {
        intent.consumed = false;
        return intent;
    }
    drag(display);
    const Drag finished = *drag_;
    drag_.reset();
    const core::Rect rect = spanning(finished.start, finished.current);
    intent.page = finished.page;
    if (rect.size.width >= kMinDragExtent && rect.size.height >= kMinDragExtent) {
        intent.kind = Intent::Kind::AddMark;
        intent.rect = rect;
        return intent;
    }
    // A click: select the mark under it, or clear the selection.
    const std::optional<std::size_t> hit = hitTest_ ? hitTest_(finished.page, finished.start) : std::nullopt;
    if (hit.has_value()) {
        selection_ = MarkRef{finished.page, *hit};
        intent.kind = Intent::Kind::SelectMark;
        intent.index = *hit;
        return intent;
    }
    intent.kind = selection_.has_value() ? Intent::Kind::ClearSelection : Intent::Kind::None;
    selection_.reset();
    return intent;
}

RedactionInteraction::Intent RedactionInteraction::key(ui::Key key) {
    Intent intent;
    switch (key) {
    case ui::Key::Delete:
    case ui::Key::Backspace:
        if (selection_.has_value() && !drag_.has_value()) {
            intent.kind = Intent::Kind::RemoveMark;
            intent.page = selection_->page;
            intent.index = selection_->index;
            selection_.reset();
            return intent;
        }
        break;
    case ui::Key::Escape:
        if (drag_.has_value()) {
            drag_.reset();
            return intent;
        }
        if (selection_.has_value()) {
            selection_.reset();
            intent.kind = Intent::Kind::ClearSelection;
            return intent;
        }
        break;
    default:
        break;
    }
    intent.consumed = false;
    return intent;
}

void RedactionInteraction::cancel() { drag_.reset(); }

std::optional<std::pair<std::size_t, core::Rect>> RedactionInteraction::preview() const {
    if (!drag_.has_value()) return std::nullopt;
    return std::make_pair(drag_->page, spanning(drag_->start, drag_->current));
}

} // namespace rivet::app
