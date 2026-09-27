// SPDX-License-Identifier: MPL-2.0
#include "ui/PageThumbnailList.hpp"

#include "core/Bitmap.hpp"
#include "core/geometry/Insets.hpp"
#include "render/PageLayout.hpp"
#include "render/RenderScaleKey.hpp"
#include "render/RenderSource.hpp"
#include "render/TileKey.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Color kBackgroundColor = Color::gray(0.949);
constexpr Color kRowSelectionFill = Color::rgba(0.0, 0.0, 0.0, 0.10);
constexpr Color kPageSelectedFill = Color::rgba(0.20, 0.45, 0.95, 0.22);
constexpr Color kActiveOutline = Color::rgba(0.20, 0.45, 0.95, 0.90);
constexpr Color kActiveOutlineUnfocused = Color::rgba(0.20, 0.45, 0.95, 0.45);
constexpr Color kInsertionColor = Color::rgba(0.20, 0.45, 0.95, 1.0);
constexpr double kInsertionThickness = 3.0;
constexpr Color kThumbnailPlaceholder = Color::gray(0.88);
constexpr Color kThumbnailBorder = Color::gray(0.72);
constexpr Color kLabelColor = Color::rgba(0.10, 0.10, 0.10, 1.0);
constexpr Font kLabelFont{11.0, Font::Weight::Regular};
constexpr const char* kEmptyStateText = "No document open";
constexpr Font kEmptyStateFont{13.0, Font::Weight::Regular};

double thumbnailAspectHeight(const render::PageLayout::PageInfo& page) {
    return PageThumbnailList::kThumbnailWidth *
           (page.sizePoints.height / std::max(1.0, page.sizePoints.width));
}

double rowHeightFor(const render::PageLayout::PageInfo& page) {
    return thumbnailAspectHeight(page) + PageThumbnailList::kLabelHeight +
           2.0 * PageThumbnailList::kRowPadding;
}

} // namespace

PageThumbnailList::PageThumbnailList()
    : aliveFlag_(std::make_shared<std::atomic<bool>>(true)) {
    auto bar = std::make_unique<ScrollBar>(ScrollOrientation::Vertical);
    scrollBar_ = bar.get();
    addChild(std::move(bar));
    scrollBar_->setOnScroll([this](double offset) { setScrollOffset(offset); });
}

PageThumbnailList::~PageThumbnailList() {
    aliveFlag_->store(false, std::memory_order_release);
}

void PageThumbnailList::setDocument(core::DocumentId documentId, const render::PageLayout* layout,
                                    render::IRenderSource* source,
                                    std::function<std::uint64_t()> revisionProvider) {
    documentId_ = documentId;
    layout_ = layout;
    source_ = source;
    revisionProvider_ = std::move(revisionProvider);
    selectedIndex_.reset();
    selectedRows_.clear();
    activeRow_.reset();
    scrollOffset_ = 0.0;
    cancelDrag();
    rebuildRowGeometry();
    syncScrollbar();
    positionScrollbar();
    invalidate();
}

void PageThumbnailList::clearDocument() {
    setDocument(core::DocumentId{}, nullptr, nullptr, {});
}

// Prefix sums of the varying row heights: O(1) rowTop and O(log n) row
// lookup regardless of page count. Nothing is rendered or requested here.
void PageThumbnailList::rebuildRowGeometry() {
    prefixHeights_.clear();
    if (layout_ == nullptr) return;
    prefixHeights_.reserve(layout_->pageCount());
    double top = 0.0;
    for (const render::PageLayout::PageInfo& page : layout_->pages()) {
        prefixHeights_.push_back(top);
        top += rowHeightFor(page);
    }
}

void PageThumbnailList::reloadPages() {
    cancelDrag();
    rebuildRowGeometry();
    const std::size_t count = rowCount();
    if (selectedIndex_ && *selectedIndex_ >= count) selectedIndex_.reset();
    if (activeRow_ && *activeRow_ >= count) activeRow_.reset();
    if (selectedRows_.size() > count) selectedRows_.resize(count);
    // Re-clamp the offset against the new content height.
    const double maxOffset = std::max(0.0, contentHeight() - bounds().size.height);
    scrollOffset_ = std::clamp(scrollOffset_, 0.0, maxOffset);
    syncScrollbar();
    invalidate();
}

void PageThumbnailList::setSelectedIndex(std::optional<std::size_t> index) {
    if (index && rowCount() > 0 && *index >= rowCount()) index.reset();
    if (selectedIndex_ == index) return;
    selectedIndex_ = index;
    invalidate();
}

void PageThumbnailList::setOnSelectionChanged(std::function<void(std::size_t)> onSelectionChanged) {
    onSelectionChanged_ = std::move(onSelectionChanged);
}

void PageThumbnailList::setPageSelection(std::vector<bool> selected, std::optional<std::size_t> active) {
    if (active && *active >= rowCount()) active.reset();
    if (selected.size() > rowCount()) selected.resize(rowCount());
    selectedRows_ = std::move(selected);
    activeRow_ = active;
    invalidate();
}

void PageThumbnailList::setOnRowClicked(std::function<void(std::size_t, ClickGesture)> onRowClicked) {
    onRowClicked_ = std::move(onRowClicked);
}

void PageThumbnailList::setOnNavigate(std::function<void(int, bool)> onNavigate) {
    onNavigate_ = std::move(onNavigate);
}

void PageThumbnailList::setOnDeleteRequested(std::function<void()> onDeleteRequested) {
    onDeleteRequested_ = std::move(onDeleteRequested);
}

void PageThumbnailList::setOnSelectAllRequested(std::function<void()> onSelectAllRequested) {
    onSelectAllRequested_ = std::move(onSelectAllRequested);
}

void PageThumbnailList::setOnMoveRequested(std::function<void(std::size_t)> onMoveRequested) {
    onMoveRequested_ = std::move(onMoveRequested);
}

void PageThumbnailList::setOnFocusRequested(std::function<void()> onFocusRequested) {
    onFocusRequested_ = std::move(onFocusRequested);
}

void PageThumbnailList::setPageLabels(std::vector<std::string> labels) {
    pageLabels_ = std::move(labels);
    invalidate();
}

void PageThumbnailList::revealPage(std::size_t index) {
    if (index >= rowCount()) return;
    const core::Rect row = rowRect(index);
    const double viewportHeight = bounds().size.height;
    if (row.minY() < scrollOffset_) {
        setScrollOffset(row.minY() - PageThumbnailList::kRowPadding);
    } else if (row.maxY() > scrollOffset_ + viewportHeight) {
        setScrollOffset(row.maxY() - viewportHeight + PageThumbnailList::kRowPadding);
    }
}

void PageThumbnailList::cancelDrag() {
    const bool wasActive = dragging_ || pressedRow_.has_value();
    dragging_ = false;
    pressedRow_.reset();
    pressCanDrag_ = false;
    deferredReplace_ = false;
    if (wasActive) invalidate();
}

double PageThumbnailList::contentHeight() const {
    if (rowCount() == 0) return 0.0;
    return prefixHeights_.back() + rowHeightFor(layout_->pages()[rowCount() - 1]);
}

std::size_t PageThumbnailList::rowCount() const {
    return layout_ != nullptr ? layout_->pageCount() : 0;
}

core::Rect PageThumbnailList::rowRect(std::size_t index) const {
    const double top = prefixHeights_[index];
    const double height = rowHeightFor(layout_->pages()[index]);
    return core::Rect{0.0, top, std::max(0.0, bounds().size.width), height};
}

core::Rect PageThumbnailList::rowFrame(std::size_t index) const {
    return rowRect(index).translated(core::Point{0.0, -scrollOffset_});
}

core::Rect PageThumbnailList::thumbnailRect(std::size_t index) const {
    const double thumbHeight = thumbnailAspectHeight(layout_->pages()[index]);
    const double left = (bounds().size.width - PageThumbnailList::kThumbnailWidth) / 2.0;
    return core::Rect{left, prefixHeights_[index] + PageThumbnailList::kRowPadding,
                      PageThumbnailList::kThumbnailWidth, thumbHeight};
}

// Visible row range via binary search over the prefix-sum table: first is the
// row crossing the viewport top (rows are ordered by top, so lower_bound on
// the tops and step back one row), last the last row starting above the
// viewport bottom. Both widened by kVisibleMarginRows. O(log n), never a scan
// over the whole page list.
std::pair<std::size_t, std::size_t> PageThumbnailList::visibleRowRange() const {
    const std::size_t count = rowCount();
    if (count == 0) return {0, 0};
    const double viewTop = scrollOffset_;
    const double viewBottom = scrollOffset_ + bounds().size.height;

    std::size_t first = 0;
    {
        const auto lb = std::lower_bound(prefixHeights_.begin(), prefixHeights_.end(), viewTop);
        const auto idx = static_cast<std::size_t>(lb - prefixHeights_.begin());
        first = idx == 0 ? 0 : idx - 1;
    }
    std::size_t last = first;
    {
        const auto lb = std::lower_bound(prefixHeights_.begin(), prefixHeights_.end(), viewBottom);
        const auto idx = static_cast<std::size_t>(lb - prefixHeights_.begin());
        last = idx == 0 ? first : std::min(count - 1, idx - 1);
    }

    first = first > PageThumbnailList::kVisibleMarginRows
                ? first - PageThumbnailList::kVisibleMarginRows
                : 0;
    last = std::min(count - 1, last + PageThumbnailList::kVisibleMarginRows);
    return {first, last};
}

std::optional<std::size_t> PageThumbnailList::rowIndexAt(const core::Point& localPoint) const {
    const std::size_t count = rowCount();
    if (count == 0) return std::nullopt;
    const double y = localPoint.y + scrollOffset_;
    // Index of the last row whose top is <= y (upper_bound - 1).
    const auto ub = std::upper_bound(prefixHeights_.begin(), prefixHeights_.end(), y);
    if (ub == prefixHeights_.begin()) return std::nullopt;
    const auto index = static_cast<std::size_t>(ub - prefixHeights_.begin()) - 1;
    if (y < prefixHeights_[index] + rowHeightFor(layout_->pages()[index])) return index;
    return std::nullopt;
}

// Gap before the row under the point when the point is in the row's upper
// half, after it otherwise; above the list = 0, below it = rowCount.
std::size_t PageThumbnailList::gapAt(const core::Point& localPoint) const {
    const std::size_t count = rowCount();
    if (count == 0) return 0;
    const double y = localPoint.y + scrollOffset_;
    if (y <= 0.0) return 0;
    const auto ub = std::upper_bound(prefixHeights_.begin(), prefixHeights_.end(), y);
    if (ub == prefixHeights_.begin()) return 0;
    const auto index = static_cast<std::size_t>(ub - prefixHeights_.begin()) - 1;
    const double middle = prefixHeights_[index] + rowHeightFor(layout_->pages()[index]) / 2.0;
    return y < middle ? index : std::min(count, index + 1);
}

void PageThumbnailList::setScrollOffset(double offset) {
    const double maxOffset = std::max(0.0, contentHeight() - bounds().size.height);
    const double clamped = std::isfinite(offset) ? std::clamp(offset, 0.0, maxOffset) : 0.0;
    if (clamped == scrollOffset_) return;
    scrollOffset_ = clamped;
    scrollBar_->setOffset(scrollOffset_);
    invalidate();
}

void PageThumbnailList::syncScrollbar() {
    scrollBar_->setExtents(bounds().size.height, contentHeight());
    scrollBar_->setOffset(scrollOffset_);
}

void PageThumbnailList::positionScrollbar() {
    const core::Rect area = bounds();
    const double thickness = ScrollBar::kThickness;
    scrollBar_->setFrame(core::Rect{area.maxX() - thickness - 2.0, 2.0, thickness,
                                    std::max(0.0, area.size.height - 4.0)});
}

bool PageThumbnailList::onMouse(const PointerEvent& event) {
    if (event.type == PointerEventType::Scroll) {
        setScrollOffset(scrollOffset_ + event.scrollDelta.y);
        if (dragging_) updateDrag(event.position);
        event.accepted = true;
        return true;
    }

    if (event.type == PointerEventType::Down && event.button == 1) {
        cancelDrag(); // a stale press (released outside the list) never lingers
        // The scrollbar child keeps its own press/drag handling.
        if (scrollBar_->frame().contains(event.position) && Widget::onMouse(event)) return true;
        if (onFocusRequested_) onFocusRequested_();
        // Rows are laid out in content space; convert the local point.
        const core::Point contentPoint{event.position.x, event.position.y + scrollOffset_};
        const std::optional<std::size_t> index = rowIndexAt(contentPoint);
        if (!index.has_value()) return Widget::onMouse(event);

        ClickGesture gesture = ClickGesture::Replace;
        if (event.modifiers.shift) {
            gesture = ClickGesture::Extend;
        } else if (event.modifiers.command || event.modifiers.control) {
            gesture = ClickGesture::Toggle;
        }
        // A plain press on an already selected row may start a drag of the
        // whole selection: its Replace is deferred to the release.
        deferredReplace_ = gesture == ClickGesture::Replace && isRowSelected(*index);
        if (!deferredReplace_) {
            if (onRowClicked_) onRowClicked_(*index, gesture);
            if (onSelectionChanged_) onSelectionChanged_(*index);
        }
        pressedRow_ = index;
        pressPoint_ = event.position;
        // The owner pushed the resulting selection synchronously: only a
        // selected row can be dragged.
        pressCanDrag_ = isRowSelected(*index);
        event.accepted = true;
        return true;
    }

    if (event.type == PointerEventType::Move) {
        if (event.button != 1) {
            // The button was released outside the list: nothing to drop.
            if (dragging_ || pressedRow_) cancelDrag();
            return Widget::onMouse(event);
        }
        if (dragging_) {
            updateDrag(event.position);
            event.accepted = true;
            return true;
        }
        if (pressedRow_ && pressCanDrag_) {
            const double dx = event.position.x - pressPoint_.x;
            const double dy = event.position.y - pressPoint_.y;
            if (dx * dx + dy * dy >= kDragThreshold * kDragThreshold) {
                dragging_ = true;
                deferredReplace_ = false;
                updateDrag(event.position);
            }
            event.accepted = true;
            return true;
        }
        return Widget::onMouse(event);
    }

    if (event.type == PointerEventType::Up && event.button == 1 && (dragging_ || pressedRow_)) {
        const bool dropped = dragging_;
        const std::size_t gap = dropGap_;
        const std::optional<std::size_t> row = pressedRow_;
        const bool deferred = deferredReplace_;
        dragging_ = false;
        pressedRow_.reset();
        pressCanDrag_ = false;
        deferredReplace_ = false;
        invalidate();
        if (dropped) {
            if (onMoveRequested_) onMoveRequested_(gap);
        } else if (deferred && row.has_value() && *row < rowCount()) {
            if (onRowClicked_) onRowClicked_(*row, ClickGesture::Replace);
            if (onSelectionChanged_) onSelectionChanged_(*row);
        }
        event.accepted = true;
        return true;
    }
    return Widget::onMouse(event);
}

// Autoscroll first (the step grows with the pointer's depth into the edge
// band), then the drop gap under the pointer in the scrolled list.
void PageThumbnailList::updateDrag(const core::Point& localPoint) {
    const double height = bounds().size.height;
    if (localPoint.y < kAutoscrollBand) {
        const double depth = std::clamp((kAutoscrollBand - localPoint.y) / kAutoscrollBand, 0.0, 1.0);
        setScrollOffset(scrollOffset_ - std::max(1.0, depth * kAutoscrollMaxStep));
    } else if (localPoint.y > height - kAutoscrollBand) {
        const double depth =
            std::clamp((localPoint.y - (height - kAutoscrollBand)) / kAutoscrollBand, 0.0, 1.0);
        setScrollOffset(scrollOffset_ + std::max(1.0, depth * kAutoscrollMaxStep));
    }
    dropGap_ = gapAt(localPoint);
    invalidate();
}

bool PageThumbnailList::onKey(const KeyEvent& event) {
    if (event.key == Key::Escape && (dragging_ || pressedRow_)) {
        cancelDrag();
        event.accepted = true;
        return true;
    }
    if (rowCount() == 0) return false;
    const bool command = event.modifiers.command || event.modifiers.control;
    switch (event.key) {
    case Key::Up:
    case Key::Left:
    case Key::Down:
    case Key::Right: {
        if (command || event.modifiers.option) return false;
        const int delta = (event.key == Key::Up || event.key == Key::Left) ? -1 : 1;
        if (onNavigate_) onNavigate_(delta, event.modifiers.shift);
        if (activeRow_) revealPage(*activeRow_);
        event.accepted = true;
        return true;
    }
    case Key::Delete:
    case Key::Backspace:
        if (command || event.modifiers.option) return false;
        if (onDeleteRequested_) onDeleteRequested_();
        event.accepted = true;
        return true;
    case Key::Character:
        if (command && !event.modifiers.shift && (event.text == "a" || event.text == "A")) {
            if (onSelectAllRequested_) onSelectAllRequested_();
            event.accepted = true;
            return true;
        }
        return false;
    default:
        return false;
    }
}

// Thumbnail cache identity: TileKey{documentId, pageId, scaleKey, 0, 0,
// contentRevision} with the scale quantized UP so the raster is never below
// the on-screen size; the painter scales down by less than 1/64, exactly like
// the viewport. The raster covers the whole page in one entry (a single
// low-resolution raster) and shares the bounded tile cache with viewport
// tiles. contentRevision makes a rotated/cropped page a new identity.
render::PhysicalRenderScaleKey
PageThumbnailList::thumbnailScaleKey(std::size_t index, double backingScale) const {
    const render::PageLayout::PageInfo& page = layout_->pages()[index];
    const double scale = PageThumbnailList::kThumbnailWidth / std::max(1.0, page.sizePoints.width);
    return render::PhysicalRenderScaleKey::fromDensities(
        render::RenderScaleKey::fromZoom(scale).scale(), backingScale);
}

void PageThumbnailList::requestThumbnail(std::size_t index, double backingScale,
                                         render::RenderPriority priority) const {
    const render::PageLayout::PageInfo& page = layout_->pages()[index];
    const render::PhysicalRenderScaleKey scaleKey = thumbnailScaleKey(index, backingScale);
    const render::TileKey tileKey{documentId_, page.id, scaleKey, 0, 0, page.contentRevision};
    const render::RenderRequest request{
        tileKey,
        render::RasterParams{core::Rect{core::Point{}, page.sizePoints}, scaleKey.scale()}};
    const std::shared_ptr<std::atomic<bool>> alive = aliveFlag_;
    source_->requestRender(request, priority,
                           [this, alive](render::RenderResult) {
                               if (alive->load(std::memory_order_acquire)) invalidate();
                           });
}

void PageThumbnailList::layout() {
    positionScrollbar();
    syncScrollbar();
}

void PageThumbnailList::paintSelf(PaintContext& context) const {
    context.fillRect(bounds(), kBackgroundColor);
    context.pushClip(bounds());

    if (layout_ == nullptr || source_ == nullptr || rowCount() == 0) {
        context.drawText(kEmptyStateText, bounds(), kEmptyStateFont, Color::gray(0.45),
                         TextAlign::Center);
        context.popClip();
        return;
    }

    const std::uint64_t revision = revisionProvider_ ? revisionProvider_() : 0;
    const auto [first, last] = visibleRowRange();
    const core::Point scrollShift{0.0, -scrollOffset_};

    for (std::size_t i = first; i <= last; ++i) {
        const core::Rect row = rowRect(i).translated(scrollShift);
        const core::Rect rowHighlight = row.inset(core::Insets::horizontal(4.0));
        // Current page (viewport tracking), then the page selection above it.
        if (selectedIndex_ && *selectedIndex_ == i) {
            context.fillRoundedRect(rowHighlight, kRowSelectionFill, 5.0);
        }
        if (isRowSelected(i)) context.fillRoundedRect(rowHighlight, kPageSelectedFill, 5.0);
        if (activeRow_ && *activeRow_ == i) {
            context.strokeRect(rowHighlight, isFocused() ? kActiveOutline : kActiveOutlineUnfocused, 2.0);
        }

        const core::Rect thumb = thumbnailRect(i).translated(scrollShift);
        const render::PageLayout::PageInfo& page = layout_->pages()[i];
        const render::PhysicalRenderScaleKey scaleKey = thumbnailScaleKey(i, context.backingScale());
        const render::TileKey tileKey{documentId_, page.id, scaleKey, 0, 0, page.contentRevision};
        if (const std::shared_ptr<const core::Bitmap> bitmap = source_->cachedTile(tileKey, revision)) {
            context.drawBitmap(*bitmap, thumb);
        } else {
            context.fillRect(thumb, kThumbnailPlaceholder);
            requestThumbnail(i, context.backingScale(),
                             (selectedIndex_ && *selectedIndex_ == i)
                                 ? render::RenderPriority::Impending
                                 : render::RenderPriority::Prefetch);
        }
        context.strokeRect(thumb, kThumbnailBorder, 1.0);

        // Page label from the PDF page-label tree when present, else the
        // 1-based positional page number.
        const std::string labelText =
            (i < pageLabels_.size() && !pageLabels_[i].empty())
                ? pageLabels_[i]
                : std::format("Page {}", i + 1);
        context.drawText(labelText,
                         core::Rect{0.0, thumb.maxY(), bounds().size.width, kLabelHeight},
                         kLabelFont, kLabelColor, TextAlign::Center);
    }

    // Drop insertion indicator: a line in the gap between two rows (top of
    // row `gap`, or the bottom of the last row).
    if (dragging_) {
        const double contentY = dropGap_ < rowCount() ? prefixHeights_[dropGap_] : contentHeight();
        const double y = contentY - scrollOffset_;
        constexpr double kInset = 10.0;
        context.fillRect(core::Rect{kInset, y - kInsertionThickness / 2.0,
                                    std::max(0.0, bounds().size.width - 2.0 * kInset),
                                    kInsertionThickness},
                         kInsertionColor);
    }

    context.popClip();
}

} // namespace rivet::ui
