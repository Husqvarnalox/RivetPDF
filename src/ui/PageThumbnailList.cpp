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
    scrollOffset_ = 0.0;

    // Prefix sums of the varying row heights: O(1) rowTop and O(log n) row
    // lookup regardless of page count. Nothing is rendered or requested here.
    prefixHeights_.clear();
    if (layout_ != nullptr) {
        prefixHeights_.reserve(layout_->pageCount());
        double top = 0.0;
        for (const render::PageLayout::PageInfo& page : layout_->pages()) {
            prefixHeights_.push_back(top);
            top += rowHeightFor(page);
        }
    }

    syncScrollbar();
    positionScrollbar();
    invalidate();
}

void PageThumbnailList::clearDocument() {
    setDocument(core::DocumentId{}, nullptr, nullptr, {});
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
        event.accepted = true;
        return true;
    }
    if (event.type == PointerEventType::Down) {
        // Rows are laid out in content space; convert the local point.
        const core::Point contentPoint{event.position.x, event.position.y + scrollOffset_};
        const std::optional<std::size_t> index = rowIndexAt(contentPoint);
        if (index.has_value()) {
            setSelectedIndex(*index);
            if (onSelectionChanged_) onSelectionChanged_(*index);
            event.accepted = true;
            return true;
        }
    }
    return Widget::onMouse(event);
}

// Thumbnail cache identity: TileKey{documentId, pageId, scaleKey, 0, 0} with
// the scale quantized UP so the raster is never below the on-screen size; the
// painter scales down by less than 1/64, exactly like the viewport. The
// raster covers the whole page in one entry (a single low-resolution raster)
// and shares the bounded tile cache with viewport tiles.
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
    const render::TileKey tileKey{documentId_, page.id, scaleKey, 0, 0};
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
        if (selectedIndex_ && *selectedIndex_ == i) {
            context.fillRoundedRect(row.inset(core::Insets::horizontal(4.0)), kRowSelectionFill, 5.0);
        }

        const core::Rect thumb = thumbnailRect(i).translated(scrollShift);
        const render::PageLayout::PageInfo& page = layout_->pages()[i];
        const render::PhysicalRenderScaleKey scaleKey = thumbnailScaleKey(i, context.backingScale());
        const render::TileKey tileKey{documentId_, page.id, scaleKey, 0, 0};
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
        // 1-based physical page number.
        const std::string labelText =
            (i < pageLabels_.size() && !pageLabels_[i].empty())
                ? pageLabels_[i]
                : std::format("Page {}", i + 1);
        context.drawText(labelText,
                         core::Rect{0.0, thumb.maxY(), bounds().size.width, kLabelHeight},
                         kLabelFont, kLabelColor, TextAlign::Center);
    }

    context.popClip();
}

} // namespace rivet::ui
