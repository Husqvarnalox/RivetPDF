// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/Widget.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace rivet::render {
class PageLayout;
class IRenderSource;
} // namespace rivet::render

namespace rivet::ui {

// Vertical list of page thumbnails with lazy, virtualized rendering.
//
// Rendering strategy: thumbnails go through the SAME IRenderSource the
// viewport uses - one whole-page raster per page at a sidebar-fixed physical
// scale, so a normal page renders as a single low-resolution raster. Requests
// are made at Prefetch priority (Impending for the selected page's tile) so
// they can never delay visible viewport tiles (the DocumentRenderer drains
// higher priorities first). Cache identity is regular TileCache identity at
// the thumbnail scale key, so thumbnail rasters share the bounded tile cache;
// no second image cache exists.
//
// Virtualization: only the visible row range (plus a small margin) is
// painted; off-screen thumbnails are never requested. Row geometry comes from
// a precomputed prefix-sum table (O(log n) lookups), so documents with
// thousands of pages cost nothing until scrolled into view.
//
// Scrolling: internal ScrollBar + wheel events; no zoom (scale is 1 in this
// widget). Clicking a row selects it and fires onSelectionChanged.
//
// Lifetime: the layout/source must be kept alive by the shell or released
// via clearDocument(). Completion callbacks are guarded by a shared alive
// flag like PdfViewport's.
class PageThumbnailList : public Widget {
public:
    static constexpr double kRowPadding = 8.0;       // padding around each row
    static constexpr double kThumbnailWidth = 160.0; // points
    static constexpr double kLabelHeight = 18.0;     // page label strip under the image
    static constexpr std::size_t kVisibleMarginRows = 2; // rows requested beyond the viewport

    PageThumbnailList();
    ~PageThumbnailList() override;

    // Binds a document. layout/source may be null (empty state).
    void setDocument(core::DocumentId documentId, const render::PageLayout* layout,
                     render::IRenderSource* source,
                     std::function<std::uint64_t()> revisionProvider);
    void clearDocument();

    // The selected (current) page row; highlighted and rendered at higher
    // priority. Out-of-range resets to nullopt.
    void setSelectedIndex(std::optional<std::size_t> index);
    std::optional<std::size_t> selectedIndex() const { return selectedIndex_; }

    // Fired after the user clicks a row (not for programmatic selection).
    void setOnSelectionChanged(std::function<void(std::size_t)> onSelectionChanged);

    // Page labels from the PDF page-label tree ("i", "A-1", ...); rows with
    // an empty label fall back to "Page N". Call after setDocument.
    void setPageLabels(std::vector<std::string> labels);

    // Scrolls the given page row fully into view. Called by the shell when
    // the tracked current page changes so the list follows the document.
    void revealPage(std::size_t index);

    bool onMouse(const PointerEvent& event) override;
    void layout() override;
    void paintSelf(PaintContext& context) const override;

private:
    // Row geometry. Row height = thumbnail height (aspect of the page) +
    // label strip + padding; tops come from the prefix-sum table.
    double contentHeight() const;
    core::Rect thumbnailRect(std::size_t index) const;
    core::Rect rowRect(std::size_t index) const;
    std::size_t rowCount() const;
    std::pair<std::size_t, std::size_t> visibleRowRange() const;
    std::optional<std::size_t> rowIndexAt(const core::Point& localPoint) const;

    void setScrollOffset(double offset);
    void syncScrollbar();
    void positionScrollbar();
    void requestThumbnail(std::size_t index, double backingScale,
                          render::RenderPriority priority) const;
    render::PhysicalRenderScaleKey thumbnailScaleKey(std::size_t index, double backingScale) const;

    core::DocumentId documentId_;
    const render::PageLayout* layout_ = nullptr;
    render::IRenderSource* source_ = nullptr;
    std::function<std::uint64_t()> revisionProvider_;

    std::optional<std::size_t> selectedIndex_;
    std::function<void(std::size_t)> onSelectionChanged_;
    std::vector<std::string> pageLabels_;

    // Prefix sums of row heights (prefixHeights_[i] = top of row i;
    // prefixHeights_.back() + last height = total content height).
    std::vector<double> prefixHeights_;
    double scrollOffset_ = 0.0;

    ScrollBar* scrollBar_ = nullptr;

    // Alive flag shared with thumbnail completion callbacks (same pattern as
    // PdfViewport).
    std::shared_ptr<std::atomic<bool>> aliveFlag_;
};

} // namespace rivet::ui
