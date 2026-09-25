// SPDX-License-Identifier: MPL-2.0
#include "ui/PdfViewport.hpp"

#include "core/Error.hpp"
#include "render/PageLayout.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
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

// Arrow-key scroll distance in logical screen points per press.
constexpr double kArrowScrollPoints = 40.0;

// Number of kTileSize cells needed to cover a device-pixel extent.
std::uint32_t tileCount(double deviceExtent) {
    if (!(deviceExtent > 0.0)) return 0;
    return static_cast<std::uint32_t>(std::ceil(deviceExtent / static_cast<double>(PdfViewport::kTileSize)));
}

// Backs the last visible tile index off by this much (in points) so a tile
// boundary landing exactly on the visible edge does not spawn the next tile.
// Far above double rounding noise at page scales, far below any meaningful
// fraction of a point.
constexpr double kEdgeEpsilon = 1e-7;

// Visible tile index range along one axis: floor of the first/last visible
// coordinate over the tile extent, clamped to [0, axisTileCount - 1]. The
// signed 64-bit intermediates keep floor() results well-defined before the
// narrowing casts - a negative floor (razor-thin sliver at the axis origin)
// must never be cast through an unsigned type. Inputs are page-local points,
// so the unclamped floors are bounded by the grid.
std::pair<std::uint32_t, std::uint32_t> visibleTileRange(double minPoints, double maxPoints,
                                                         double tileExtentPoints,
                                                         std::uint32_t axisTileCount) {
    const std::int64_t maxIndex = static_cast<std::int64_t>(axisTileCount) - 1;
    const std::int64_t first = static_cast<std::int64_t>(std::floor(minPoints / tileExtentPoints));
    const std::int64_t last =
        static_cast<std::int64_t>(std::floor((maxPoints - kEdgeEpsilon) / tileExtentPoints));
    return {static_cast<std::uint32_t>(std::clamp(first, std::int64_t{0}, maxIndex)),
            static_cast<std::uint32_t>(std::clamp(last, std::int64_t{0}, maxIndex))};
}

} // namespace

PdfViewport::PdfViewport()
    : state_(&emptyStateState_),
      aliveFlag_(std::make_shared<std::atomic<bool>>(true)) {
    // Scrollbar children overlay the content near the edges; frames are
    // managed by layout().
    auto vBar = std::make_unique<ScrollBar>(ScrollOrientation::Vertical);
    auto hBar = std::make_unique<ScrollBar>(ScrollOrientation::Horizontal);
    vScrollBar_ = vBar.get();
    hScrollBar_ = hBar.get();
    addChild(std::move(vBar));
    addChild(std::move(hBar));

    vScrollBar_->setOnScroll([this](double offset) {
        setScrollOffsetPoints(core::Point{scrollOffsetPoints().x, offset});
    });
    hScrollBar_->setOnScroll([this](double offset) {
        setScrollOffsetPoints(core::Point{offset, scrollOffsetPoints().y});
    });

    // Single funnel for zoom and scroll changes: repaint + dropping queued
    // renders on zoom change + scrollbar sync + current-page tracking +
    // status notification. The alive flag turns stale callbacks from a
    // previously bound state into no-ops after destruction.
    const std::shared_ptr<std::atomic<bool>> alive = aliveFlag_;
    emptyStateState_.setCallback([this, alive] {
        if (alive->load(std::memory_order_acquire)) onStateChanged();
    });
}

PdfViewport::~PdfViewport() {
    // In-flight render callbacks may outlive the widget; the shared flag
    // turns them into no-ops (see the threading contract in the header).
    aliveFlag_->store(false, std::memory_order_release);
    // Disconnect any state still pointing at this viewport.
    if (state_ != nullptr) state_->setCallback({});
    state_ = nullptr;
}

void PdfViewport::setDocument(core::DocumentId documentId, const render::PageLayout* layout,
                              render::IRenderSource* source,
                              std::function<std::uint64_t()> revisionProvider,
                              render::ViewerState* viewState) {
    if (source_ != nullptr && source_ != source) source_->cancelAll();

    // Disconnect the previously bound state so late changes from it no longer
    // drive this viewport.
    if (state_ != nullptr && state_ != &emptyStateState_) state_->setCallback({});

    documentId_ = documentId;
    layout_ = layout;
    source_ = source;
    revisionProvider_ = std::move(revisionProvider);

    if (viewState != nullptr) {
        state_ = viewState;
        const std::shared_ptr<std::atomic<bool>> alive = aliveFlag_;
        state_->setCallback([this, alive] {
            if (alive->load(std::memory_order_acquire)) onStateChanged();
        });
    } else {
        state_ = &emptyStateState_;
    }

    // Restored offsets may exceed the new document's bounds.
    setScrollOffsetPoints(state_->scrollOffsetPoints());
    currentPage_ = 0;
    syncScrollbars();
    updateCurrentPage();
    invalidate();
}

void PdfViewport::clearDocument() {
    if (source_ != nullptr) source_->cancelAll();
    if (state_ != nullptr && state_ != &emptyStateState_) state_->setCallback({});
    documentId_ = core::DocumentId{};
    layout_ = nullptr;
    source_ = nullptr;
    revisionProvider_ = nullptr;
    state_ = &emptyStateState_;
    emptyStateState_.setScrollOffsetPoints(core::Point{});
    currentPage_ = 0;
    syncScrollbars();
    updateCurrentPage();
    invalidate();
}

bool PdfViewport::zoomInStep() {
    if (layout_ == nullptr) return false;
    state_->zoom().setFitMode(render::ZoomState::FitMode::None);
    return state_->zoom().zoomIn();
}

bool PdfViewport::zoomOutStep() {
    if (layout_ == nullptr) return false;
    state_->zoom().setFitMode(render::ZoomState::FitMode::None);
    return state_->zoom().zoomOut();
}

void PdfViewport::zoomActualSize() {
    if (layout_ == nullptr) return;
    state_->zoom().setFitMode(render::ZoomState::FitMode::None);
    state_->zoom().actualSize();
}

bool PdfViewport::setManualZoom(double zoom) {
    if (layout_ == nullptr) return false;
    state_->zoom().setFitMode(render::ZoomState::FitMode::None);
    return state_->zoom().setZoom(zoom);
}

void PdfViewport::setFitMode(render::ZoomState::FitMode mode) {
    if (layout_ == nullptr) return;
    state_->zoom().setFitMode(mode);
    layout();
}

core::Point PdfViewport::clampedScrollOffset(const core::Point& offset) const {
    if (layout_ == nullptr) return core::Point{};
    const core::Size content = layout_->contentSizePoints();
    const core::Size visibleExtent = frame().size / zoom().zoom();
    const auto clampAxis = [](double value, double contentExtent, double visibleExtentPoints) {
        const double maxOffset = std::max(0.0, contentExtent - visibleExtentPoints);
        return std::clamp(value, 0.0, maxOffset);
    };
    return core::Point{clampAxis(offset.x, content.width, visibleExtent.width),
                       clampAxis(offset.y, content.height, visibleExtent.height)};
}

core::Rect PdfViewport::visibleContentRectPoints() const {
    if (layout_ == nullptr) return core::Rect{};
    return core::Rect{state_->scrollOffsetPoints(), frame().size / state_->zoom().zoom()};
}

void PdfViewport::setScrollOffsetPoints(const core::Point& offset) {
    const core::Point clamped = clampedScrollOffset(offset);
    state_->setScrollOffsetPoints(clamped);
    // The state's callback already invalidated via onStateChanged when the
    // value changed; sync here too so the scrollbars always mirror the state.
    syncScrollbars();
}

void PdfViewport::scrollByContentPoints(const core::Point& delta) {
    setScrollOffsetPoints(state_->scrollOffsetPoints() + delta);
}

void PdfViewport::goToPage(std::size_t index) {
    if (layout_ == nullptr || index >= layout_->pageCount()) return;
    setScrollOffsetPoints(core::Point{state_->scrollOffsetPoints().x,
                                      layout_->pageTopOffsetPoints(index)});
}

void PdfViewport::setZoomChangedCallback(std::function<void(double)> onZoomChanged) {
    onZoomChanged_ = std::move(onZoomChanged);
    // Sync the baseline so the first state change reports only real zoom
    // changes (and does not spuriously cancel queued renders). No immediate
    // fire: the shell also reads zoom() directly when binding a tab.
    lastReportedZoom_ = state_->zoom().zoom();
}

void PdfViewport::setCurrentPageChangedCallback(std::function<void(std::size_t)> onPageChanged) {
    onPageChanged_ = std::move(onPageChanged);
}

void PdfViewport::setOnFocusRequested(std::function<void()> onFocusRequested) {
    onFocusRequested_ = std::move(onFocusRequested);
}

void PdfViewport::onStateChanged() {
    const double zoomValue = state_->zoom().zoom();
    if (zoomValue != lastReportedZoom_) {
        // Queued renders were requested at the previous scale; drop them so
        // the next paint re-requests tiles at the new scale instead of
        // delivering stale ones. cancelAll fires Cancelled callbacks, which
        // only invalidate - no re-entry into zoom logic. The in-flight job,
        // if any, finishes into the cache under its own key. source_ is
        // non-null only between setDocument() and clearDocument().
        if (source_ != nullptr) source_->cancelAll();
        lastReportedZoom_ = zoomValue;
        if (onZoomChanged_) onZoomChanged_(zoomValue);
    }
    syncScrollbars();
    updateCurrentPage();
    invalidate();
}

void PdfViewport::syncScrollbars() {
    if (layout_ == nullptr) {
        vScrollBar_->setExtents(0.0, 0.0);
        hScrollBar_->setExtents(0.0, 0.0);
        vScrollBar_->setOffset(0.0);
        hScrollBar_->setOffset(0.0);
        return;
    }
    const core::Size content = layout_->contentSizePoints();
    const core::Size visible = frame().size / state_->zoom().zoom();
    vScrollBar_->setExtents(visible.height, content.height);
    vScrollBar_->setOffset(state_->scrollOffsetPoints().y);
    hScrollBar_->setExtents(visible.width, content.width);
    hScrollBar_->setOffset(state_->scrollOffsetPoints().x);
}

void PdfViewport::updateCurrentPage() {
    if (layout_ == nullptr || layout_->pageCount() == 0) {
        if (currentPage_ != 0) {
            currentPage_ = 0;
            if (onPageChanged_) onPageChanged_(0);
        }
        return;
    }
    const std::optional<std::size_t> current =
        layout_->currentPageIndex(visibleContentRectPoints());
    const std::size_t page = current.value_or(0);
    if (page != currentPage_) {
        currentPage_ = page;
        if (onPageChanged_) onPageChanged_(page);
    }
    // Keep the text of the tracked page (and its neighbors) warm so click
    // selection and search hit a loaded page.
    if (textBridge_ != nullptr && layout_ != nullptr && layout_->pageCount() > 0) {
        textBridge_->warmPage(currentPage_);
        if (currentPage_ > 0) textBridge_->warmPage(currentPage_ - 1);
        if (currentPage_ + 1 < layout_->pageCount()) textBridge_->warmPage(currentPage_ + 1);
    }
}

// Maps a viewport-local point to the page display point it lands on: finds
// the visible page whose frame (in viewport space) contains the point, then
// converts to page-local display points by inverting the frame mapping.
std::optional<std::pair<std::size_t, core::Point>> PdfViewport::pagePointAt(const core::Point& localPoint) const {
    if (layout_ == nullptr || source_ == nullptr) return std::nullopt;
    const double zoomFactor = state_->zoom().zoom();
    const core::Rect contentRect{state_->scrollOffsetPoints(), frame().size / zoomFactor};
    const std::optional<std::pair<std::size_t, std::size_t>> visible =
        layout_->visiblePageRange(contentRect);
    if (!visible.has_value()) return std::nullopt;
    for (std::size_t i = visible->first; i <= visible->second; ++i) {
        const core::Rect pageFrame = layout_->pageFramePoints(i);
        const core::Rect pageInViewport{(pageFrame.origin - state_->scrollOffsetPoints()) * zoomFactor,
                                        pageFrame.size * zoomFactor};
        if (pageInViewport.contains(localPoint)) {
            const core::Point pagePoint = (localPoint - pageInViewport.origin) / zoomFactor +
                                          core::Point{};
            // pagePoint is relative to the page frame's top-left, which in
            // page display space is (0, 0): exactly the display-space point.
            return std::pair<std::size_t, core::Point>{i, pagePoint};
        }
    }
    return std::nullopt;
}

void PdfViewport::revealContentRect(std::size_t pageIndex, const core::Rect& pageRectPoints) {
    if (layout_ == nullptr || pageIndex >= layout_->pageCount()) return;
    const double zoomFactor = state_->zoom().zoom();
    const core::Rect pageFrame = layout_->pageFramePoints(pageIndex);
    // Target: page rect center at the viewport center.
    const core::Point targetContent = pageFrame.origin + pageRectPoints.center();
    const core::Point halfViewport{frame().size.width / (2.0 * zoomFactor),
                                   frame().size.height / (2.0 * zoomFactor)};
    const core::Point desired = targetContent - halfViewport;
    setScrollOffsetPoints(desired);
}

bool PdfViewport::onMouse(const PointerEvent& event) {
    if (event.type == PointerEventType::Scroll) {
        if (layout_ == nullptr) return false;

        if (event.modifiers.command || event.modifiers.control) {
            // Pinch gesture: stepped zoom anchored at the pointer so the
            // content underneath stays approximately stationary. Manual zoom
            // exits an active fit mode.
            const double oldZoom = state_->zoom().zoom();
            state_->zoom().setFitMode(render::ZoomState::FitMode::None);
            const bool changed = event.scrollDelta.y < 0.0 ? state_->zoom().zoomIn()
                                                           : state_->zoom().zoomOut();
            if (changed) {
                const double newZoom = state_->zoom().zoom();
                const core::Point contentAnchor =
                    state_->scrollOffsetPoints() + event.position / oldZoom;
                setScrollOffsetPoints(contentAnchor - event.position / newZoom);
            }
            event.accepted = true;
            return true;
        }

        // Plain scroll: wheel delta is in logical pixels; convert to content
        // points (positive delta = content moves up/left = offset grows).
        scrollByContentPoints(event.scrollDelta / state_->zoom().zoom());
        event.accepted = true;
        return true;
    }

    // Link interaction through the bridge: a press over a link arms it and
    // fires on release over the SAME link (button-like drag-out cancels).
    // Links take precedence over text selection.
    if (textBridge_ != nullptr && layout_ != nullptr && !selecting_) {
        if (event.type == PointerEventType::Down && event.button == 1) {
            const auto pagePoint = pagePointAt(event.position);
            if (pagePoint.has_value()) {
                const auto link = textBridge_->linkAtPoint(pagePoint->first, pagePoint->second);
                if (link.has_value()) {
                    linkPressed_ = true;
                    pressedLink_ = link;
                    event.accepted = true;
                    return true;
                }
            }
        } else if (event.type == PointerEventType::Up && event.button == 1 && linkPressed_) {
            linkPressed_ = false;
            const auto pagePoint = pagePointAt(event.position);
            const auto link = pagePoint.has_value()
                                  ? textBridge_->linkAtPoint(pagePoint->first, pagePoint->second)
                                  : std::nullopt;
            if (link.has_value() && pressedLink_.has_value() &&
                link->kind == pressedLink_->kind && link->pageIndex == pressedLink_->pageIndex &&
                link->url == pressedLink_->url) {
                pressedLink_.reset();
                textBridge_->linkActivated(*link);
            } else {
                pressedLink_.reset();
            }
            event.accepted = true;
            return true;
        } else if (event.type == PointerEventType::Move && event.button == 0) {
            // Hover: repaint when the hover state changes.
            const auto pagePoint = pagePointAt(event.position);
            const bool hovered =
                pagePoint.has_value() &&
                textBridge_->linkAtPoint(pagePoint->first, pagePoint->second).has_value();
            if (hovered != linkHovered_) {
                linkHovered_ = hovered;
                event.accepted = true;
                invalidate();
                if (hovered) return true;
            }
            if (hovered) {
                event.accepted = true;
                return true;
            }
        }
    }

    // Text selection through the bridge (button 1, document bound).
    if (textBridge_ != nullptr && layout_ != nullptr) {
        if (event.type == PointerEventType::Down && event.button == 1) {
            const auto hit = pagePointAt(event.position);
            if (hit.has_value()) {
                const std::optional<std::uint32_t> charIndex =
                    textBridge_->charIndexAtPoint(hit->first, hit->second);
                if (charIndex.has_value()) {
                    selecting_ = true;
                    textBridge_->selectionDragBegan(hit->first, *charIndex, event.modifiers.shift);
                    event.accepted = true;
                    return true;
                }
            }
            // A plain click that hit no text clears the selection.
            textBridge_->selectionCleared();
        } else if (event.type == PointerEventType::Move && selecting_) {
            const auto hit = pagePointAt(event.position);
            if (hit.has_value()) {
                const std::optional<std::uint32_t> charIndex =
                    textBridge_->charIndexAtPoint(hit->first, hit->second);
                if (charIndex.has_value()) textBridge_->selectionDragMoved(hit->first, *charIndex);
            }
            event.accepted = true;
            return true;
        } else if (event.type == PointerEventType::Up && selecting_) {
            selecting_ = false;
            textBridge_->selectionDragEnded();
            event.accepted = true;
            return true;
        }
    }

    // Scrollbars (and later overlays) live in the children; route to them in
    // topmost-first order. A press that falls through to the content asks the
    // host for keyboard focus.
    if (Widget::onMouse(event)) return true;
    if (event.type == PointerEventType::Down && onFocusRequested_) onFocusRequested_();
    return false;
}

bool PdfViewport::onKey(const KeyEvent& event) {
    const bool zoomInKey = event.key == Key::Plus ||
                           (event.key == Key::Character && event.text == "=");
    const bool zoomOutKey = event.key == Key::Minus ||
                            (event.key == Key::Character && event.text == "-");
    if (layout_ != nullptr && (zoomInKey || zoomOutKey)) {
        if (zoomInKey) {
            zoomInStep();
        } else {
            zoomOutStep();
        }
        event.accepted = true;
        return true;
    }

    if (layout_ == nullptr) return false;

    // PageUp/PageDown scroll one viewport height (in content points); arrows
    // scroll a fixed screen distance (converted to content points).
    const double pageHeightPoints = frame().size.height / state_->zoom().zoom();
    const double arrowContentPoints = kArrowScrollPoints / state_->zoom().zoom();
    switch (event.key) {
    case Key::PageDown:
        scrollByContentPoints(core::Point{0.0, pageHeightPoints});
        break;
    case Key::PageUp:
        scrollByContentPoints(core::Point{0.0, -pageHeightPoints});
        break;
    case Key::Down:
        scrollByContentPoints(core::Point{0.0, arrowContentPoints});
        break;
    case Key::Up:
        scrollByContentPoints(core::Point{0.0, -arrowContentPoints});
        break;
    case Key::Right:
        scrollByContentPoints(core::Point{arrowContentPoints, 0.0});
        break;
    case Key::Left:
        scrollByContentPoints(core::Point{-arrowContentPoints, 0.0});
        break;
    case Key::Home:
        setScrollOffsetPoints(core::Point{state_->scrollOffsetPoints().x, 0.0});
        break;
    case Key::End:
        // Out-of-range values clamp to the content bounds.
        setScrollOffsetPoints(
            core::Point{state_->scrollOffsetPoints().x, std::numeric_limits<double>::max()});
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
    setScrollOffsetPoints(state_->scrollOffsetPoints());
    positionScrollbars();
    syncScrollbars();
    updateCurrentPage();
}

void PdfViewport::positionScrollbars() {
    const core::Rect area = bounds();
    const double thickness = ScrollBar::kThickness;
    // Vertical bar hugs the right edge, horizontal the bottom edge; when
    // both are usable they meet in the corner.
    const bool vUsable = vScrollBar_->isUsable();
    const bool hUsable = hScrollBar_->isUsable();
    const double vLength = std::max(0.0, area.size.height - (hUsable ? thickness + 2.0 : 4.0));
    const double hLength = std::max(0.0, area.size.width - (vUsable ? thickness + 2.0 : 4.0));
    vScrollBar_->setFrame(core::Rect{area.maxX() - thickness - 2.0, 2.0, thickness, vLength});
    hScrollBar_->setFrame(core::Rect{2.0, area.maxY() - thickness - 2.0, hLength, thickness});
}

void PdfViewport::resolveFitMode() {
    const render::ZoomState::FitMode mode = state_->zoom().fitMode();
    if (mode == render::ZoomState::FitMode::None) return;
    if (layout_ == nullptr || layout_->pageCount() == 0) {
        state_->zoom().setFitMode(render::ZoomState::FitMode::None);
        return;
    }
    if (mode == render::ZoomState::FitMode::Width) {
        double widest = 0.0;
        for (const render::PageLayout::PageInfo& page : layout_->pages()) {
            widest = std::max(widest, page.sizePoints.width);
        }
        state_->zoom().setZoom(state_->zoom().fitWidthZoom(frame().size.width, widest));
    } else { // FitMode::Page: fit the current page within the viewport
        const std::size_t index = std::min(currentPage_, layout_->pageCount() - 1);
        state_->zoom().setZoom(state_->zoom().fitPageZoom(
            frame().size.width, frame().size.height, layout_->pages()[index].sizePoints));
    }
    // Fit modes are modes: the intent survives this layout pass and keeps
    // recomputing on resize until manual zoom exits the mode.
}

// Paint algorithm:
//   1. Fill the canvas background.
//   2. Empty state (no layout/source): centered hint text; done.
//   3. contentRect = {scrollOffset, frame.size / zoom} in content
//      points; layout->visiblePageRange(contentRect) selects what to draw.
//   4. Each visible page: map its content-space frame into local viewport
//      coordinates ((frame.origin - scrollOffset) * zoom, size * zoom),
//      fill it white and stroke a border.
//   5. Tile pass per page (paintPageTiles): overlay the cached tiles and
//      request the missing ones, painting placeholders meanwhile.
//   6. Scrollbar children paint on top via Widget::paint's child pass.
void PdfViewport::paintSelf(PaintContext& context) const {
    context.pushClip(bounds());
    context.fillRect(bounds(), kCanvasBackground);

    if (layout_ == nullptr || source_ == nullptr) {
        context.drawText(kEmptyStateText, bounds(), kEmptyStateFont, kEmptyStateColor,
                         TextAlign::Center);
        context.popClip();
        return;
    }

    const double zoomFactor = state_->zoom().zoom();
    const core::Rect contentRect{state_->scrollOffsetPoints(), frame().size / zoomFactor};
    const std::optional<std::pair<std::size_t, std::size_t>> visible =
        layout_->visiblePageRange(contentRect);
    if (visible.has_value()) {
        const std::uint64_t revision = revisionProvider_ ? revisionProvider_() : 0;
        for (std::size_t i = visible->first; i <= visible->second; ++i) {
            const core::Rect pageFrame = layout_->pageFramePoints(i);
            const core::Rect pageInViewport{(pageFrame.origin - state_->scrollOffsetPoints()) * zoomFactor,
                                            pageFrame.size * zoomFactor};
            context.fillRect(pageInViewport, kPageBackground);
            context.strokeRect(pageInViewport, kPageBorder, 1.0);
            paintPageTiles(i, pageFrame, pageInViewport, contentRect, revision, context);
            paintPageOverlays(i, pageFrame, context);
        }
        // Limited nearby prefetch: one viewport-height band of the pages
        // adjacent to the visible range, scheduled behind visible tiles.
        prefetchNeighborPages(*visible, contentRect, revision, context);
    }
    context.popClip();
}

void PdfViewport::prefetchNeighborPages(const std::pair<std::size_t, std::size_t>& visibleRange,
                                        const core::Rect& contentRect, std::uint64_t revision,
                                        PaintContext& context) const {
    const double bandHeight = std::min(frame().size.height / state_->zoom().zoom(),
                                       std::numeric_limits<double>::max());
    if (visibleRange.second + 1 < layout_->pageCount()) {
        const core::Rect nextFrame = layout_->pageFramePoints(visibleRange.second + 1);
        const double height = std::min(bandHeight, nextFrame.size.height);
        requestBandTiles(visibleRange.second + 1,
                         core::Rect{nextFrame.origin.x, nextFrame.origin.y, contentRect.size.width,
                                    height},
                         revision, context);
    }
    if (visibleRange.first > 0) {
        const core::Rect prevFrame = layout_->pageFramePoints(visibleRange.first - 1);
        const double height = std::min(bandHeight, prevFrame.size.height);
        requestBandTiles(visibleRange.first - 1,
                         core::Rect{prevFrame.origin.x, prevFrame.maxY() - height,
                                    contentRect.size.width, height},
                         revision, context);
    }
}

// Tile enumeration mirrors paintPageTiles' request path (same cache identity
// math), minus the painting: misses are requested at Impending priority so
// the DocumentRenderer's lane ordering drains them only after Visible work.
void PdfViewport::requestBandTiles(std::size_t pageIndex, const core::Rect& bandContentRect,
                                   std::uint64_t revision, PaintContext& context) const {
    const render::PageLayout::PageInfo& info = layout_->pages()[pageIndex];
    const core::Rect pageFrame = layout_->pageFramePoints(pageIndex);
    const render::RenderScaleKey zoomKey = render::RenderScaleKey::fromZoom(state_->zoom().zoom());
    const render::PhysicalRenderScaleKey physicalKey =
        render::PhysicalRenderScaleKey::fromDensities(zoomKey.scale(), context.backingScale());
    const double devicePixelsPerPoint = physicalKey.scale();
    const double tileExtentPoints = static_cast<double>(kTileSize) / devicePixelsPerPoint;
    const std::uint32_t tilesX = tileCount(pageFrame.size.width * devicePixelsPerPoint);
    const std::uint32_t tilesY = tileCount(pageFrame.size.height * devicePixelsPerPoint);
    if (tilesX == 0 || tilesY == 0) return;

    const core::Rect pageBounds{core::Point{}, pageFrame.size};
    const core::Rect bandLocal = bandContentRect.intersection(pageFrame).translated(-pageFrame.origin);
    if (bandLocal.isEmpty()) return;

    const auto [txMin, txMax] =
        visibleTileRange(bandLocal.minX(), bandLocal.maxX(), tileExtentPoints, tilesX);
    const auto [tyMin, tyMax] =
        visibleTileRange(bandLocal.minY(), bandLocal.maxY(), tileExtentPoints, tilesY);
    for (std::uint32_t ty = tyMin; ty <= tyMax; ++ty) {
        for (std::uint32_t tx = txMin; tx <= txMax; ++tx) {
            const core::Rect tileRect{static_cast<double>(tx) * tileExtentPoints,
                                      static_cast<double>(ty) * tileExtentPoints,
                                      tileExtentPoints, tileExtentPoints};
            const core::Rect clipped = tileRect.intersection(pageBounds);
            if (clipped.isEmpty()) continue;
            const render::TileKey key{documentId_, info.id, physicalKey, tx, ty};
            if (source_->cachedTile(key, revision) == nullptr) {
                requestTileWithPriority(key, render::RasterParams{clipped, devicePixelsPerPoint},
                                        render::RenderPriority::Impending);
            }
        }
    }
}

void PdfViewport::requestTileWithPriority(const render::TileKey& key,
                                          const render::RasterParams& params,
                                          render::RenderPriority priority) const {
    const render::RenderRequest request{key, params};
    const std::shared_ptr<std::atomic<bool>> alive = aliveFlag_;
    source_->requestRender(request, priority,
                           [this, alive](render::RenderResult) {
                               if (alive->load(std::memory_order_acquire)) invalidate();
                           });
}

// Overlay pass (selection highlights, search matches): rects arrive in page
// display space from the bridge and map to viewport space exactly like tile
// destinations. Painted AFTER the tiles, never baked into them.
void PdfViewport::paintPageOverlays(std::size_t pageIndex, const core::Rect& pageFramePoints,
                                    PaintContext& context) const {
    if (textBridge_ == nullptr) return;
    if (linkHovered_) {
        // Subtle hover indication for the link under the pointer: stroked
        // rect over the tiles (never baked into them).
        for (const core::Rect& rect : textBridge_->linkRects(pageIndex)) {
            const double zoomFactor = state_->zoom().zoom();
            const core::Rect dest{
                (pageFramePoints.origin + rect.origin - state_->scrollOffsetPoints()) * zoomFactor,
                rect.size * zoomFactor};
            context.strokeRect(dest, Color::rgba(0.15, 0.4, 0.9, 0.8), 1.0);
        }
    }
    const std::vector<OverlayRect> overlays = textBridge_->overlayRects(pageIndex);
    if (overlays.empty()) return;
    const double zoomFactor = state_->zoom().zoom();
    for (const OverlayRect& overlay : overlays) {
        const core::Rect dest{(pageFramePoints.origin + overlay.rect.origin - state_->scrollOffsetPoints()) *
                                  zoomFactor,
                              overlay.rect.size * zoomFactor};
        context.fillRect(dest, overlay.color);
    }
}

// Tile geometry: tiles live on a kTileSize x kTileSize DEVICE-pixel grid
// anchored at the page's top-left. Page display coordinates are top-left
// origin / y-down (same orientation as viewport-local logical space), so the
// grid maps directly with no flip: the cell (tx, ty) covers page points
// {tx * E, ty * E, E, E} with E = kTileSize / devicePixelsPerPoint, where the
// density is the PhysicalRenderScaleKey of quantized zoom x display backing
// scale. Cache identity and RasterParams derive from that ONE key, so a cache
// entry's pixel dimensions can never disagree with its identity.
//
// Only tiles overlapping the visible region are visited: per axis the index
// range is [floor(min / E), floor((max - epsilon) / E)] clamped to the grid
// (see visibleTileRange), instead of sweeping the whole grid and skipping
// invisible cells - a large page under a small viewport must not touch every
// tile of the grid on each repaint.
//
// The raster rect recorded in RasterParams is the tile cell CLIPPED to the
// page bounds (page display points). Cache identity is stable across scrolls
// and repaints because the clip depends only on the page size; the bitmap
// returned by cachedTile() for a TileKey therefore rasters exactly
// params.pageRectPoints at params.devicePixelsPerPoint, and the viewport draws
// it scaled into that same region mapped to viewport space.
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
    const render::RenderScaleKey zoomKey = render::RenderScaleKey::fromZoom(state_->zoom().zoom());
    // Physical cache identity: quantized zoom x display backing scale. The
    // raster density below is DERIVED from this key (never the raw product),
    // so requests rasterizing at different pixel dimensions never share a
    // cache entry - 100% zoom on 1x and 2x displays are different identities.
    const render::PhysicalRenderScaleKey physicalKey =
        render::PhysicalRenderScaleKey::fromDensities(zoomKey.scale(), context.backingScale());
    const double devicePixelsPerPoint = physicalKey.scale();

    // Visible region inside the page, in page-local display points (culling).
    const core::Rect pageBounds{core::Point{}, pageFramePoints.size};
    const core::Rect visibleLocal =
        contentRect.intersection(pageFramePoints).translated(-pageFramePoints.origin);
    if (visibleLocal.isEmpty()) return;

    const double tileExtentPoints = static_cast<double>(kTileSize) / devicePixelsPerPoint;
    const std::uint32_t tilesX = tileCount(pageFramePoints.size.width * devicePixelsPerPoint);
    const std::uint32_t tilesY = tileCount(pageFramePoints.size.height * devicePixelsPerPoint);
    if (tilesX == 0 || tilesY == 0) return;

    const auto [txMin, txMax] =
        visibleTileRange(visibleLocal.minX(), visibleLocal.maxX(), tileExtentPoints, tilesX);
    const auto [tyMin, tyMax] =
        visibleTileRange(visibleLocal.minY(), visibleLocal.maxY(), tileExtentPoints, tilesY);

    for (std::uint32_t ty = tyMin; ty <= tyMax; ++ty) {
        for (std::uint32_t tx = txMin; tx <= txMax; ++tx) {
            const core::Rect tileRect{static_cast<double>(tx) * tileExtentPoints,
                                      static_cast<double>(ty) * tileExtentPoints,
                                      tileExtentPoints, tileExtentPoints};
            const core::Rect clipped = tileRect.intersection(pageBounds);
            if (clipped.isEmpty()) continue;

            const render::TileKey key{documentId_, info.id, physicalKey, tx, ty};
            const core::Rect dest{pageInViewport.origin + clipped.origin * state_->zoom().zoom(),
                                  clipped.size * state_->zoom().zoom()};
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
    requestTileWithPriority(key, params, render::RenderPriority::Visible);
}

} // namespace rivet::ui
