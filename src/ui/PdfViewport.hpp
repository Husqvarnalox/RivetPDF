// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/RenderPriority.hpp"
#include "render/ViewerState.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/ViewerTextBridge.hpp"
#include "ui/Widget.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace rivet::render {
class PageLayout;
class IRenderSource;
struct RasterParams;
struct TileKey;
} // namespace rivet::render

namespace rivet::ui {

// The document view: paints pages as tiled rasters fetched from a render
// source and drives zoom + scroll for the shell.
//
// State ownership (Phase 2): the viewport holds NO document view state of its
// own. Zoom and scroll offset live in the bound render::ViewerState (owned by
// the application layer per tab); the viewport is the component that mutates
// it during interaction and re-clamps it against the live content bounds.
// While no state is bound, an internal fallback state serves the empty view.
//
// Ownership and lifetime:
//   - The viewport never owns the layout, the render source, or the document.
//     The shell must keep them alive or call clearDocument() first. Null
//     layout/source is the supported empty state.
//   - The bound ViewerState must outlive the binding or be released via
//     clearDocument() (which unbinds and disconnects the state callback).
//
// Fit modes are MODES, not one-shot intents: while fitMode() is Width or
// Page, every layout pass (e.g. a window resize) recomputes the zoom. Manual
// zoom (zoomInStep / zoomOutStep / zoomActualSize / setManualZoom and
// pointer-anchored pinch) exits the mode first.
//
// Threading contract for render completion callbacks:
//   - Per the IRenderSource contract, onDone callbacks must be delivered on
//     the main thread when a main-thread dispatcher is configured (production
//     setups must configure one). The viewport additionally guards against
//     callbacks arriving after its destruction via a shared atomic flag, so
//     late or stray callbacks become no-ops instead of use-after-free; this
//     is a safety net, not a license to invoke from arbitrary threads.
//   - The completion callback merely invalidates the view; the shell's redraw
//     sink paints the newly arrived tile on the next frame.
//
// State-change funnel: ViewerState::setCallback is installed by the viewport
// itself (invalidate + drop queued renders on zoom change + scrollbar sync +
// current-page tracking + status notification). The shell must use
// setZoomChangedCallback rather than replacing the state's callback.
class PdfViewport : public Widget {
public:
    static constexpr std::uint32_t kTileSize = 512; // device px per tile edge

    PdfViewport();
    ~PdfViewport() override;

    // Binds the document and the view state this viewport renders and
    // mutates. layout/source may be null (empty state); revisionProvider
    // supplies the document revision used for cache identity (may be null ->
    // revision 0). viewState may be null: the internal fallback state is used
    // (its offset resets). Rebinding re-clamps the incoming state's offset.
    void setDocument(core::DocumentId documentId, const render::PageLayout* layout,
                     render::IRenderSource* source,
                     std::function<std::uint64_t()> revisionProvider,
                     render::ViewerState* viewState);
    void clearDocument();

    // The bound view state, or the internal fallback state when unbound.
    // Never null. viewState() is the authoritative pointer; zoom() is a
    // convenience accessor into it.
    render::ViewerState* viewState() { return state_; }
    render::ZoomState& zoom() { return state_->zoom(); }
    const render::ZoomState& zoom() const { return state_->zoom(); }

    // Manual zoom entry points: exit fit mode, then apply. Used by toolbar,
    // keyboard and pinch paths so fit modes stay sticky until the user zooms.
    bool zoomInStep();
    bool zoomOutStep();
    void zoomActualSize();
    bool setManualZoom(double zoom);

    // Enters a fit MODE (Width or Page): the zoom recomputes on every layout
    // pass until manual zoom exits the mode. Applies immediately via layout().
    void setFitMode(render::ZoomState::FitMode mode);

    // Scroll offset: the content-space point (at zoom 1.0) shown at the
    // viewport's top-left. Reads/writes through the bound state, clamped.
    core::Point scrollOffsetPoints() const { return state_->scrollOffsetPoints(); }
    void setScrollOffsetPoints(const core::Point& offset);
    void scrollByContentPoints(const core::Point& delta);

    // Scrolls so that page `index`'s top edge sits at the viewport's top
    // (clamped to the content bounds); index >= pageCount is rejected.
    void goToPage(std::size_t index);

    // The currently tracked page (viewport-center rule; see
    // PageLayout::currentPageIndex). 0 when no document is bound.
    std::size_t currentPageIndex() const { return currentPage_; }

    // The content-space rect currently visible: {scrollOffset, frame / zoom}.
    core::Rect visibleContentRectPoints() const;

    // Status-bar hook; invoked when the zoom actually changes (not on scroll).
    void setZoomChangedCallback(std::function<void(double)> onZoomChanged);

    // Fired when the tracked current page changes (scroll, zoom, resize,
    // goToPage). The shell updates the sidebar selection and the page field.
    void setCurrentPageChangedCallback(std::function<void(std::size_t)> onPageChanged);

    // Focus intent: the host owns focus routing; the viewport reports that a
    // click wants keyboard focus (arrow keys etc.).
    void setOnFocusRequested(std::function<void()> onFocusRequested);

    // Installs the text interaction bridge (non-owning; may be null to
    // disable text features). The viewport calls warmPage() for the tracked
    // page and its neighbors, routes mouse selection through the bridge and
    // paints the bridge's overlay rects above the tiles.
    void setTextBridge(IViewerTextBridge* bridge) { textBridge_ = bridge; }

    // Presentation mode: one page centered (fit-page recomputed per page),
    // black canvas, no scrollbars; Page/arrow/Space keys flip pages. The
    // shell hides its chrome around it. Exiting restores the previous zoom
    // and fit mode (the bound ViewerState is untouched: presentation applies
    // through a temporary ZoomState swap... it simply drives fit-mode and
    // page navigation; the user's zoom is restored by the shell on exit).
    void setPresentationMode(bool enabled);
    bool presentationMode() const { return presentationMode_; }

    // Scrolls the view so that the given rect of page `index` (page display
    // points) is centered in the viewport (clamped to the content bounds);
    // keeps the current zoom. Used by search-result and outline navigation.
    void revealContentRect(std::size_t pageIndex, const core::Rect& pageRectPoints);

    core::Size preferredSize(const PaintContext&) const override { return frame().size; }

    // Scroll: pinch-zoom (command/ctrl) anchored at the pointer, plain
    // content scrolling. Other pointer events route to children first (the
    // scrollbars), then fall through unconsumed (selection handles them in a
    // later phase).
    bool onMouse(const PointerEvent& event) override;

    // Plus/'=' zoom in, Minus/'-' zoom out, PageUp/PageDown scroll one
    // viewport height in content points, Home/End jump to the top/bottom,
    // arrows scroll by a fixed screen distance. Consumes only what it acts on.
    bool onKey(const KeyEvent& event) override;

    void paintSelf(PaintContext& context) const override;

    // Re-clamps the scroll offset, recomputes an active fit mode, resyncs the
    // scrollbars and refreshes the tracked current page.
    void layout() override;

private:
    void paintPageTiles(std::size_t pageIndex, const core::Rect& pageFramePoints,
                        const core::Rect& pageInViewport, const core::Rect& contentRect,
                        std::uint64_t revision, PaintContext& context) const;
    void paintPageOverlays(std::size_t pageIndex, const core::Rect& pageFramePoints,
                           PaintContext& context) const;
    // Prefetches a limited band of the adjacent pages (top band of the next
    // page, bottom band of the previous one) at Impending priority so the
    // lane ordering keeps them behind visible tiles.
    void prefetchNeighborPages(const std::pair<std::size_t, std::size_t>& visibleRange,
                               const core::Rect& contentRect, std::uint64_t revision,
                               PaintContext& context) const;
    // Requests (without painting) the tiles of one page overlapping a
    // content-space band.
    void requestBandTiles(std::size_t pageIndex, const core::Rect& bandContentRect,
                          std::uint64_t revision, PaintContext& context) const;
    void requestTile(const render::TileKey& key, const render::RasterParams& params) const;
    void requestTileWithPriority(const render::TileKey& key, const render::RasterParams& params,
                                 render::RenderPriority priority) const;
    core::Point clampedScrollOffset(const core::Point& offset) const;
    void resolveFitMode();
    // Frames the scrollbar children within the current bounds.
    void positionScrollbars();
    // The one ViewerState change funnel (zoom or scroll).
    void onStateChanged();
    // Pushes current extents/offset into the scrollbar children.
    void syncScrollbars();
    // Recomputes the tracked page from the visible rect; fires the callback.
    void updateCurrentPage();
    // Page display points under a viewport-local point, or nullopt when the
    // point is outside every visible page.
    std::optional<std::pair<std::size_t, core::Point>> pagePointAt(const core::Point& localPoint) const;

    core::DocumentId documentId_;
    const render::PageLayout* layout_ = nullptr;
    render::IRenderSource* source_ = nullptr;
    std::function<std::uint64_t()> revisionProvider_;
    // The bound state; never null (falls back to emptyStateState_).
    render::ViewerState* state_ = nullptr;
    render::ViewerState emptyStateState_;
    std::size_t currentPage_ = 0;
    double lastReportedZoom_ = 1.0;

    std::function<void(double)> onZoomChanged_;
    std::function<void(std::size_t)> onPageChanged_;
    std::function<void()> onFocusRequested_;

    // Scrollbar children, overlaying the content (created here, owned by the
    // widget tree; raw pointers valid for the viewport's lifetime).
    ScrollBar* vScrollBar_ = nullptr;
    ScrollBar* hScrollBar_ = nullptr;

    // Text interaction bridge (non-owning; null = text features disabled).
    IViewerTextBridge* textBridge_ = nullptr;
    // True while a mouse drag is selecting text.
    bool selecting_ = false;
    bool presentationMode_ = false;

    // Pending link press (down over a link; fires on up over the same one).
    bool linkPressed_ = false;
    std::optional<ViewerLinkHit> pressedLink_;
    // Hover state for the link under the pointer (repaint on change).
    bool linkHovered_ = false;

    // Alive flag shared with in-flight render callbacks (see class comment).
    std::shared_ptr<std::atomic<bool>> aliveFlag_;
};

} // namespace rivet::ui
