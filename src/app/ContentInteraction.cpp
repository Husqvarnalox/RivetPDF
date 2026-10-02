// SPDX-License-Identifier: MPL-2.0
#include "app/ContentInteraction.hpp"

#include <algorithm>
#include <cmath>

namespace rivet::app {

namespace {

using Intent = ContentInteraction::Intent;
using Handle = ContentInteraction::Handle;

double distance(core::Point a, core::Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

bool isCorner(Handle handle) {
    return handle == Handle::TopLeft || handle == Handle::TopRight || handle == Handle::BottomLeft ||
           handle == Handle::BottomRight;
}

core::Rect rectFromCorners(core::Point a, core::Point b) {
    return core::Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
}

Intent passThrough() {
    Intent intent;
    intent.kind = Intent::Kind::PassThrough;
    intent.consumed = false;
    return intent;
}

} // namespace

void ContentInteraction::setTool(ContentTool tool) {
    tool_ = tool;
    gesture_ = Gesture{};
    hover_.reset();
}

core::Point ContentInteraction::clamp(core::Point point, core::Size pageSize) const {
    if (pageSize.width <= 0.0 || pageSize.height <= 0.0) return point;
    return core::Point{std::clamp(point.x, 0.0, pageSize.width), std::clamp(point.y, 0.0, pageSize.height)};
}

core::Point ContentInteraction::baselineDirection(const std::array<core::Point, 4>& quad) {
    const double dx = quad[1].x - quad[0].x;
    const double dy = quad[1].y - quad[0].y;
    const double length = std::hypot(dx, dy);
    if (length < 1e-9) return core::Point{1.0, 0.0};
    return core::Point{dx / length, dy / length};
}

core::Point ContentInteraction::wrapHandlePoint(const HitInfo& info) {
    return core::Point{(info.quad[1].x + info.quad[2].x) / 2.0, (info.quad[1].y + info.quad[2].y) / 2.0};
}

core::Rect ContentInteraction::aspectResizedRect(const core::Rect& bounds, Handle handle, core::Point point) {
    if (!isCorner(handle) || bounds.size.width <= 0.0 || bounds.size.height <= 0.0) {
        return AnnotationInteraction::resizedRect(bounds, handle, point);
    }
    const bool left = handle == Handle::TopLeft || handle == Handle::BottomLeft;
    const bool top = handle == Handle::TopLeft || handle == Handle::TopRight;
    // The fixed corner is the opposite one.
    const core::Point anchor{left ? bounds.maxX() : bounds.minX(), top ? bounds.maxY() : bounds.minY()};
    const double sx = left ? -1.0 : 1.0;
    const double sy = top ? -1.0 : 1.0;
    const double scale = std::max(0.0, std::max(sx * (point.x - anchor.x) / bounds.size.width,
                                                sy * (point.y - anchor.y) / bounds.size.height));
    const double width = bounds.size.width * scale;
    const double height = bounds.size.height * scale;
    return core::Rect{left ? anchor.x - width : anchor.x, top ? anchor.y - height : anchor.y, width, height};
}

void ContentInteraction::track(Gesture& gesture, const PointerInput& input) const {
    gesture.current = clamp(input.point, gesture.pageSize);
    gesture.shift = input.shift;
    if (!gesture.started && distance(gesture.start, gesture.current) * gesture.zoom >= kDragThreshold) {
        gesture.started = true;
    }
}

core::Rect ContentInteraction::resizeTarget(const Gesture& gesture) const {
    const core::Rect& bounds = gesture.target.info.bounds;
    const bool image = gesture.target.info.kind == Kind::Image;
    const bool keepAspect = image != gesture.shift && isCorner(gesture.handle);
    return keepAspect ? aspectResizedRect(bounds, gesture.handle, gesture.current)
                      : AnnotationInteraction::resizedRect(bounds, gesture.handle, gesture.current);
}

double ContentInteraction::wrapTarget(const Gesture& gesture) const {
    const core::Point u = baselineDirection(gesture.target.info.quad);
    const double travel = (gesture.current.x - gesture.start.x) * u.x + (gesture.current.y - gesture.start.y) * u.y;
    return std::max(kMinWrapWidth, gesture.target.info.wrapBase + travel);
}

Intent ContentInteraction::pointerDown(const PointerInput& input) {
    const double zoom = input.zoom > 0.0 ? input.zoom : 1.0;
    Gesture gesture;
    gesture.page = input.page;
    gesture.start = clamp(input.point, input.pageSize);
    gesture.current = gesture.start;
    gesture.pageSize = input.pageSize;
    gesture.zoom = zoom;
    gesture.shift = input.shift;
    hover_.reset();

    if (tool_ == ContentTool::AddText) {
        gesture.kind = GestureKind::AddText;
        gesture_ = std::move(gesture);
        return Intent{};
    }
    if (tool_ != ContentTool::SelectObject) return passThrough();

    // 1. A handle of the selection.
    if (selection_.has_value() && selection_->page == input.page) {
        const HitInfo& info = selection_->info;
        if (info.canWrap) {
            const core::Point handle = wrapHandlePoint(info);
            const double radius = AnnotationInteraction::kHandleHitRadius / zoom;
            if (std::abs(input.point.x - handle.x) <= radius && std::abs(input.point.y - handle.y) <= radius) {
                gesture.kind = GestureKind::Wrap;
                gesture.target = *selection_;
                gesture_ = std::move(gesture);
                return Intent{};
            }
        } else if (info.canResize && info.kind != Kind::Text) {
            const Handle handle = AnnotationInteraction::handleAt(info.bounds, input.point, zoom);
            if (handle != Handle::None) {
                gesture.kind = GestureKind::Resize;
                gesture.handle = handle;
                gesture.target = *selection_;
                gesture_ = std::move(gesture);
                return Intent{};
            }
        }
    }

    // 2. An object: select it (maybe start a move, maybe open the editor).
    if (hitTest_) {
        if (const std::optional<HitInfo> hit = hitTest_(input.page, input.point, kHitTolerance / zoom);
            hit.has_value()) {
            selection_ = Selected{input.page, *hit};
            gesture.target = *selection_;
            gesture.kind = hit->canMove ? GestureKind::Move : GestureKind::Swallow;
            gesture_ = std::move(gesture);
            Intent intent;
            intent.page = input.page;
            intent.info = *hit;
            if (input.clickCount >= 2 && hit->canEditText) {
                gesture_.kind = GestureKind::Swallow;
                intent.kind = Intent::Kind::OpenEditor;
            } else {
                intent.kind = Intent::Kind::Select;
            }
            return intent;
        }
    }

    // 3. A miss: deselect; the press is consumed (no text selection here).
    gesture.kind = GestureKind::Swallow;
    gesture_ = std::move(gesture);
    Intent clear;
    clear.kind = Intent::Kind::ClearSelection;
    selection_.reset();
    return clear;
}

Intent ContentInteraction::pointerMove(const PointerInput& input) {
    if (gesture_.kind == GestureKind::None) {
        // Hover.
        if (tool_ == ContentTool::SelectObject && hitTest_) {
            const double zoom = input.zoom > 0.0 ? input.zoom : 1.0;
            const std::optional<HitInfo> hit = hitTest_(input.page, input.point, kHitTolerance / zoom);
            if (hit.has_value()) hover_ = Selected{input.page, *hit};
            else hover_.reset();
        } else {
            hover_.reset();
        }
        Intent intent;
        intent.kind = Intent::Kind::None;
        intent.consumed = false;
        return intent;
    }
    track(gesture_, input);
    return Intent{};
}

Intent ContentInteraction::pointerUp(const PointerInput& input) {
    Gesture g = std::move(gesture_);
    gesture_ = Gesture{};
    if (g.kind == GestureKind::None) return passThrough();
    g.current = clamp(input.point, g.pageSize);
    g.shift = input.shift;
    if (!g.started && distance(g.start, g.current) * g.zoom >= kDragThreshold) g.started = true;

    Intent intent;
    intent.page = g.page;
    intent.info = g.target.info;
    switch (g.kind) {
    case GestureKind::Move: {
        if (!g.started) break;
        const core::Point delta{g.current.x - g.start.x, g.current.y - g.start.y};
        if (delta.x == 0.0 && delta.y == 0.0) break;
        intent.kind = Intent::Kind::Move;
        intent.delta = delta;
        break;
    }
    case GestureKind::Resize: {
        if (!g.started) break;
        const core::Rect rect = resizeTarget(g);
        if (rect.size.width < kMinSize || rect.size.height < kMinSize || rect == g.target.info.bounds) break;
        intent.kind = Intent::Kind::Resize;
        intent.rect = rect;
        break;
    }
    case GestureKind::Wrap: {
        if (!g.started) break;
        const double width = wrapTarget(g);
        if (std::abs(width - g.target.info.wrapBase) < 0.5) break;
        intent.kind = Intent::Kind::SetWrap;
        intent.width = width;
        break;
    }
    case GestureKind::AddText: {
        if (g.started) {
            intent.kind = Intent::Kind::AddTextBox;
            intent.rect = rectFromCorners(g.start, g.current);
            if (intent.rect.size.width < kMinWrapWidth) {
                // Too thin to be a box: treat as a click.
                intent.kind = Intent::Kind::AddTextClick;
                intent.point = g.start;
            }
        } else {
            intent.kind = Intent::Kind::AddTextClick;
            intent.point = g.start;
        }
        break;
    }
    case GestureKind::Swallow:
    case GestureKind::None:
        break;
    }
    return intent;
}

Intent ContentInteraction::key(KeyInput input) {
    Intent intent;
    switch (input) {
    case KeyInput::Escape:
        if (gesture_.kind != GestureKind::None) {
            gesture_ = Gesture{}; // the held press is cancelled; its release passes through
            return Intent{};
        }
        if (selection_.has_value()) {
            selection_.reset();
            intent.kind = Intent::Kind::ClearSelection;
            return intent;
        }
        return passThrough();
    case KeyInput::Delete:
    case KeyInput::Backspace:
        if (!selection_.has_value() || tool_ != ContentTool::SelectObject) return passThrough();
        intent.kind = Intent::Kind::Delete;
        intent.page = selection_->page;
        intent.info = selection_->info;
        return intent;
    case KeyInput::Enter:
        if (!selection_.has_value() || tool_ != ContentTool::SelectObject || !selection_->info.canEditText) {
            return passThrough();
        }
        intent.kind = Intent::Kind::OpenEditor;
        intent.page = selection_->page;
        intent.info = selection_->info;
        return intent;
    case KeyInput::Left:
    case KeyInput::Right:
    case KeyInput::Up:
    case KeyInput::Down:
        if (!selection_.has_value() || tool_ != ContentTool::SelectObject || !selection_->info.canMove) {
            return passThrough();
        }
        intent.kind = Intent::Kind::Nudge;
        intent.page = selection_->page;
        intent.info = selection_->info;
        intent.key = input;
        return intent; // the controller scales by Shift (it sees the modifier)
    }
    return passThrough();
}

bool ContentInteraction::gestureInProgress() const {
    switch (gesture_.kind) {
    case GestureKind::Move:
    case GestureKind::Resize:
    case GestureKind::Wrap:
        return gesture_.started;
    case GestureKind::AddText:
        return true;
    default:
        return false;
    }
}

std::optional<std::size_t> ContentInteraction::gesturePage() const {
    if (gesture_.kind == GestureKind::None) return std::nullopt;
    return gesture_.page;
}

ContentInteraction::Preview ContentInteraction::preview() const {
    Preview preview;
    const Gesture& g = gesture_;
    if (!g.started) return preview;
    preview.page = g.page;
    switch (g.kind) {
    case GestureKind::Move: {
        const core::Point delta{g.current.x - g.start.x, g.current.y - g.start.y};
        preview.kind = Preview::Kind::Move;
        preview.rect = g.target.info.bounds;
        preview.rect.origin.x += delta.x;
        preview.rect.origin.y += delta.y;
        preview.quad = g.target.info.quad;
        for (core::Point& p : preview.quad) {
            p.x += delta.x;
            p.y += delta.y;
        }
        break;
    }
    case GestureKind::Resize:
        preview.kind = Preview::Kind::Resize;
        preview.rect = resizeTarget(g);
        break;
    case GestureKind::Wrap: {
        const HitInfo& info = g.target.info;
        const core::Point u = baselineDirection(info.quad);
        const double width = wrapTarget(g);
        preview.kind = Preview::Kind::Wrap;
        preview.quad = info.quad;
        preview.quad[1] = core::Point{info.quad[0].x + u.x * width, info.quad[0].y + u.y * width};
        preview.quad[2] = core::Point{preview.quad[1].x + (info.quad[2].x - info.quad[1].x),
                                      preview.quad[1].y + (info.quad[2].y - info.quad[1].y)};
        break;
    }
    case GestureKind::AddText:
        preview.kind = Preview::Kind::AddBox;
        preview.rect = rectFromCorners(g.start, g.current);
        break;
    default:
        break;
    }
    return preview;
}

} // namespace rivet::app
