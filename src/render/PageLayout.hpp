#pragma once

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"

#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace rivet::render {

// Stacks pages vertically in content space (points, origin top-left, y-down).
//
// Content space: the margin surrounds the page column on all sides, each page
// frame is horizontally centered within the widest page, and consecutive
// frames are separated by the gap. All geometry is precomputed when the page
// list, gap or margin changes, so queries are O(1) or a small linear scan.
class PageLayout {
public:
    struct PageInfo {
        core::PageId id;
        core::Size sizePoints; // display size (post-rotation)
        core::PageRotation rotation = core::PageRotation::None;
    };

    PageLayout() = default;

    void setPages(std::vector<PageInfo> pages); // recomputes the layout
    const std::vector<PageInfo>& pages() const { return pages_; }

    void setPageGapPoints(double gap); // default 16; negative clamps to 0
    double pageGapPoints() const { return gap_; }

    void setPageMarginPoints(double margin); // default 24 on all sides; negative clamps to 0
    double pageMarginPoints() const { return margin_; }

    std::size_t pageCount() const { return pages_.size(); }

    // width = max page width + 2*margin; height = sum of page heights +
    // gaps + 2*margin. Zero for an empty layout.
    core::Size contentSizePoints() const { return contentSize_; }

    // Frame of page `index` in content space. Asserts index < pageCount.
    core::Rect pageFramePoints(std::size_t index) const;

    // Index of the page whose frame contains the point, if any.
    std::optional<std::size_t> pageIndexAt(const core::Point& contentPoint) const;

    // Inclusive index range of pages whose frames intersect the rect (edges
    // touching exactly do not count). nullopt when nothing intersects.
    std::optional<std::pair<std::size_t, std::size_t>>
    visiblePageRange(const core::Rect& contentRectPoints) const;

    // Content-space y of the top edge of page `index` (for scroll
    // positioning). Asserts index < pageCount.
    double pageTopOffsetPoints(std::size_t index) const;

    // Index of the "current" page for a viewport showing contentRect:
    // the page containing the rect's center point; when the center falls in
    // a gap or margin, the page with the largest visible area (ties resolve
    // to the LOWER index, so the result cannot oscillate across a boundary).
    // When the rect intersects no page at all (viewport parked in the margin
    // beyond the page column), the nearest end page wins: first page when
    // the rect is above the column, last page when below. nullopt only for
    // an empty layout. Pure function of the rect: deterministic, testable.
    std::optional<std::size_t> currentPageIndex(const core::Rect& contentRectPoints) const;

private:
    void rebuild();

    std::vector<PageInfo> pages_;
    std::vector<core::Rect> frames_;
    core::Size contentSize_;
    double gap_ = 16.0;
    double margin_ = 24.0;
};

} // namespace rivet::render
