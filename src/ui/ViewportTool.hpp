// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <optional>
#include <utility>

namespace rivet::ui {

// What a viewport tool may ask of the viewport hosting it. All rects are in
// the viewport's LOCAL logical coordinates (the space pointer events arrive
// in): points, top-left origin, y-down. Device pixels never appear here -
// hit testing in logical points is Retina-independent by construction.
class ViewportToolHost {
public:
    // Frame of page `pageIndex` (layout order) in viewport-local logical
    // coordinates at the current zoom and scroll (it may lie partly or fully
    // outside the viewport bounds). nullopt when no such page is laid out.
    virtual std::optional<core::Rect> pageRectInViewport(std::size_t pageIndex) const = 0;
    // Logical points per page display point (the current zoom).
    virtual double zoomFactor() const = 0;
    // The viewport's local bounds.
    virtual core::Rect viewportBounds() const = 0;
    virtual void requestRepaint() = 0;
    // Number of laid-out pages (0 without a document).
    virtual std::size_t pageCount() const = 0;
    // The visible page under a viewport-local point: its layout index and
    // the point in that page's DISPLAY space (points, top-left origin,
    // y-down, the same space annotations and text hit testing use). nullopt
    // when the point is outside every visible page or nothing is laid out.
    virtual std::optional<std::pair<std::size_t, core::Point>>
    pageAt(core::Point viewportPoint) const = 0;

protected:
    ~ViewportToolHost() = default;
};

// An interaction mode layered over the document view (crop today). The
// viewport forwards pointer (except wheel scroll/zoom, which keeps
// navigating), key and paint calls to the ACTIVE tool before its own
// handling, so the viewport itself stays a pure document view and tools own
// their state and geometry. Annotation editing is not a tool: it is a
// persistent ViewportLayer (below) that coexists with text selection.
//
// Main thread only. The viewport does not own the tool: the owner installs
// it with PdfViewport::setActiveTool and must uninstall it before the tool
// dies.
class ViewportTool {
public:
    virtual ~ViewportTool() = default;

    // Pointer events (viewport-local coordinates). Return true to consume.
    virtual bool onMouse(ViewportToolHost& host, const PointerEvent& event) = 0;
    // Keys while the tool is active. Return true to consume.
    virtual bool onKey(ViewportToolHost& host, const KeyEvent& event) = 0;
    // Painted above the page tiles and text overlays, below the scrollbars.
    virtual void paint(const ViewportToolHost& host, PaintContext& context) const = 0;
};

// The annotation layer slot: a persistent overlay the viewport consults for
// every pointer (except wheel), key and paint call, in this order:
//   pointer: active tool -> layer.onMouse -> the viewport's own handling
//            (links, text selection, scrollbars) -> layer.afterMouse for an
//            event the layer did not consume;
//   key:     active tool -> layer.onKey -> the viewport's own handling.
// Wheel scroll/pinch never reach the layer (they keep navigating). In
// presentation mode the layer is still PAINTED (annotations stay visible)
// but receives no pointer or key events.
//
// Main thread only. Non-owning: the owner installs it with
// PdfViewport::setAnnotationLayer and must uninstall it before it dies.
class ViewportLayer {
public:
    virtual ~ViewportLayer() = default;

    // Before the viewport's own handling. Return true to consume.
    virtual bool onMouse(ViewportToolHost& host, const PointerEvent& event) = 0;
    // After the viewport handled an event this layer did not consume
    // (whatever the viewport did with it). Hover feedback, deselection.
    virtual void afterMouse(ViewportToolHost& host, const PointerEvent& event) = 0;
    virtual bool onKey(ViewportToolHost& host, const KeyEvent& event) = 0;
    // Per visible page, right after the page's tiles and below the text,
    // search and link overlays. `pageRectInViewport` is the rect the page's
    // tiles were just painted in (viewport-local logical points).
    virtual void paintPage(const ViewportToolHost& host, std::size_t pageIndex,
                           const core::Rect& pageRectInViewport, PaintContext& context) const = 0;
    // Once, after every page overlay and below the active tool: selection
    // handles, in-progress shapes.
    virtual void paintAbove(const ViewportToolHost& host, PaintContext& context) const = 0;
};

} // namespace rivet::ui
