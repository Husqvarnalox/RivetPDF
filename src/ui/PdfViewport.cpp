#include "ui/PdfViewport.hpp"

#include "core/Error.hpp"
#include "render/PageLayout.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "render/RenderScaleKey.hpp"
#include "render/RenderSource.hpp"
#include "render/TileKey.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Color kCanvasBackground = Color::gray(0.82);
constexpr Color kEmptyStateColor = Color::gray(0.45);
constexpr Color kPageBackground = Color::white();
constexpr Color kPageBorder = Color::gray(0.7);
constexpr Color kTilePlaceholder = Color::gray(0.92);
constexpr Font kEmptyStateFont{15.0, Font::Weight::Regular};
constexpr const char* kEmptyStateText = "Open a PDF to begin";

// Number of kTileSize cells needed to cover a device-pixel extent.
std::uint32_t tileCount(double deviceExtent) {
    if (!(deviceExtent > 0.0)) return 0;
    return static_cast<std::uint32_t>(std::ceil(deviceExtent / static_cast<double>(PdfViewport::kTileSize)));
}

} // namespace

PdfViewport::PdfViewport()
    : aliveFlag_(std::make_shared<std::atomic<bool>>(true)) {
    // Single funnel for zoom changes: repaint + status-bar notification.
    zoom_.setCallback([this](double value) {
        invalidate();
        if (onZoomChanged_) onZoomChanged_(value);
    });
}

PdfViewport::~PdfViewport() {
    // In-flight render callbacks may outlive the widget; the shared flag
    // turns them into no-ops (see the threading contract in the header).
    aliveFlag_->store(false, std::memory_order_release);
}

void PdfViewport::setDocument(core::DocumentId documentId, const render::PageLayout* layout,
                              render::IRenderSource* source,
                              std::function<std::uint64_t()> revisionProvider) {
    if (source_ != nullptr && source_ != source) source_->cancelAll();
    documentId_ = documentId;
    layout_ = layout;
    source_ = source;
    revisionProvider_ = std::move(revisionProvider);
    scrollOffsetPoints_ = core::Point{};
    invalidate();
}

void PdfViewport::clearDocument() {
    if (source_ != nullptr) source_->cancelAll();
    documentId_ = core::DocumentId{};
    layout_ = nullptr;
    source_ = nullptr;
    revisionProvider_ = nullptr;
    scrollOffsetPoints_ = core::Point{};
    invalidate();
}

void PdfViewport::setScrollOffsetPoints(const core::Point& offset) {
    const core::Point clamped = clampedScrollOffset(offset);
    if (core::Point::nearlyEqual(clamped, scrollOffsetPoints_)) return;
    scrollOffsetPoints_ = clamped;
    invalidate();
}

void PdfViewport::scrollByContentPoints(const core::Point& delta) {
    setScrollOffsetPoints(scrollOffsetPoints_ + delta);
}

void PdfViewport::setZoomChangedCallback(std::function<void(double)> onZoomChanged) {
    onZoomChanged_ = std::move(onZoomChanged);
}

core::Point PdfViewport::clampedScrollOffset(const core::Point& offset) const {
    if (layout_ == nullptr) return core::Point{};
    const core::Size content = layout_->contentSizePoints();
    const core::Size visibleExtent = frame().size / zoom_.zoom();
    const auto clampAxis = [](double value, double contentExtent, double visibleExtentPoints) {
        const double maxOffset = std::max(0.0, contentExtent - visibleExtentPoints);
        return std::clamp(value, 0.0, maxOffset);
    };
    return core::Point{clampAxis(offset.x, content.width, visibleExtent.width),
                       clampAxis(offset.y, content.height, visibleExtent.height)};
}

bool PdfViewport::onMouse(const PointerEvent& event) {
    if (event.type != PointerEventType::Scroll) return false;

    if (event.modifiers.command || event.modifiers.control) {
        // Pinch gesture: stepped zoom anchored at the viewport center.
        const double oldZoom = zoom_.zoom();
        const bool changed = event.scrollDelta.y < 0.0 ? zoom_.zoomIn() : zoom_.zoomOut();
        if (changed) {
            const double newZoom = zoom_.zoom();
            const core::Point center{frame().size.width / 2.0, frame().size.height / 2.0};
            const core::Point anchored = scrollOffsetPoints_ + center / oldZoom;
            setScrollOffsetPoints(anchored - center / newZoom);
        }
        event.accepted = true;
        return true;
    }

    // Plain scroll: wheel delta is in logical pixels; convert to content
    // points (positive delta = content moves up/left = offset grows).
    scrollByContentPoints(event.scrollDelta / zoom_.zoom());
    event.accepted = true;
    return true;
}

bool PdfViewport::onKey(const KeyEvent& event) {
    const bool zoomInKey = event.key == Key::Plus ||
                           (event.key == Key::Character && event.text == "=");
    const bool zoomOutKey = event.key == Key::Minus ||
                            (event.key == Key::Character && event.text == "-");
    if (zoomInKey || zoomOutKey) {
        if (zoomInKey) {
            zoom_.zoomIn();
        } else {
            zoom_.zoomOut();
        }
        event.accepted = true;
        return true;
    }

    // PageUp/PageDown scroll one viewport height (in content points).
    const double pageHeightPoints = frame().size.height / zoom_.zoom();
    switch (event.key) {
    case Key::PageDown:
        scrollByContentPoints(core::Point{0.0, pageHeightPoints});
        break;
    case Key::PageUp:
        scrollByContentPoints(core::Point{0.0, -pageHeightPoints});
        break;
    case Key::Home:
        setScrollOffsetPoints(core::Point{scrollOffsetPoints_.x, 0.0});
        break;
    case Key::End:
        // Out-of-range values clamp to the content bounds.
        setScrollOffsetPoints(
            core::Point{scrollOffsetPoints_.x, std::numeric_limits<double>::max()});
        break;
    default:
        return false;
    }
    event.accepted = true;
    return true;
}

void PdfViewport::layout() {
    resolveFitMode();
    // Re-clamp after a resize (no-op while the offset stays valid).
    setScrollOffsetPoints(scrollOffsetPoints_);
}

void PdfViewport::resolveFitMode() {
    const render::ZoomState::FitMode mode = zoom_.fitMode();
    if (mode == render::ZoomState::FitMode::None) return;
    if (layout_ == nullptr || layout_->pageCount() == 0) {
        zoom_.setFitMode(render::ZoomState::FitMode::None);
        return;
    }
    if (mode == render::ZoomState::FitMode::Width) {
        double widest = 0.0;
        for (const render::PageLayout::PageInfo& page : layout_->pages()) {
            widest = std::max(widest, page.sizePoints.width);
        }
        zoom_.setZoom(zoom_.fitWidthZoom(frame().size.width, widest));
    } else { // FitMode::Page: fit the first page within the viewport
        zoom_.setZoom(zoom_.fitPageZoom(frame().size.width, frame().size.height,
                                        layout_->pages().front().sizePoints));
    }
    // Fit intent is one-shot: consumed by this layout pass.
    zoom_.setFitMode(render::ZoomState::FitMode::None);
}

// Paint algorithm:
//   1. Fill the canvas background.
//   2. Empty state (no layout/source): centered hint text; done.
//   3. contentRect = {scrollOffsetPoints, frame.size / zoom} in content
//      points; layout->visiblePageRange(contentRect) selects what to draw.
//   4. Each visible page: map its content-space frame into local viewport
//      coordinates ((frame.origin - scrollOffset) * zoom, size * zoom),
//      fill it white and stroke a border.
//   5. Tile pass per page (paintPageTiles): overlay the cached tiles and
//      request the missing ones, painting placeholders meanwhile.
void PdfViewport::paintSelf(PaintContext& context) const {
    context.pushClip(bounds());
    context.fillRect(bounds(), kCanvasBackground);

    if (layout_ == nullptr || source_ == nullptr) {
        context.drawText(kEmptyStateText, bounds(), kEmptyStateFont, kEmptyStateColor,
                         TextAlign::Center);
        context.popClip();
        return;
    }

    const double zoomFactor = zoom_.zoom();
    const core::Rect contentRect{scrollOffsetPoints_, frame().size / zoomFactor};
    const std::optional<std::pair<std::size_t, std::size_t>> visible =
        layout_->visiblePageRange(contentRect);
    if (visible.has_value()) {
        const std::uint64_t revision = revisionProvider_ ? revisionProvider_() : 0;
        for (std::size_t i = visible->first; i <= visible->second; ++i) {
            const core::Rect pageFrame = layout_->pageFramePoints(i);
            const core::Rect pageInViewport{(pageFrame.origin - scrollOffsetPoints_) * zoomFactor,
                                            pageFrame.size * zoomFactor};
            context.fillRect(pageInViewport, kPageBackground);
            context.strokeRect(pageInViewport, kPageBorder, 1.0);
            paintPageTiles(i, pageFrame, pageInViewport, contentRect, revision, context);
        }
    }
    context.popClip();
}

// Tile geometry: tiles live on a kTileSize x kTileSize DEVICE-pixel grid
// anchored at the page's top-left. Page display coordinates are top-left
// origin / y-down (same orientation as viewport-local logical space), so the
// grid maps directly with no flip: the cell (tx, ty) covers page points
// {tx*512, ty*512, 512, 512} / devicePixelsPerPoint.
//
// The raster rect recorded in RasterParams is the tile cell CLIPPED to the
// page bounds (page display points). Cache identity is stable across scrolls
// and repaints because the clip depends only on the page size; the bitmap
// returned by cachedTile() for a TileKey is therefore expected to raster
// exactly params.pageRectPoints at params.devicePixelsPerPoint, and the
// viewport draws it scaled into that same region mapped to viewport space.
//
// Missing tiles: paint a gray placeholder and ask the render source for the
// tile at Visible priority. Deduplicating identical in-flight requests is
// the source's job (IRenderSource contract). The completion callback merely
// invalidates; it must be invoked on the viewport's owning thread (main
// thread when a dispatcher is configured) — see the header's threading notes.
void PdfViewport::paintPageTiles(std::size_t pageIndex, const core::Rect& pageFramePoints,
                                 const core::Rect& pageInViewport, const core::Rect& contentRect,
                                 std::uint64_t revision, PaintContext& context) const {
    const render::PageLayout::PageInfo& info = layout_->pages()[pageIndex];
    const render::RenderScaleKey scaleKey = render::RenderScaleKey::fromZoom(zoom_.zoom());
    const double devicePixelsPerPoint = scaleKey.scale() * context.backingScale();

    // Visible region inside the page, in page-local display points (culling).
    const core::Rect pageBounds{core::Point{}, pageFramePoints.size};
    const core::Rect visibleLocal =
        contentRect.intersection(pageFramePoints).translated(-pageFramePoints.origin);
    if (visibleLocal.isEmpty()) return;

    const double tileExtentPoints = static_cast<double>(kTileSize) / devicePixelsPerPoint;
    const std::uint32_t tilesX = tileCount(pageFramePoints.size.width * devicePixelsPerPoint);
    const std::uint32_t tilesY = tileCount(pageFramePoints.size.height * devicePixelsPerPoint);

    for (std::uint32_t ty = 0; ty < tilesY; ++ty) {
        for (std::uint32_t tx = 0; tx < tilesX; ++tx) {
            const core::Rect tileRect{static_cast<double>(tx) * tileExtentPoints,
                                      static_cast<double>(ty) * tileExtentPoints,
                                      tileExtentPoints, tileExtentPoints};
            const core::Rect clipped = tileRect.intersection(pageBounds);
            if (clipped.isEmpty() || !visibleLocal.intersects(clipped)) continue;

            const render::TileKey key{documentId_, info.id, scaleKey, tx, ty};
            const core::Rect dest{pageInViewport.origin + clipped.origin * zoom_.zoom(),
                                  clipped.size * zoom_.zoom()};
            if (const std::shared_ptr<const core::Bitmap> tile = source_->cachedTile(key, revision)) {
                context.drawBitmap(*tile, dest);
            } else {
                context.fillRect(dest, kTilePlaceholder);
                requestTile(key, render::RasterParams{clipped, devicePixelsPerPoint});
            }
        }
    }
}

void PdfViewport::requestTile(const render::TileKey& key, const render::RasterParams& params) const {
    const render::RenderRequest request{key, params};
    // Shared alive flag: a callback delivered after destruction (or from a
    // misbehaving thread) sees false and does nothing. Capture `alive` by
    // value so the flag outlives this stack frame.
    const std::shared_ptr<std::atomic<bool>> alive = aliveFlag_;
    source_->requestRender(request, render::RenderPriority::Visible,
                           [this, alive](core::Result<core::Bitmap> /*result*/) {
                               if (alive->load(std::memory_order_acquire)) invalidate();
                           });
}

} // namespace rivet::ui
