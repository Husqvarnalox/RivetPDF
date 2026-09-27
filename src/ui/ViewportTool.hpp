// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "ui/PaintContext.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <optional>

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

protected:
    ~ViewportToolHost() = default;
};

// An interaction mode layered over the document view (crop today; future
// annotation/markup tools). The viewport forwards pointer (except wheel
// scroll/zoom, which keeps navigating), key and paint calls to the ACTIVE
// tool before its own handling, so the viewport itself stays a pure
// document view and tools own their state and geometry.
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

} // namespace rivet::ui
