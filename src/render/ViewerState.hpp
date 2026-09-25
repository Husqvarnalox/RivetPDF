// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "render/ZoomState.hpp"

#include <functional>

namespace rivet::render {

// View state of ONE document view (one tab): zoom and scroll offset.
//
// The state is owned by the application layer per view (DocumentTab) and the
// PdfViewport binds to it while the tab is active, mutating it during
// interaction. Keeping the state outside the viewport is what preserves
// per-tab scroll/zoom across tab switches and leaves room for several views
// over one document session.
//
// Single source of truth: while a state is bound, the viewport reads and
// writes ONLY through it; the viewport holds no shadow copy.
//
// Clamping: ViewerState stores the offset as given. The viewport owns
// clamping (it knows the layout and its own frame) and re-clamps on bind,
// on layout and on every write it performs.
//
// Main-thread only; no internal synchronization.
class ViewerState {
public:
    ViewerState();

    ViewerState(const ViewerState&) = delete;
    ViewerState& operator=(const ViewerState&) = delete;

    ZoomState& zoom() { return zoom_; }
    const ZoomState& zoom() const { return zoom_; }

    // Scroll offset in content points (the point of the zoom-1.0 content
    // shown at the viewport's top-left). Not clamped here: the bound
    // viewport owns clamping against the live content bounds.
    const core::Point& scrollOffsetPoints() const { return scrollOffset_; }
    void setScrollOffsetPoints(const core::Point& offset);

    // Fired on the calling thread whenever the offset or the zoom actually
    // changes. The viewport installs its invalidate/cancel handler here.
    void setCallback(std::function<void()> onChanged);

private:
    ViewerState(ViewerState&&) = delete;
    ViewerState& operator=(ViewerState&&) = delete;

    ZoomState zoom_;
    core::Point scrollOffset_{0.0, 0.0};
    std::function<void()> onChanged_;
};

} // namespace rivet::render
