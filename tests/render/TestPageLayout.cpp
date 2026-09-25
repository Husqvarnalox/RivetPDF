#include "RivetTest.h"

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/PageLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

using rivet::core::PageId;
using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::render::PageLayout;

namespace {

PageLayout::PageInfo page(std::uint64_t id, double width, double height) {
    PageLayout::PageInfo info;
    info.id = PageId{id};
    info.sizePoints = Size{width, height};
    return info;
}

} // namespace

RIVET_TEST(pageLayoutSinglePage) {
    PageLayout layout;
    layout.setPages({page(1, 612, 792)});
    CHECK_EQ(layout.pageCount(), std::size_t{1});
    const Size singleContent{660, 840};
    CHECK_EQ(layout.contentSizePoints(), singleContent);
    CHECK(Rect::nearlyEqual(layout.pageFramePoints(0), Rect{24.0, 24.0, 612.0, 792.0}, 1e-9));
    CHECK_NEAR(layout.pageTopOffsetPoints(0), 24.0, 1e-9);

    const auto range = layout.visiblePageRange(Rect{0.0, 0.0, 660.0, 840.0});
    CHECK(range.has_value());
    CHECK_EQ(range->first, std::size_t{0});
    CHECK_EQ(range->second, std::size_t{0});
    CHECK_EQ(layout.pageIndexAt(Point{300, 400}).value_or(99), std::size_t{0});
}

RIVET_TEST(pageLayoutStacksAndCenters) {
    PageLayout layout;
    layout.setPages({page(1, 612, 792), page(2, 400, 300)});
    CHECK_EQ(layout.pageCount(), std::size_t{2});
    // width = 612 + 48; height = 24 + 792 + 16 + 300 + 24.
    const Size stackedContent{660, 1156};
    CHECK_EQ(layout.contentSizePoints(), stackedContent);
    // Page 2 is narrower than page 1, so it is centered: x = 24 + (612-400)/2.
    CHECK(Rect::nearlyEqual(layout.pageFramePoints(0), Rect{24.0, 24.0, 612.0, 792.0}, 1e-9));
    CHECK(Rect::nearlyEqual(layout.pageFramePoints(1), Rect{130.0, 832.0, 400.0, 300.0}, 1e-9));
    CHECK_NEAR(layout.pageTopOffsetPoints(1), 832.0, 1e-9);
}

RIVET_TEST(pageLayoutGapAndMargin) {
    PageLayout layout;
    layout.setPages({page(1, 612, 792), page(2, 400, 300)});

    layout.setPageGapPoints(0);
    CHECK_NEAR(layout.pageGapPoints(), 0.0, 1e-12);
    const Size gaplessContent{660, 1140};
    CHECK_EQ(layout.contentSizePoints(), gaplessContent);
    CHECK(Rect::nearlyEqual(layout.pageFramePoints(1), Rect{130.0, 816.0, 400.0, 300.0}, 1e-9));

    layout.setPageMarginPoints(0);
    CHECK_NEAR(layout.pageMarginPoints(), 0.0, 1e-12);
    const Size marginlessContent{612, 1092};
    CHECK_EQ(layout.contentSizePoints(), marginlessContent);
    CHECK(Rect::nearlyEqual(layout.pageFramePoints(0), Rect{0.0, 0.0, 612.0, 792.0}, 1e-9));
    CHECK(Rect::nearlyEqual(layout.pageFramePoints(1), Rect{106.0, 792.0, 400.0, 300.0}, 1e-9));

    // Negative values clamp to zero.
    layout.setPageGapPoints(-5);
    CHECK_NEAR(layout.pageGapPoints(), 0.0, 1e-12);
    layout.setPageMarginPoints(-5);
    CHECK_NEAR(layout.pageMarginPoints(), 0.0, 1e-12);
}

RIVET_TEST(pageLayoutVisibleRange) {
    PageLayout layout;
    layout.setPages({page(1, 612, 792), page(2, 400, 300)});

    const auto both = layout.visiblePageRange(Rect{0.0, 0.0, 660.0, 1156.0});
    CHECK(both.has_value());
    CHECK_EQ(both->first, std::size_t{0});
    CHECK_EQ(both->second, std::size_t{1});

    const auto firstOnly = layout.visiblePageRange(Rect{0.0, 0.0, 660.0, 100.0});
    CHECK(firstOnly.has_value());
    CHECK_EQ(firstOnly->first, std::size_t{0});
    CHECK_EQ(firstOnly->second, std::size_t{0});

    const auto secondOnly = layout.visiblePageRange(Rect{0.0, 900.0, 660.0, 50.0});
    CHECK(secondOnly.has_value());
    CHECK_EQ(secondOnly->first, std::size_t{1});
    CHECK_EQ(secondOnly->second, std::size_t{1});

    const auto spanning = layout.visiblePageRange(Rect{0.0, 800.0, 660.0, 100.0});
    CHECK(spanning.has_value());
    CHECK_EQ(spanning->first, std::size_t{0});
    CHECK_EQ(spanning->second, std::size_t{1});

    // Edges touching exactly do not count: page 1 ends at y=816, page 2
    // starts at y=832.
    CHECK(!layout.visiblePageRange(Rect{0.0, 816.0, 660.0, 16.0}).has_value());
    CHECK(!layout.visiblePageRange(Rect{0.0, 1200.0, 660.0, 50.0}).has_value());
    CHECK(!layout.visiblePageRange(Rect{0.0, 820.0, 5.0, 5.0}).has_value()); // inside the gap
}

RIVET_TEST(pageLayoutIndexAt) {
    PageLayout layout;
    layout.setPages({page(1, 612, 792), page(2, 400, 300)});

    CHECK_EQ(layout.pageIndexAt(Point{24, 24}).value_or(99), std::size_t{0}); // page 1 top-left corner
    CHECK_EQ(layout.pageIndexAt(Point{300, 400}).value_or(99), std::size_t{0});
    CHECK_EQ(layout.pageIndexAt(Point{130, 900}).value_or(99), std::size_t{1});
    CHECK(!layout.pageIndexAt(Point{300, 824}).has_value());  // in the gap
    CHECK(!layout.pageIndexAt(Point{10, 400}).has_value());   // left of page 1
    CHECK(!layout.pageIndexAt(Point{300, 2000}).has_value()); // below the content
}

RIVET_TEST(pageLayoutOffsetsCumulative) {
    PageLayout layout;
    layout.setPages({page(1, 612, 792), page(2, 612, 792), page(3, 612, 792)});
    CHECK_NEAR(layout.pageTopOffsetPoints(0), 24.0, 1e-9);
    CHECK_NEAR(layout.pageTopOffsetPoints(1), 24.0 + 792.0 + 16.0, 1e-9);
    CHECK_NEAR(layout.pageTopOffsetPoints(2), 24.0 + 2.0 * (792.0 + 16.0), 1e-9);
    const Size threePageContent{660.0, 3.0 * 792.0 + 2.0 * 16.0 + 48.0};
    CHECK_EQ(layout.contentSizePoints(), threePageContent);
}

RIVET_TEST(pageLayoutEmpty) {
    PageLayout layout;
    CHECK_EQ(layout.pageCount(), std::size_t{0});
    const Size zeroContent{0, 0};
    CHECK_EQ(layout.contentSizePoints(), zeroContent);
    CHECK(layout.pages().empty());
    CHECK(!layout.pageIndexAt(Point{0, 0}).has_value());
    CHECK(!layout.visiblePageRange(Rect{0.0, 0.0, 100.0, 100.0}).has_value());

    // Clearing an occupied layout returns to the empty state.
    layout.setPages({page(1, 100, 100)});
    CHECK_EQ(layout.pageCount(), std::size_t{1});
    layout.setPages({});
    CHECK_EQ(layout.pageCount(), std::size_t{0});
    CHECK_EQ(layout.contentSizePoints(), zeroContent);
}

RIVET_TEST(currentPageIndexFollowsTheViewportCenter) {
    // Pages {200x300}, {300x200}, {100x100}, margin 24, gap 16:
    //   page 0 = {24, 24, 200, 300}   (y 24..324)
    //   page 1 = {59, 340, 300, 200}  (y 340..540)
    //   page 2 = {137, 556, 100, 100} (y 556..656)
    PageLayout layout;
    layout.setPages({page(1, 200, 300), page(2, 300, 200), page(3, 100, 100)});
    layout.setPageGapPoints(16.0);
    layout.setPageMarginPoints(24.0);

    // Center inside page 0.
    CHECK_EQ(layout.currentPageIndex(Rect{0.0, 0.0, 348.0, 300.0}), std::optional<std::size_t>(0));

    // Center in the gap (y 333 in [324, 340]): largest visible area wins,
    // which is page 1 (33 > 31 points of overlap).
    CHECK_EQ(layout.currentPageIndex(Rect{0.0, 293.0, 348.0, 80.0}), std::optional<std::size_t>(1));

    // Center inside page 1.
    CHECK_EQ(layout.currentPageIndex(Rect{0.0, 290.0, 348.0, 300.0}), std::optional<std::size_t>(1));

    // Center inside page 2.
    CHECK_EQ(layout.currentPageIndex(Rect{0.0, 500.0, 348.0, 200.0}), std::optional<std::size_t>(2));
}

RIVET_TEST(currentPageIndexTieBreaksToTheLowerPage) {
    PageLayout layout;
    layout.setPages({page(1, 200, 300), page(2, 200, 300)});
    layout.setPageGapPoints(16.0);
    layout.setPageMarginPoints(24.0);
    // page 0 = {24, 24, 200, 300}; page 1 = {24, 340, 200, 300}. The rect
    // {24, 320, 200, 24} overlaps both pages by exactly 4 points of height;
    // the tie must resolve to the lower index (stable at the boundary).
    CHECK_EQ(layout.currentPageIndex(Rect{24.0, 320.0, 200.0, 24.0}), std::optional<std::size_t>(0));
}

RIVET_TEST(currentPageIndexOutsideTheColumnClampsToAnEndPage) {
    PageLayout layout;
    layout.setPages({page(1, 200, 300), page(2, 200, 300)});
    layout.setPageGapPoints(16.0);
    layout.setPageMarginPoints(24.0);

    // Rect entirely above the page column (page 0 starts at y 24).
    CHECK_EQ(layout.currentPageIndex(Rect{0.0, 0.0, 248.0, 10.0}), std::optional<std::size_t>(0));
    // Rect entirely below the page column (content height = 664).
    CHECK_EQ(layout.currentPageIndex(Rect{0.0, 670.0, 248.0, 40.0}), std::optional<std::size_t>(1));

    // Empty layout has no current page.
    PageLayout empty;
    CHECK_EQ(empty.currentPageIndex(Rect{0.0, 0.0, 100.0, 100.0}), std::nullopt);
}
