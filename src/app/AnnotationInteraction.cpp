// SPDX-License-Identifier: MPL-2.0
#include "app/AnnotationInteraction.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace rivet::app {

namespace {

double distance(core::Point a, core::Point b) { return std::hypot(a.x - b.x, a.y - b.y); }

core::Rect rectFromCorners(core::Point a, core::Point b) {
    return core::Rect{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(a.x - b.x), std::abs(a.y - b.y)};
}

bool bigEnough(const core::Rect& rect) {
    return rect.size.width >= AnnotationInteraction::kMinEditSize &&
           rect.size.height >= AnnotationInteraction::kMinEditSize;
}

} // namespace

// --- Static geometry -----------------------------------------------------------

core::Point AnnotationInteraction::handlePoint(const core::Rect& bounds, Handle handle) {
    const double x0 = bounds.minX();
    const double x1 = bounds.maxX();
    const double y0 = bounds.minY();
    const double y1 = bounds.maxY();
    const double xm = (x0 + x1) / 2.0;
    const double ym = (y0 + y1) / 2.0;
    switch (handle) {
    case Handle::TopLeft: return {x0, y0};
    case Handle::Top: return {xm, y0};
    case Handle::TopRight: return {x1, y0};
    case Handle::Right: return {x1, ym};
    case Handle::BottomRight: return {x1, y1};
    case Handle::Bottom: return {xm, y1};
    case Handle::BottomLeft: return {x0, y1};
    case Handle::Left: return {x0, ym};
    default: return bounds.center();
    }
}

AnnotationInteraction::Handle AnnotationInteraction::handleAt(const core::Rect& bounds, core::Point point,
                                                              double zoom) {
    if (!(zoom > 0.0)) return Handle::None;
    const double radius = kHandleHitRadius / zoom;
    // Corners first, then edge midpoints.
    static constexpr Handle kOrder[] = {Handle::TopLeft,    Handle::TopRight, Handle::BottomRight,
                                        Handle::BottomLeft, Handle::Top,      Handle::Right,
                                        Handle::Bottom,     Handle::Left};
    for (const Handle handle : kOrder) {
        const core::Point at = handlePoint(bounds, handle);
        if (std::abs(point.x - at.x) <= radius && std::abs(point.y - at.y) <= radius) return handle;
    }
    return Handle::None;
}

AnnotationInteraction::Handle AnnotationInteraction::lineHandleAt(core::Point lineStart, core::Point lineEnd,
                                                                  core::Point point, double zoom) {
    if (!(zoom > 0.0)) return Handle::None;
    const double radius = kHandleHitRadius / zoom;
    const auto near = [&](core::Point at) {
        return std::abs(point.x - at.x) <= radius && std::abs(point.y - at.y) <= radius;
    };
    if (near(lineEnd)) return Handle::LineEnd;
    if (near(lineStart)) return Handle::LineStart;
    return Handle::None;
}

core::Rect AnnotationInteraction::resizedRect(const core::Rect& bounds, Handle handle, core::Point point) {
    double left = bounds.minX();
    double right = bounds.maxX();
    double top = bounds.minY();
    double bottom = bounds.maxY();
    switch (handle) {
    case Handle::TopLeft: left = point.x; top = point.y; break;
    case Handle::Top: top = point.y; break;
    case Handle::TopRight: right = point.x; top = point.y; break;
    case Handle::Right: right = point.x; break;
    case Handle::BottomRight: right = point.x; bottom = point.y; break;
    case Handle::Bottom: bottom = point.y; break;
    case Handle::BottomLeft: left = point.x; bottom = point.y; break;
    case Handle::Left: left = point.x; break;
    default: break;
    }
    return core::Rect{std::min(left, right), std::min(top, bottom), std::abs(right - left),
                      std::abs(bottom - top)};
}

core::Rect AnnotationInteraction::dragRect(core::Point start, core::Point current, bool square,
                                           core::Size pageSize) {
    if (!square) return rectFromCorners(start, current);
    const double dx = current.x - start.x;
    const double dy = current.y - start.y;
    const double signX = dx < 0.0 ? -1.0 : 1.0;
    const double signY = dy < 0.0 ? -1.0 : 1.0;
    double side = std::max(std::abs(dx), std::abs(dy));
    if (pageSize.width > 0.0 && pageSize.height > 0.0) {
        const double roomX = signX > 0.0 ? pageSize.width - start.x : start.x;
        const double roomY = signY > 0.0 ? pageSize.height - start.y : start.y;
        side = std::min({side, std::max(0.0, roomX), std::max(0.0, roomY)});
    }
    return rectFromCorners(start, core::Point{start.x + signX * side, start.y + signY * side});
}

core::Point AnnotationInteraction::lineEndFor(core::Point anchor, core::Point end, bool constrain) {
    if (!constrain) return end;
    const double dx = end.x - anchor.x;
    const double dy = end.y - anchor.y;
    const double length = std::hypot(dx, dy);
    if (length <= 0.0) return end;
    constexpr double kStep = std::numbers::pi / 4.0;
    const double angle = std::round(std::atan2(dy, dx) / kStep) * kStep;
    return core::Point{anchor.x + length * std::cos(angle), anchor.y + length * std::sin(angle)};
}

core::Rect AnnotationInteraction::defaultStampRect(core::Point center, core::Size pageSize) {
    double x = center.x - kStampWidth / 2.0;
    double y = center.y - kStampHeight / 2.0;
    if (pageSize.width > 0.0 && pageSize.height > 0.0) {
        x = std::clamp(x, 0.0, std::max(0.0, pageSize.width - kStampWidth));
        y = std::clamp(y, 0.0, std::max(0.0, pageSize.height - kStampHeight));
    }
    return core::Rect{x, y, kStampWidth, kStampHeight};
}

// --- State machine -----------------------------------------------------------------

void AnnotationInteraction::setTool(AnnotationTool tool) {
    if (tool_ == tool) return;
    tool_ = tool;
    cancelGesture();
}

core::Point AnnotationInteraction::clamp(core::Point point, core::Size pageSize) const {
    if (!(pageSize.width > 0.0) || !(pageSize.height > 0.0)) return point;
    return core::Point{std::clamp(point.x, 0.0, pageSize.width), std::clamp(point.y, 0.0, pageSize.height)};
}

bool AnnotationInteraction::gestureInProgress() const {
    return gesture_.kind != GestureKind::None && gesture_.kind != GestureKind::Swallow;
}

std::optional<std::size_t> AnnotationInteraction::gesturePage() const {
    if (gesture_.kind == GestureKind::None) return std::nullopt;
    return gesture_.page;
}

AnnotationInteraction::Intent AnnotationInteraction::pointerDown(const PointerInput& input) {
    gesture_ = Gesture{};
    const double zoom = input.zoom > 0.0 ? input.zoom : 1.0;
    const core::Point point = clamp(input.point, input.pageSize);

    Gesture gesture;
    gesture.page = input.page;
    gesture.start = point;
    gesture.current = point;
    gesture.pageSize = input.pageSize;
    gesture.zoom = zoom;
    gesture.shift = input.shift;

    // Select-like behaviour: every tool but Ink.
    if (tool_ != AnnotationTool::Ink) {
        // 1. Handles of the selected annotation (on this page).
        if (selection_.has_value() && selection_->page == input.page) {
            const HitInfo& info = selection_->info;
            Handle handle = Handle::None;
            if (info.canResize) {
                handle = info.isLine ? lineHandleAt(info.lineStart, info.lineEnd, input.point, zoom)
                                     : handleAt(info.bounds, input.point, zoom);
            }
            if (handle != Handle::None) {
                gesture.kind = (handle == Handle::LineStart || handle == Handle::LineEnd)
                                   ? GestureKind::LineHandle
                                   : GestureKind::Resize;
                gesture.handle = handle;
                gesture.target = *selection_;
                gesture_ = std::move(gesture);
                return Intent{};
            }
        }
        // 2. Annotation hit: select it (never create), maybe start a move.
        if (hitTest_) {
            if (const std::optional<HitInfo> hit = hitTest_(input.page, input.point, kHitTolerance / zoom);
                hit.has_value()) {
                const bool alreadySelected = selection_.has_value() && selection_->info.id == hit->id;
                selection_ = Selected{input.page, *hit};
                gesture.target = *selection_;
                gesture.kind = hit->canMove ? GestureKind::Move : GestureKind::Swallow;
                gesture_ = std::move(gesture);
                if (input.clickCount >= 2 && hit->canEditContents) {
                    gesture_.kind = GestureKind::Swallow;
                    Intent intent;
                    intent.kind = Intent::Kind::OpenNoteEditor;
                    intent.page = input.page;
                    intent.id = hit->id;
                    return intent;
                }
                if (alreadySelected) return Intent{};
                Intent intent;
                intent.kind = Intent::Kind::Select;
                intent.page = input.page;
                intent.id = hit->id;
                return intent;
            }
        }
    }

    // 3. A miss (or Ink).
    switch (tool_) {
    case AnnotationTool::Select:
    case AnnotationTool::Highlight:
    case AnnotationTool::Underline:
    case AnnotationTool::StrikeOut: {
        // Text selection / links belong to the viewport.
        Intent intent;
        intent.consumed = false;
        if (selection_.has_value()) {
            selection_.reset();
            intent.kind = Intent::Kind::ClearSelection;
        } else {
            intent.kind = Intent::Kind::PassThrough;
        }
        return intent;
    }
    case AnnotationTool::Note:
        gesture.kind = GestureKind::NoteClick;
        break;
    case AnnotationTool::Ink:
        gesture.kind = GestureKind::Ink;
        gesture.points.push_back(point);
        break;
    case AnnotationTool::Rectangle:
    case AnnotationTool::Ellipse:
    case AnnotationTool::Line:
    case AnnotationTool::Arrow:
    case AnnotationTool::Stamp:
        gesture.kind = GestureKind::ShapeDrag;
        break;
    }
    gesture_ = std::move(gesture);
    return Intent{};
}

void AnnotationInteraction::track(Gesture& gesture, const PointerInput& input) const {
    if (input.zoom > 0.0) gesture.zoom = input.zoom;
    gesture.shift = input.shift;
    gesture.current = clamp(input.point, gesture.pageSize);
    if (!gesture.started && distance(gesture.start, gesture.current) >= kDragThreshold / gesture.zoom) {
        gesture.started = true;
    }
    if (gesture.kind == GestureKind::Ink && gesture.points.size() < kMaxInkPoints &&
        distance(gesture.points.back(), gesture.current) >= 0.25 / gesture.zoom) {
        gesture.points.push_back(gesture.current);
    }
}

AnnotationInteraction::Intent AnnotationInteraction::pointerMove(const PointerInput& input) {
    if (gesture_.kind == GestureKind::None) {
        Intent intent;
        intent.kind = Intent::Kind::PassThrough;
        intent.consumed = false;
        return intent;
    }
    track(gesture_, input);
    return Intent{};
}

AnnotationInteraction::Intent AnnotationInteraction::pointerUp(const PointerInput& input) {
    if (gesture_.kind == GestureKind::None) {
        Intent intent;
        intent.kind = Intent::Kind::PassThrough;
        intent.consumed = false;
        return intent;
    }
    track(gesture_, input);
    const Gesture finished = std::move(gesture_);
    gesture_ = Gesture{};
    return commit(finished);
}

AnnotationInteraction::Intent AnnotationInteraction::commit(const Gesture& g) const {
    Intent intent;
    intent.page = g.page;
    intent.zoom = g.zoom;
    switch (g.kind) {
    case GestureKind::None:
    case GestureKind::Swallow:
        break;
    case GestureKind::NoteClick:
        if (!g.started) {
            core::Rect box{g.start.x, g.start.y, kNoteSize, kNoteSize};
            if (g.pageSize.width > 0.0 && g.pageSize.height > 0.0) {
                box.origin.x = std::clamp(box.origin.x, 0.0, std::max(0.0, g.pageSize.width - kNoteSize));
                box.origin.y = std::clamp(box.origin.y, 0.0, std::max(0.0, g.pageSize.height - kNoteSize));
            }
            intent.kind = Intent::Kind::CreateNote;
            intent.rect = box;
        }
        break;
    case GestureKind::Ink:
        if (g.started && g.points.size() >= 2) {
            intent.kind = Intent::Kind::CreateInk;
            intent.strokes.push_back(g.points);
        }
        break;
    case GestureKind::ShapeDrag:
        intent.tool = tool_;
        switch (tool_) {
        case AnnotationTool::Rectangle:
        case AnnotationTool::Ellipse: {
            if (!g.started) break;
            const core::Rect rect = dragRect(g.start, g.current, g.shift, g.pageSize);
            if (!bigEnough(rect)) break;
            intent.kind = Intent::Kind::CreateShape;
            intent.rect = rect;
            break;
        }
        case AnnotationTool::Line:
        case AnnotationTool::Arrow:
            if (!g.started) break;
            intent.kind = Intent::Kind::CreateShape;
            intent.a = g.start;
            intent.b = lineEndFor(g.start, g.current, g.shift);
            break;
        case AnnotationTool::Stamp:
            if (!g.started) {
                intent.kind = Intent::Kind::CreateStamp;
                intent.rect = defaultStampRect(g.start, g.pageSize);
                break;
            }
            if (const core::Rect rect = dragRect(g.start, g.current, false, g.pageSize); bigEnough(rect)) {
                intent.kind = Intent::Kind::CreateStamp;
                intent.rect = rect;
            }
            break;
        default:
            break;
        }
        break;
    case GestureKind::Move: {
        const core::Point delta = g.current - g.start;
        if (!g.started || (delta.x == 0.0 && delta.y == 0.0)) break;
        intent.kind = Intent::Kind::Move;
        intent.id = g.target.info.id;
        intent.delta = delta;
        break;
    }
    case GestureKind::Resize: {
        if (!g.started) break;
        const core::Rect rect = resizedRect(g.target.info.bounds, g.handle, g.current);
        if (!bigEnough(rect) || rect == g.target.info.bounds) break;
        intent.kind = Intent::Kind::Resize;
        intent.id = g.target.info.id;
        intent.rect = rect;
        break;
    }
    case GestureKind::LineHandle: {
        if (!g.started) break;
        const HitInfo& info = g.target.info;
        intent.kind = Intent::Kind::LineEndpoints;
        intent.id = info.id;
        if (g.handle == Handle::LineStart) {
            intent.a = lineEndFor(info.lineEnd, g.current, g.shift);
            intent.b = info.lineEnd;
        } else {
            intent.a = info.lineStart;
            intent.b = lineEndFor(info.lineStart, g.current, g.shift);
        }
        break;
    }
    }
    return intent;
}

AnnotationInteraction::Intent AnnotationInteraction::key(KeyInput input) {
    Intent intent;
    intent.consumed = false;
    intent.kind = Intent::Kind::PassThrough;
    switch (input) {
    case KeyInput::Escape:
        if (gestureInProgress()) {
            // Cancel; the rest of the press (moves, release) is swallowed.
            gesture_.kind = GestureKind::Swallow;
            gesture_.points.clear();
            gesture_.started = false;
            intent.kind = Intent::Kind::None;
            intent.consumed = true;
        } else if (selection_.has_value()) {
            selection_.reset();
            intent.kind = Intent::Kind::ClearSelection;
            intent.consumed = true;
        } else if (tool_ != AnnotationTool::Select) {
            setTool(AnnotationTool::Select);
            intent.kind = Intent::Kind::SelectTool;
            intent.tool = AnnotationTool::Select;
            intent.consumed = true;
        }
        break;
    case KeyInput::Delete:
    case KeyInput::Backspace:
        if (!gestureActive() && selection_.has_value() && selection_->info.canDelete) {
            intent.kind = Intent::Kind::Delete;
            intent.id = selection_->info.id;
            intent.page = selection_->page;
            intent.consumed = true;
            selection_.reset();
        }
        break;
    case KeyInput::Enter:
        if (!gestureActive() && selection_.has_value() && selection_->info.canEditContents) {
            intent.kind = Intent::Kind::OpenNoteEditor;
            intent.id = selection_->info.id;
            intent.page = selection_->page;
            intent.consumed = true;
        }
        break;
    }
    return intent;
}

AnnotationInteraction::Intent AnnotationInteraction::afterUnconsumedUp(bool textSelectionNonEmpty) const {
    Intent intent;
    if (isMarkupTool(tool_) && textSelectionNonEmpty) {
        intent.kind = Intent::Kind::ConvertTextSelection;
        intent.tool = tool_;
    } else {
        intent.kind = Intent::Kind::PassThrough;
        intent.consumed = false;
    }
    return intent;
}

AnnotationInteraction::Preview AnnotationInteraction::preview() const {
    Preview preview;
    const Gesture& g = gesture_;
    preview.page = g.page;
    switch (g.kind) {
    case GestureKind::Ink:
        if (g.points.size() >= 2) {
            preview.kind = Preview::Kind::Ink;
            preview.tool = AnnotationTool::Ink;
            preview.points = g.points;
        }
        break;
    case GestureKind::ShapeDrag:
        if (!g.started) break;
        preview.tool = tool_;
        if (tool_ == AnnotationTool::Line || tool_ == AnnotationTool::Arrow) {
            preview.kind = Preview::Kind::Line;
            preview.a = g.start;
            preview.b = lineEndFor(g.start, g.current, g.shift);
        } else {
            const bool square = g.shift && (tool_ == AnnotationTool::Rectangle || tool_ == AnnotationTool::Ellipse);
            preview.kind = Preview::Kind::Rect;
            preview.rect = dragRect(g.start, g.current, square, g.pageSize);
        }
        break;
    case GestureKind::Move: {
        if (!g.started) break;
        const core::Point delta = g.current - g.start;
        preview.editing = true;
        const HitInfo& info = g.target.info;
        if (info.isLine) {
            preview.kind = Preview::Kind::Line;
            preview.a = info.lineStart + delta;
            preview.b = info.lineEnd + delta;
        } else {
            preview.kind = Preview::Kind::Bounds;
            preview.rect = info.bounds.translated(delta);
        }
        break;
    }
    case GestureKind::Resize:
        if (!g.started) break;
        preview.editing = true;
        preview.kind = Preview::Kind::Bounds;
        preview.rect = resizedRect(g.target.info.bounds, g.handle, g.current);
        break;
    case GestureKind::LineHandle: {
        if (!g.started) break;
        const HitInfo& info = g.target.info;
        preview.editing = true;
        preview.kind = Preview::Kind::Line;
        if (g.handle == Handle::LineStart) {
            preview.a = lineEndFor(info.lineEnd, g.current, g.shift);
            preview.b = info.lineEnd;
        } else {
            preview.a = info.lineStart;
            preview.b = lineEndFor(info.lineStart, g.current, g.shift);
        }
        break;
    }
    default:
        break;
    }
    return preview;
}

} // namespace rivet::app
