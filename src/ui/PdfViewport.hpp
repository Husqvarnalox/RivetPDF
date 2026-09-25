#pragma once

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/ZoomState.hpp"
#include "ui/Widget.hpp"

#include <atomic>
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
// source and owns scroll + zoom state for the shell.
//
// Ownership and lifetime:
//   - The viewport never owns the layout, the render source, or the document.
//     The shell must keep them alive or call clearDocument() first. Null
//     layout/source is the supported empty state.
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
// Zoom callbacks: the viewport installs the ZoomState::setCallback hook
// itself (invalidate + cancelAll on the render source, dropping queued
// renders that were requested at the previous scale + status notification).
// The shell must use setZoomChangedCallback rather than replacing ZoomState's
// callback.
class PdfViewport : public Widget {
public:
    static constexpr std::uint32_t kTileSize = 512; // device px per tile edge

    PdfViewport();
    ~PdfViewport() override;

    // Non-owning handles. layout/source may be null (empty state);
    // revisionProvider supplies the document revision used for cache
    // identity (may be null -> revision 0). Resets the scroll offset.
    void setDocument(core::DocumentId documentId, const render::PageLayout* layout,
                     render::IRenderSource* source,
                     std::function<std::uint64_t()> revisionProvider);
    void clearDocument();

    render::ZoomState& zoom() { return zoom_; }
    const render::ZoomState& zoom() const { return zoom_; }

    // Scroll offset: the content-space point (at zoom 1.0) shown at the
    // viewport's top-left.
    core::Point scrollOffsetPoints() const { return scrollOffsetPoints_; }

    // Clamped to the content bounds: x/y in [0, max(0, content - viewportExtent/zoom)].
    void setScrollOffsetPoints(const core::Point& offset);
    void scrollByContentPoints(const core::Point& delta);

    // Status-bar hook; invoked whenever the zoom actually changes.
    void setZoomChangedCallback(std::function<void(double)> onZoomChanged);

    core::Size preferredSize(const PaintContext&) const override { return frame().size; }

    // Scroll event with command/ctrl: zoom step (delta.y < 0 => zoom in),
    // anchored at the viewport center. Plain scroll: content delta =
    // scrollDelta / zoom. Both consume the event.
    bool onMouse(const PointerEvent& event) override;

    // Plus/'=' zoom in, Minus/'-' zoom out, PageUp/PageDown scroll one
    // viewport height in content points, Home/End jump to the top/bottom.
    bool onKey(const KeyEvent& event) override;

    void paintSelf(PaintContext& context) const override;

    // Re-clamps the scroll offset after a resize and resolves a pending zoom
    // fit mode (FitMode::Width uses the widest page, FitMode::Page the first
    // page; fit intent is consumed one-shot, then reset to None).
    void layout() override;

private:
    void paintPageTiles(std::size_t pageIndex, const core::Rect& pageFramePoints,
                        const core::Rect& pageInViewport, const core::Rect& contentRect,
                        std::uint64_t revision, PaintContext& context) const;
    void requestTile(const render::TileKey& key, const render::RasterParams& params) const;
    core::Point clampedScrollOffset(const core::Point& offset) const;
    void resolveFitMode();

    core::DocumentId documentId_;
    const render::PageLayout* layout_ = nullptr;
    render::IRenderSource* source_ = nullptr;
    std::function<std::uint64_t()> revisionProvider_;
    render::ZoomState zoom_;
    core::Point scrollOffsetPoints_;
    std::function<void(double)> onZoomChanged_;
    // Alive flag shared with in-flight render callbacks (see class comment).
    std::shared_ptr<std::atomic<bool>> aliveFlag_;
};

} // namespace rivet::ui
