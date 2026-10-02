#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace rivet::ui {

// Repaint request target. Widgets call invalidate(); the sink (owned by the
// shell) schedules a frame. Intentionally no virtual destructor: it is
// implemented by the shell and never deleted through this interface.
struct IRedrawSink {
    virtual void requestRedraw() = 0;

protected:
    ~IRedrawSink() = default;
};

// Base class of the retained-mode widget tree used by Rivet's shell.
//
// Coordinate model:
//   - frame() is the widget's rectangle in the PARENT's coordinate space.
//   - bounds() is {0, 0, size} in the widget's own (local) space.
//   - Paint and input methods always operate in LOCAL coordinates; the tree
//     plumbing (paintChildren / onMouse / hitTest) performs conversions.
class Widget {
public:
    Widget() = default;
    virtual ~Widget() = default;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;

    // Frame in the parent's coordinate space. Runs layout() on change so
    // containers reposition their children after being resized.
    void setFrame(const core::Rect& frame) {
        if (frame_ == frame) return;
        frame_ = frame;
        layout();
    }
    const core::Rect& frame() const { return frame_; }

    // Local bounds: origin {0, 0}, size == frame().size.
    core::Rect bounds() const { return core::Rect{core::Point{}, frame_.size}; }

    // Ideal size hint. Default: the current frame size.
    virtual core::Size preferredSize(const PaintContext&) const { return frame_.size; }

    // Tree management. Children are painted in insertion order; the
    // last-added child is visually topmost for hit-testing.
    void addChild(std::unique_ptr<Widget> child) {
        if (!child) return;
        Widget* raw = child.get();
        children_.push_back(std::move(child));
        raw->parent_ = this;
        raw->setRedrawSink(redrawSink_);
        invalidate();
    }

    // Detaches `child` from this widget without destroying it, transferring
    // ownership to the caller. Returns nullptr when `child` is not a child of
    // this widget. The detached child's parent and redraw sink are cleared.
    std::unique_ptr<Widget> detachChild(const Widget* child) {
        if (child == nullptr) return nullptr;
        for (auto it = children_.begin(); it != children_.end(); ++it) {
            if (it->get() == child) {
                if (capturedChild_ == child) capturedChild_ = nullptr;
                std::unique_ptr<Widget> detached = std::move(*it);
                detached->parent_ = nullptr;
                detached->setRedrawSink(nullptr);
                children_.erase(it);
                invalidate();
                return detached;
            }
        }
        return nullptr;
    }

    // Detaches `child` and destroys it. Returns true when it was found. On a
    // true return the child pointer is dangling afterwards — use detachChild()
    // when ownership must survive the removal.
    bool removeChild(const Widget* child) { return detachChild(child) != nullptr; }

    const std::vector<std::unique_ptr<Widget>>& children() const { return children_; }
    Widget* parent() { return parent_; }
    const Widget* parent() const { return parent_; }

    // Paint this widget (self first), then children via paintChildren().
    virtual void paint(PaintContext& context) const {
        paintSelf(context);
        paintChildren(context);
    }

    // Override point for self painting; default paints nothing.
    virtual void paintSelf(PaintContext&) const {}

    // Paints children in order, each clipped (and origin-translated) to its
    // frame; see the PaintContext contract for push/pop semantics.
    void paintChildren(PaintContext& context) const {
        for (const auto& child : children_) {
            if (child->frame().isEmpty()) continue;
            context.pushClip(child->frame());
            child->paint(context);
            context.popClip();
        }
    }

    // Input. Events arrive in this widget's LOCAL coordinates (routers
    // hit-test and convert). Return true if consumed. Default: forward to
    // children in reverse paint order (topmost first, converting the point
    // into each child's space) until one consumes the event.
    //
    // Pointer capture: the child that consumed a Down is "captured" and
    // receives the following Moves with a button held and the Up even when
    // the pointer left its frame, so drags (scrollbar thumb, text selection,
    // press-and-release buttons) behave outside the widget. The capture ends
    // on Up, on a Move without a held button (the Up was lost elsewhere), on
    // the next Down, and when the child is detached from this widget. It is
    // kept per parent, so it composes down the tree through this default.
    virtual bool onMouse(const PointerEvent& event) {
        if (capturedChild_ != nullptr) {
            const bool heldMove = event.type == PointerEventType::Move && event.button != 0;
            if (event.type == PointerEventType::Up || heldMove) {
                Widget& target = *capturedChild_;
                if (event.type == PointerEventType::Up) capturedChild_ = nullptr;
                PointerEvent local = event;
                local.position = event.position - target.frame_.origin;
                // The target may detach (and destroy) itself while handling
                // the event: detachChild() clears the capture, and `target`
                // is not touched afterwards.
                const bool handled = target.onMouse(local);
                if (local.accepted) event.accepted = true;
                return handled;
            }
            if (event.type == PointerEventType::Down || event.type == PointerEventType::Move) {
                capturedChild_ = nullptr; // stale: the matching Up never arrived
            }
        }
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            Widget& child = **it;
            if (!child.frame().contains(event.position)) continue;
            PointerEvent local = event;
            local.position = event.position - child.frame().origin;
            const Widget* const target = &child;
            if (child.onMouse(local)) {
                if (event.type == PointerEventType::Down) captureIfStillChild(target);
                return true;
            }
        }
        return false;
    }

    // Keyboard events have no position; the default forwards to focused
    // children only. The host decides which widget holds focus.
    virtual bool onKey(const KeyEvent& event) {
        for (const auto& child : children_) {
            if (child->isFocused() && child->onKey(event)) return true;
        }
        return false;
    }

    // Topmost widget (deepest child) whose frame contains localPoint, or
    // this widget when no child matches. Children are probed in reverse
    // paint order, so the last-added (visually topmost) wins.
    virtual Widget* hitTest(const core::Point& localPoint) {
        for (auto it = children_.rbegin(); it != children_.rend(); ++it) {
            Widget& child = **it;
            if (child.frame().contains(localPoint)) {
                return child.hitTest(localPoint - child.frame_.origin);
            }
        }
        return this;
    }

    // Invalidation plumbing. The sink propagates to the whole subtree, and
    // children added later inherit it in addChild().
    void setRedrawSink(IRedrawSink* sink) {
        redrawSink_ = sink;
        for (const auto& child : children_) child->setRedrawSink(sink);
    }

    // Requests a repaint through the sink; no-op when none is installed.
    void invalidate() const {
        if (redrawSink_ != nullptr) redrawSink_->requestRedraw();
    }

    // Layout hook: called by setFrame() after frame changes and by
    // containers after repositioning children. Default: no-op (children keep
    // their manual frames). Containers that arrange children override this.
    virtual void layout() {}

    // Focus foundations. Focus state is tracked per widget; the host decides
    // which widget receives it and routes key events through the tree.
    virtual bool wantsFocus() const { return false; }
    void setFocused(bool focused) {
        if (focused_ == focused) return;
        focused_ = focused;
        invalidate();
    }
    bool isFocused() const { return focused_; }

private:
    // Starts capturing the child after a consumed Down, unless the handler
    // already removed it from this widget.
    void captureIfStillChild(const Widget* target) {
        for (const auto& child : children_) {
            if (child.get() == target) {
                capturedChild_ = child.get();
                return;
            }
        }
    }

    Widget* parent_ = nullptr;
    Widget* capturedChild_ = nullptr; // pointer capture (see onMouse)
    std::vector<std::unique_ptr<Widget>> children_;
    core::Rect frame_;
    IRedrawSink* redrawSink_ = nullptr;
    bool focused_ = false;
};

} // namespace rivet::ui
