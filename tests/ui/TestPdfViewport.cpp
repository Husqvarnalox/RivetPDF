#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/PageLayout.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderScaleKey.hpp"
#include "render/TileKey.hpp"
#include "render/ViewerState.hpp"
#include "render/ZoomState.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/ViewerTextBridge.hpp"
#include "ui/ViewportTool.hpp"
#include "ui/UiTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using rivet::core::DocumentId;
using rivet::core::PageId;
using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::render::PageLayout;
using rivet::render::PhysicalRenderScaleKey;
using rivet::render::RenderPriority;
using rivet::render::RenderScaleKey;
using rivet::render::TileKey;
using rivet::render::ZoomState;
using rivet::render::testing::FakeRenderSource;
using rivet::ui::KeyEvent;
using rivet::ui::Key;
using rivet::ui::PdfViewport;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::testing::CountingRedrawSink;
using rivet::ui::testing::FakePaintContext;

namespace {

constexpr std::uint64_t kRevision = 7;

// Two pages {612x792} and {400x400} with gap 16 and margin 24:
//   content  = {612 + 48, 792 + 400 + 16 + 48} = {660, 1256}
//   page 0   = {24, 24, 612, 792}
//   page 1   = {130, 832, 400, 400}
struct Fixture {
    PageLayout layout;
    FakeRenderSource source;
    rivet::render::ViewerState state;
    PdfViewport viewport;

    Fixture() {
        std::vector<PageLayout::PageInfo> pages;
        pages.push_back(PageLayout::PageInfo{PageId{11}, Size{612.0, 792.0}});
        pages.push_back(PageLayout::PageInfo{PageId{22}, Size{400.0, 400.0}});
        layout.setPages(pages);
        layout.setPageGapPoints(16.0);
        layout.setPageMarginPoints(24.0);

        viewport.setFrame(Rect{0.0, 0.0, 800.0, 600.0});
        viewport.setDocument(DocumentId{1}, &layout, &source, [] { return kRevision; }, &state);
    }
};

PointerEvent scrollEvent(Point delta, bool command, bool control) {
    PointerEvent event;
    event.type = PointerEventType::Scroll;
    event.scrollDelta = delta;
    event.modifiers.command = command;
    event.modifiers.control = control;
    return event;
}

// The physical cache identity the viewport must derive for `zoom` on a display
// with `backingScale` (quantized zoom x backing, one key for both identity and
// raster density).
PhysicalRenderScaleKey physicalKeyFor(double zoom, double backingScale) {
    return PhysicalRenderScaleKey::fromDensities(RenderScaleKey::fromZoom(zoom).scale(), backingScale);
}

// Exact requested tile-coordinate set for one page (order-independent).
std::set<std::pair<std::uint32_t, std::uint32_t>> requestedTiles(const FakeRenderSource& source,
                                                                 PageId pageId) {
    std::set<std::pair<std::uint32_t, std::uint32_t>> tiles;
    for (const auto& record : source.requests) {
        if (record.request.key.pageId == pageId) {
            tiles.emplace(record.request.key.tileX, record.request.key.tileY);
        }
    }
    return tiles;
}

bool sameColor(const rivet::ui::Color& a, const rivet::ui::Color& b) {
    return std::fabs(a.r - b.r) <= 1e-9 && std::fabs(a.g - b.g) <= 1e-9 &&
           std::fabs(a.b - b.b) <= 1e-9 && std::fabs(a.a - b.a) <= 1e-9;
}

bool hasText(const FakePaintContext& context, const char* text) {
    for (const auto& record : context.texts) {
        if (record.text == text) return true;
    }
    return false;
}

} // namespace

RIVET_TEST(emptyStatePaintsMessage) {
    PdfViewport viewport;
    viewport.setFrame(Rect{0.0, 0.0, 800.0, 600.0});

    FakePaintContext context;
    viewport.paint(context);

    CHECK(context.bitmaps.empty());
    CHECK_EQ(hasText(context, "Open a PDF to begin"), true);
}

RIVET_TEST(cachedTileIsDrawnAtPageOrigin) {
    Fixture f;
    const TileKey originTile{DocumentId{1}, PageId{11}, physicalKeyFor(1.0, 1.0), 0, 0};
    f.source.insertTile(originTile, 512, 512);

    FakePaintContext context;
    f.viewport.paint(context);

    // Page 0's frame is {24, 24, 612, 792} in content space; at zoom 1 with
    // zero scroll the cached tile (0, 0) covers {24, 24, 512, 512} logical px.
    CHECK_EQ(context.bitmaps.size(), std::size_t{1});
    CHECK(Rect::nearlyEqual(context.bitmaps[0].dest, Rect{24.0, 24.0, 512.0, 512.0}, 1e-9));
    CHECK_EQ(f.source.lastCacheRevision, kRevision);
}

RIVET_TEST(missingTilesAreRequestedAtVisiblePriority) {
    Fixture f;
    FakePaintContext context;
    f.viewport.paint(context);

    // Page 1's top edge is at y=832, below the 600 px viewport: only page 0
    // paints. Its visible slice {0, 0, 612, 600} touches exactly the 2x2 tile
    // block at the page origin, all missing.
    const auto tiles = requestedTiles(f.source, PageId{11});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expected{
        {0, 0}, {1, 0}, {0, 1}, {1, 1}};
    CHECK(tiles == expected);
    // Visible work is always Visible priority.
    for (const auto& record : f.source.requests) {
        if (record.request.key.pageId == PageId{11}) {
            CHECK(record.priority == RenderPriority::Visible);
        }
    }

    // The next page's top band is prefetched at Impending priority.
    bool sawPrefetch = false;
    for (const auto& record : f.source.requests) {
        if (record.request.key.pageId != PageId{22}) continue;
        CHECK(record.priority == RenderPriority::Impending);
        sawPrefetch = true;
    }
    CHECK(sawPrefetch);

    for (const auto& record : f.source.requests) {
        if (record.request.key.pageId != PageId{11}) continue;
        CHECK(record.priority == RenderPriority::Visible);
        CHECK_EQ(record.request.key.documentId, DocumentId{1});
        CHECK_EQ(record.request.key.scale, physicalKeyFor(1.0, 1.0));
        // Raster density derives from the key, exactly.
        CHECK_EQ(record.request.params.devicePixelsPerPoint, record.request.key.scale.scale());
        CHECK_EQ(record.request.params.devicePixelsPerPoint, 1.0);
    }

    // Tile (1, 0) is clipped to the page's 612pt width: 100 points wide.
    bool sawRightTile = false;
    for (const auto& record : f.source.requests) {
        if (record.request.key.pageId != PageId{11}) continue;
        if (record.request.key.tileX == 1 && record.request.key.tileY == 0) {
            sawRightTile = true;
            CHECK(Rect::nearlyEqual(record.request.params.pageRectPoints,
                                    Rect{512.0, 0.0, 100.0, 512.0}, 1e-9));
        }
    }
    CHECK(sawRightTile);

    // A gray placeholder is painted per missing tile.
    int placeholders = 0;
    for (const auto& fill : context.fills) {
        if (sameColor(fill.color, rivet::ui::Color::gray(0.92))) ++placeholders;
    }
    CHECK_EQ(placeholders, 4);
}

RIVET_TEST(partiallyVisiblePageRequestsOnlyCornerTiles) {
    Fixture f;
    // A 100x100 window over the content whose visible slice of page 0 is its
    // bottom-right corner: content rect {560, 700, 100, 100} -> page-local
    // {536, 676, 76, 100}, which lies entirely inside tile (1, 1).
    f.viewport.setFrame(Rect{0.0, 0.0, 100.0, 100.0});
    f.viewport.setScrollOffsetPoints(Point{560.0, 700.0});

    FakePaintContext context;
    f.viewport.paint(context);

    const auto tiles = requestedTiles(f.source, PageId{11});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expected{{1, 1}};
    CHECK(tiles == expected);
    // The viewport center {610, 750} is inside page 0.
    CHECK(f.viewport.currentPageIndex() == std::size_t{0});

    // Tile (1, 1) clipped to the 612x792 page: {512, 512, 100, 280} points.
    for (const auto& record : f.source.requests) {
        if (record.request.key.pageId == PageId{11} && record.request.key.tileX == 1 &&
            record.request.key.tileY == 1) {
            CHECK(Rect::nearlyEqual(record.request.params.pageRectPoints,
                                    Rect{512.0, 512.0, 100.0, 280.0}, 1e-9));
        }
    }
}

RIVET_TEST(viewportInsidePageRequestsOnlyCoveringTiles) {
    Fixture f;
    // Zoom 2: tile extent is 256 points. A 600x600 viewport (300x300 points)
    // whose top-left sits at page-local {256, 256} spans tile columns/rows
    // 1..2 only - out of a 3x4 grid.
    CHECK(f.viewport.zoom().setZoom(2.0));
    f.viewport.setFrame(Rect{0.0, 0.0, 600.0, 600.0});
    f.viewport.setScrollOffsetPoints(Point{280.0, 280.0});

    FakePaintContext context;
    f.viewport.paint(context);

    const auto tiles = requestedTiles(f.source, PageId{11});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expected{
        {1, 1}, {2, 1}, {1, 2}, {2, 2}};
    CHECK(tiles == expected);
}

RIVET_TEST(tinyViewportAtHighZoomRequestsExactlyTheVisibleTiles) {
    Fixture f;
    // Zoom 64: tile extent is 8 points; page 0 spans a 77x99 grid. A 100x100
    // viewport covers 1.5625 points of the page - direct index math must
    // produce exactly one request where a whole-grid sweep would visit 7623
    // cells (and two under a negative-floor cast bug).
    CHECK(f.viewport.zoom().setZoom(64.0));
    f.viewport.setFrame(Rect{0.0, 0.0, 100.0, 100.0});
    f.viewport.setScrollOffsetPoints(Point{24.0, 24.0});

    FakePaintContext context;
    f.viewport.paint(context);

    const auto tiles = requestedTiles(f.source, PageId{11});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expected{{0, 0}};
    CHECK(tiles == expected);

    // Same window parked in the page's far bottom-right corner: still exactly
    // one request, now for grid cell (75, 97) - no wraparound, no underflow.
    f.source.requests.clear();
    f.viewport.setScrollOffsetPoints(Point{628.0, 804.0});
    FakePaintContext context2;
    f.viewport.paint(context2);

    const auto tilesBottomRight = requestedTiles(f.source, PageId{11});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expectedBottomRight{{75, 97}};
    CHECK(tilesBottomRight == expectedBottomRight);
}

RIVET_TEST(largePageSmallViewportRequestsOnlyAHandfulOfTiles) {
    PageLayout layout;
    std::vector<PageLayout::PageInfo> pages;
    pages.push_back(PageLayout::PageInfo{PageId{33}, Size{2048.0, 2048.0}});
    layout.setPages(pages);

    FakeRenderSource source;
    rivet::render::ViewerState state;
    PdfViewport viewport;
    viewport.setFrame(Rect{0.0, 0.0, 700.0, 700.0});
    viewport.setDocument(DocumentId{1}, &layout, &source, [] { return kRevision; }, &state);
    viewport.setScrollOffsetPoints(Point{124.0, 124.0}); // page-local {100, 100}

    FakePaintContext context;
    viewport.paint(context);

    // The 2048x2048 page is a 4x4 grid at zoom 1 (16 tiles); the visible
    // 700x700 slice starting at {100, 100} covers tiles (0..1, 0..1) only.
    const auto tiles = requestedTiles(source, PageId{33});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expected{
        {0, 0}, {1, 0}, {0, 1}, {1, 1}};
    CHECK(tiles == expected);
}

RIVET_TEST(tileBoundaryAtVisibleEdgeDoesNotSpawnTheNextTile) {
    Fixture f;
    // Viewport height 536: page-local visible extent is exactly one tile of
    // 512 points. The boundary sits exactly on the visible edge, so row 1
    // must NOT be requested.
    f.viewport.setFrame(Rect{0.0, 0.0, 800.0, 536.0});
    f.viewport.setScrollOffsetPoints(Point{0.0, 0.0});

    FakePaintContext context;
    f.viewport.paint(context);

    const auto tiles = requestedTiles(f.source, PageId{11});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expected{{0, 0}, {1, 0}};
    CHECK(tiles == expected);

    // One more point of height makes row 1 partially visible: it is requested.
    f.source.requests.clear();
    f.viewport.setFrame(Rect{0.0, 0.0, 800.0, 537.0});
    FakePaintContext context2;
    f.viewport.paint(context2);

    const auto tilesWithOneMorePoint = requestedTiles(f.source, PageId{11});
    const std::set<std::pair<std::uint32_t, std::uint32_t>> expectedWithRow{
        {0, 0}, {1, 0}, {0, 1}, {1, 1}};
    CHECK(tilesWithOneMorePoint == expectedWithRow);
}

RIVET_TEST(scrollIsClampedToContentBounds) {
    Fixture f;

    f.viewport.setScrollOffsetPoints(Point{-50.0, -50.0});
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 0.0}, 1e-9));

    // max offset = max(0, content - viewport/zoom) = {max(0, 660-800), 1256-600}.
    f.viewport.setScrollOffsetPoints(Point{10000.0, 10000.0});
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 656.0}, 1e-9));

    // In-range values pass through; scrolling accumulates within bounds.
    f.viewport.setScrollOffsetPoints(Point{0.0, 200.0});
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 200.0}, 1e-9));
    f.viewport.scrollByContentPoints(Point{0.0, 600.0});
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 656.0}, 1e-9));
}

RIVET_TEST(wheelScrollsWithoutModifiers) {
    Fixture f;

    PointerEvent scroll = scrollEvent(Point{0.0, 120.0}, false, false);
    CHECK_EQ(f.viewport.onMouse(scroll), true);
    CHECK(scroll.accepted);
    // Content delta = scrollDelta / zoom = 120 / 1.0.
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 120.0}, 1e-9));
}

RIVET_TEST(commandWheelZoomsAnchoredAtPointer) {
    Fixture f;

    int zoomCallbacks = 0;
    double reportedZoom = 0.0;
    f.viewport.setZoomChangedCallback([&zoomCallbacks, &reportedZoom](double value) {
        ++zoomCallbacks;
        reportedZoom = value;
    });

    // deltaY < 0 with command: zoom in one step.
    PointerEvent pinch = scrollEvent(Point{0.0, -1.0}, true, false);
    pinch.position = Point{400.0, 300.0}; // viewport center, as on a real trackpad
    CHECK_EQ(f.viewport.onMouse(pinch), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);
    CHECK_EQ(zoomCallbacks, 1);
    CHECK_NEAR(reportedZoom, 1.25, 1e-12);

    // Anchor: the content point under the pointer {400, 300} stays put
    // -> {400, 300} - {400, 300}/1.25 = {80, 60}; x clamps to 660 - 640 = 20.
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{20.0, 60.0}, 1e-9));

    // deltaY >= 0 with command: zoom back out, re-anchored.
    PointerEvent pinchOut = scrollEvent(Point{0.0, 1.0}, true, false);
    pinchOut.position = Point{400.0, 300.0};
    CHECK_EQ(f.viewport.onMouse(pinchOut), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.0, 1e-12);
    CHECK_EQ(zoomCallbacks, 2);

    // Control behaves like command.
    PointerEvent ctrlPinch = scrollEvent(Point{0.0, -1.0}, false, true);
    ctrlPinch.position = Point{400.0, 300.0};
    CHECK_EQ(f.viewport.onMouse(ctrlPinch), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);
    CHECK_EQ(zoomCallbacks, 3);
}

RIVET_TEST(zoomChangesQuantizeTileRequests) {
    Fixture f;
    CHECK(f.viewport.zoom().setZoom(2.0));

    FakePaintContext context;
    f.viewport.paint(context);

    // Visible content rect at zoom 2: {0, 0, 400, 300} -> page 0 only. Tile
    // extent is 512/2 = 256 points; the 376x276 visible slice of the page
    // touches a 2x2 tile block.
    const PhysicalRenderScaleKey quantized = physicalKeyFor(2.0, 1.0);
    CHECK_EQ(quantized.value, std::uint32_t{128}); // ceil(2.0 * 64) / 64 == 2.0
    for (const auto& record : f.source.requests) {
        if (record.request.key.pageId != PageId{11}) continue;
        CHECK_EQ(record.request.key.scale, quantized);
        // Exact: both sides are the same quantized value over the denominator.
        CHECK_EQ(record.request.params.devicePixelsPerPoint, record.request.key.scale.scale());
        CHECK_EQ(record.request.params.devicePixelsPerPoint, 2.0);
        CHECK(record.request.params.pageRectPoints.size.width <= 256.0 + 1e-9);
    }

    bool sawOriginTile = false;
    for (const auto& record : f.source.requests) {
        if (record.request.key.tileX == 0 && record.request.key.tileY == 0) {
            sawOriginTile = true;
            CHECK(Rect::nearlyEqual(record.request.params.pageRectPoints,
                                    Rect{0.0, 0.0, 256.0, 256.0}, 1e-9));
        }
    }
    CHECK(sawOriginTile);
}

RIVET_TEST(backingScaleDistinguishesCacheIdentity) {
    Fixture f;

    FakePaintContext context;
    context.backingScaleValue = 2.0;
    f.viewport.paint(context);

    // Same zoom, 2x display: the requested identity and the raster density
    // double, so entries cannot be shared with a 1x display. The 612x792 page
    // is a 3x4 grid at density 2, and the 800x600 viewport (covering
    // {0, 0, 612, 600} page points = {0, 0, 1224, 1200} device px) sees the
    // 3x3 block at the grid origin.
    const PhysicalRenderScaleKey retina = physicalKeyFor(1.0, 2.0);
    CHECK_EQ(retina.value, std::uint32_t{128});
    std::size_t visibleRequests = 0;
    for (const auto& record : f.source.requests) {
        if (record.request.key.pageId != PageId{11}) continue;
        ++visibleRequests;
        CHECK_EQ(record.request.key.scale, retina);
        CHECK_EQ(record.request.params.devicePixelsPerPoint, 2.0);
        // Tile extent in points halves on the 2x display.
        CHECK(record.request.params.pageRectPoints.size.width <= 256.0 + 1e-9);
    }
    CHECK_EQ(visibleRequests, std::size_t{9});

    // The 1x identity is a different cache key.
    CHECK(!(retina == physicalKeyFor(1.0, 1.0)));
}

RIVET_TEST(zoomChangeCancelsQueuedRenders) {
    Fixture f;
    FakePaintContext context;
    f.viewport.paint(context);
    CHECK_GE(f.source.requests.size(), std::size_t{4});

    // Any zoom change drops queued-not-started renders requested at the old
    // scale; the next paint re-requests current-scale tiles.
    CHECK(f.viewport.zoom().setZoom(1.25));
    CHECK_EQ(f.source.cancelAllCount, 1);

    // No zoom change, no cancel.
    CHECK(!f.viewport.zoom().setZoom(1.25));
    CHECK_EQ(f.source.cancelAllCount, 1);

    // With no document attached the hook must not touch a null source.
    f.viewport.clearDocument();
    CHECK_EQ(f.source.cancelAllCount, 2); // clearDocument itself cancels
    CHECK(f.viewport.zoom().setZoom(1.5));
    CHECK_EQ(f.source.cancelAllCount, 2);
}

RIVET_TEST(keyInput) {
    Fixture f;

    // Plus zooms in one step.
    KeyEvent plus;
    plus.key = Key::Plus;
    CHECK_EQ(f.viewport.onKey(plus), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);

    // '=' (Character) also zooms in.
    KeyEvent equals;
    equals.key = Key::Character;
    equals.text = "=";
    CHECK_EQ(f.viewport.onKey(equals), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.5, 1e-12);

    // PageDown scrolls one viewport height in content points: 600 / 1.5 = 400.
    KeyEvent pageDown;
    pageDown.key = Key::PageDown;
    CHECK_EQ(f.viewport.onKey(pageDown), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 400.0, 1e-9);

    // Minus zooms out.
    KeyEvent minus;
    minus.key = Key::Minus;
    CHECK_EQ(f.viewport.onKey(minus), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);

    // Another PageDown: 400 + 600/1.25 = 880. At zoom 1.25 the visible
    // extent is 600/1.25 = 480 points, so the clamp is 1256 - 480 = 776.
    CHECK_EQ(f.viewport.onKey(pageDown), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 776.0, 1e-9);

    KeyEvent home;
    home.key = Key::Home;
    CHECK_EQ(f.viewport.onKey(home), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 0.0, 1e-9);

    KeyEvent end;
    end.key = Key::End;
    CHECK_EQ(f.viewport.onKey(end), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 776.0, 1e-9);

    // PageUp: 776 - 600/1.25 = 296.
    KeyEvent pageUp;
    pageUp.key = Key::PageUp;
    CHECK_EQ(f.viewport.onKey(pageUp), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 296.0, 1e-9);

    // Unhandled keys are not consumed.
    KeyEvent escape;
    escape.key = Key::Escape;
    CHECK_EQ(f.viewport.onKey(escape), false);
}

RIVET_TEST(completedRenderInvalidatesTheView) {
    Fixture f;
    f.source.completeImmediately = true;

    CountingRedrawSink sink;
    f.viewport.setRedrawSink(&sink);

    FakePaintContext context;
    f.viewport.paint(context);

    // Every missing tile synchronously completed its callback -> one
    // invalidation per request.
    CHECK(!f.source.requests.empty());
    CHECK_EQ(sink.count, static_cast<int>(f.source.requests.size()));
}

RIVET_TEST(resizeReclampsScrollAndKeepsFitWidthSticky) {
    Fixture f;
    f.viewport.setScrollOffsetPoints(Point{0.0, 656.0});

    // Taller viewport extends past the content: max y becomes 1256 - 700 = 556.
    f.viewport.setFrame(Rect{0.0, 0.0, 800.0, 700.0});
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 556.0, 1e-9);

    // Fit WIDTH is a MODE: the zoom recomputes on every layout pass and the
    // mode stays active until manual zoom exits it (Phase 2 requirement).
    CHECK(f.viewport.zoom().setZoom(1.25)); // manual zoom first
    f.viewport.setFitMode(ZoomState::FitMode::Width);
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::Width);
    f.viewport.setFrame(Rect{0.0, 0.0, 612.0, 700.0});
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.0, 1e-9); // 612 / 612
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::Width);

    // Still active on the next resize: narrower window -> smaller zoom
    // (306 / 612 = 0.5), recomputed live.
    f.viewport.setFrame(Rect{0.0, 0.0, 306.0, 700.0});
    CHECK_NEAR(f.viewport.zoom().zoom(), 0.5, 1e-9);
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::Width);

    // Manual zoom exits the mode.
    CHECK(f.viewport.zoomInStep());
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::None);
    CHECK_NEAR(f.viewport.zoom().zoom(), 0.67, 1e-9);
}

RIVET_TEST(fitPageFollowsTheCurrentPageAndStaysSticky) {
    Fixture f;
    // Viewport 800x600; the tracked page is 0 (612x792) -> fit page zoom =
    // min(800/612, 600/792) = 600/792 = 0.757575...
    f.viewport.setFitMode(ZoomState::FitMode::Page);
    CHECK_NEAR(f.viewport.zoom().zoom(), 600.0 / 792.0, 1e-9);
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::Page);

    // Scroll to page 1 (top at y=832): the tracked page changes and the next
    // layout pass refits page 1 (400x400) -> min(800/400, 600/400) = 1.5.
    f.viewport.goToPage(1);
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{1});
    f.viewport.layout();
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.5, 1e-9);
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::Page);
}

RIVET_TEST(viewerStateCarriesZoomAndScrollAcrossRebind) {
    // `other` is declared BEFORE the Fixture so it outlives the viewport that
    // binds it (the documented ViewerState lifetime contract).
    rivet::render::ViewerState other;
    Fixture f;
    f.viewport.zoom().setZoom(1.5);
    f.viewport.setScrollOffsetPoints(Point{0.0, 200.0});

    // Rebinding to the same state (as on tab switches) restores the state.
    f.viewport.setDocument(DocumentId{1}, &f.layout, &f.source, [] { return kRevision; }, &f.state);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.5, 1e-12);
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 200.0}, 1e-9));
    CHECK_EQ(f.viewport.viewState(), &f.state);

    // Rebinding to a fresh state (a different tab) starts at its values;
    // its stored offset is re-clamped against the new document's bounds.
    other.zoom().setZoom(2.0);
    other.setScrollOffsetPoints(Point{0.0, 2000.0});
    f.viewport.setDocument(DocumentId{1}, &f.layout, &f.source, [] { return kRevision; }, &other);
    CHECK_NEAR(f.viewport.zoom().zoom(), 2.0, 1e-12);
    // 2000 clamps to 1256 - 300 = 956 at zoom 2 (visible extent 600 / 2).
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 956.0}, 1e-9));
}

RIVET_TEST(currentPageTracksTheViewportCenter) {
    Fixture f;

    std::vector<std::size_t> reported;
    f.viewport.setCurrentPageChangedCallback([&reported](std::size_t page) { reported.push_back(page); });
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{0});

    // Page 1's frame is {130, 832, 400, 400}: scrolling past y = 832 + 200 -
    // 300 = 732 puts the viewport center {400, 300} inside page 1.
    f.viewport.setScrollOffsetPoints(Point{0.0, 740.0});
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{1});
    CHECK(reported.size() == 1 && reported.back() == std::size_t{1});

    // Back up: page 0 again.
    f.viewport.setScrollOffsetPoints(Point{0.0, 100.0});
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{0});
    CHECK_EQ(reported.size(), std::size_t{2});

    // Same-page scrolls must not re-fire.
    f.viewport.setScrollOffsetPoints(Point{0.0, 120.0});
    CHECK_EQ(reported.size(), std::size_t{2});
}

RIVET_TEST(goToPageScrollsToThePageTop) {
    Fixture f;
    // Page 1's top edge is at content y = 832 (24 + 792 + 16); at zoom 1 the
    // scrollable maximum is 1256 - 600 = 656, so the request clamps there and
    // page 1 is fully visible.
    f.viewport.goToPage(1);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 656.0, 1e-9);
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{1});

    // Out-of-range pages are rejected without touching the offset.
    f.viewport.goToPage(7);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 656.0, 1e-9);

    // At zoom 2 the scrollable maximum (956) covers the page top exactly.
    f.viewport.zoom().setZoom(2.0);
    f.viewport.goToPage(1);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 832.0, 1e-9);
}

RIVET_TEST(arrowKeysScrollByAFixedScreenDistance) {
    Fixture f;
    f.viewport.zoom().setZoom(2.0);

    rivet::ui::KeyEvent down;
    down.key = Key::Down;
    CHECK_EQ(f.viewport.onKey(down), true);
    // 40 logical points / zoom 2 = 20 content points.
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 20.0, 1e-9);

    rivet::ui::KeyEvent up;
    up.key = Key::Up;
    CHECK_EQ(f.viewport.onKey(up), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 0.0, 1e-9);

    rivet::ui::KeyEvent right;
    right.key = Key::Right;
    CHECK_EQ(f.viewport.onKey(right), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().x, 20.0, 1e-9);

    rivet::ui::KeyEvent left;
    left.key = Key::Left;
    CHECK_EQ(f.viewport.onKey(left), true);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().x, 0.0, 1e-9);
}

RIVET_TEST(emptyStateZoomAndNavigationAreSafe) {
    PdfViewport viewport;
    viewport.setFrame(Rect{0.0, 0.0, 800.0, 600.0});

    CHECK_EQ(viewport.zoomInStep(), false);
    CHECK_EQ(viewport.zoomOutStep(), false);
    CHECK_EQ(viewport.setManualZoom(2.0), false);
    viewport.zoomActualSize();          // must not crash
    viewport.setFitMode(ZoomState::FitMode::Width); // must not crash
    viewport.goToPage(0);               // must not crash

    rivet::ui::KeyEvent down;
    down.key = Key::Down;
    CHECK_EQ(viewport.onKey(down), false); // nothing to scroll
}

RIVET_TEST(clearDocumentCancelsPendingAndRepaintsEmptyState) {
    Fixture f;
    FakePaintContext before;
    f.viewport.paint(before);
    const std::size_t requestsBefore = f.source.requests.size();
    CHECK_GE(requestsBefore, std::size_t{4});

    f.viewport.clearDocument();
    CHECK_EQ(f.source.cancelAllCount, 1);

    FakePaintContext after;
    f.viewport.paint(after);
    CHECK(after.bitmaps.empty());
    CHECK_EQ(f.source.requests.size(), requestsBefore); // no new requests
    CHECK_EQ(hasText(after, "Open a PDF to begin"), true);
}

RIVET_TEST(presentationModeFlipsPagesAndRefits) {
    Fixture f;
    f.viewport.setPresentationMode(true);

    // Entering presentation centers the tracked page with a fit-page zoom:
    // page 0 (612x792) in 800x600 -> zoom 600/792.
    CHECK(f.viewport.presentationMode());
    CHECK_NEAR(f.viewport.zoom().zoom(), 600.0 / 792.0, 1e-9);

    // PageDown flips to page 1 (400x400 -> zoom min(800/400, 600/400) = 1.5)
    // and top-aligns it.
    rivet::ui::KeyEvent pageDown;
    pageDown.key = Key::PageDown;
    CHECK_EQ(f.viewport.onKey(pageDown), true);
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{1});
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.5, 1e-9);
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 832.0, 1e-9);

    // Space on the LAST page is consumed but does nothing.
    rivet::ui::KeyEvent space;
    space.key = Key::Space;
    CHECK_EQ(f.viewport.onKey(space), true);
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{1});
    rivet::ui::KeyEvent up;
    up.key = Key::Up;
    CHECK_EQ(f.viewport.onKey(up), true);
    CHECK_EQ(f.viewport.currentPageIndex(), std::size_t{0});

    // Exiting restores fit mode None (manual zoom state preserved).
    f.viewport.setPresentationMode(false);
    CHECK(!f.viewport.presentationMode());
}

// ---- annotation layer slot --------------------------------------------------

namespace {

// Shared, ordered event log for tools, layers and the fake text bridge.
using EventLog = std::vector<std::string>;

class LoggingTool final : public rivet::ui::ViewportTool {
public:
    explicit LoggingTool(EventLog& log) : log_(log) {}
    bool consume = false;

    bool onMouse(rivet::ui::ViewportToolHost&, const PointerEvent&) override {
        log_.push_back("tool.mouse");
        return consume;
    }
    bool onKey(rivet::ui::ViewportToolHost&, const KeyEvent&) override {
        log_.push_back("tool.key");
        return consume;
    }
    void paint(const rivet::ui::ViewportToolHost&, rivet::ui::PaintContext&) const override {
        log_.push_back("tool.paint");
    }

private:
    EventLog& log_;
};

class LoggingLayer final : public rivet::ui::ViewportLayer {
public:
    explicit LoggingLayer(EventLog& log) : log_(log) {}
    bool consumeMouse = false;
    bool consumeKey = false;

    // Paint-time observations (paint is const on the interface).
    mutable std::vector<std::pair<std::size_t, Rect>> pageRects;
    mutable std::vector<std::size_t> bitmapsAtPage;  // tiles painted when paintPage ran
    mutable std::vector<std::size_t> overlaysAtPage; // overlays painted when paintPage ran
    mutable std::size_t overlaysAtAbove = 0;

    static constexpr rivet::ui::Color kOverlayColor{0.1, 0.2, 0.3, 0.4};

    static std::size_t countOverlays(const FakePaintContext& context) {
        std::size_t count = 0;
        for (const auto& fill : context.fills) count += fill.color == kOverlayColor ? 1 : 0;
        return count;
    }

    bool onMouse(rivet::ui::ViewportToolHost&, const PointerEvent&) override {
        log_.push_back("layer.mouse");
        return consumeMouse;
    }
    void afterMouse(rivet::ui::ViewportToolHost&, const PointerEvent&) override {
        log_.push_back("layer.after");
    }
    bool onKey(rivet::ui::ViewportToolHost&, const KeyEvent&) override {
        log_.push_back("layer.key");
        return consumeKey;
    }
    void paintPage(const rivet::ui::ViewportToolHost&, std::size_t pageIndex,
                   const Rect& pageRectInViewport, rivet::ui::PaintContext& context) const override {
        const auto& fake = static_cast<const FakePaintContext&>(context);
        pageRects.emplace_back(pageIndex, pageRectInViewport);
        bitmapsAtPage.push_back(fake.bitmaps.size());
        overlaysAtPage.push_back(countOverlays(fake));
        log_.push_back("layer.page");
    }
    void paintAbove(const rivet::ui::ViewportToolHost&, rivet::ui::PaintContext& context) const override {
        log_.push_back("layer.above");
        overlaysAtAbove = countOverlays(static_cast<const FakePaintContext&>(context));
    }

private:
    EventLog& log_;
};

// Text bridge that "hits" a character everywhere and shows one overlay rect
// per page, so selection routing and overlay paint order are observable.
class LoggingBridge final : public rivet::ui::IViewerTextBridge {
public:
    explicit LoggingBridge(EventLog& log) : log_(log) {}

    void warmPage(std::size_t) override {}
    std::optional<std::uint32_t> charIndexAtPoint(std::size_t, const Point&) override { return 0u; }
    std::vector<rivet::ui::OverlayRect> overlayRects(std::size_t) override {
        return {rivet::ui::OverlayRect{Rect{10.0, 10.0, 20.0, 20.0}, LoggingLayer::kOverlayColor}};
    }
    void selectionDragBegan(std::size_t, std::uint32_t, bool) override {
        log_.push_back("bridge.began");
    }
    void selectionDragMoved(std::size_t, std::uint32_t) override {}
    void selectionDragEnded() override { log_.push_back("bridge.ended"); }
    void selectionCleared() override { log_.push_back("bridge.cleared"); }
    std::optional<rivet::ui::ViewerLinkHit> linkAtPoint(std::size_t, const Point&) override {
        return std::nullopt;
    }
    std::vector<Rect> linkRects(std::size_t) override { return {}; }
    void linkActivated(const rivet::ui::ViewerLinkHit&) override {}

private:
    EventLog& log_;
};

PointerEvent mouseAt(PointerEventType type, Point position) {
    PointerEvent event;
    event.type = type;
    event.position = position;
    event.button = 1;
    return event;
}

} // namespace

RIVET_TEST(layerEventOrderToolThenLayerThenViewportThenAfterMouse) {
    Fixture f;
    EventLog log;
    LoggingTool tool(log);
    LoggingLayer layer(log);
    LoggingBridge bridge(log);
    f.viewport.setTextBridge(&bridge);
    f.viewport.setActiveTool(&tool);
    f.viewport.setAnnotationLayer(&layer);
    CHECK(f.viewport.annotationLayer() == &layer);

    // Tool consumes: the layer and the viewport never see the event.
    tool.consume = true;
    const PointerEvent down = mouseAt(PointerEventType::Down, Point{100.0, 100.0});
    CHECK_EQ(f.viewport.onMouse(down), true);
    CHECK(log == (EventLog{"tool.mouse"}));
    log.clear();

    // Layer consumes: the viewport's selection never starts, no afterMouse.
    tool.consume = false;
    layer.consumeMouse = true;
    CHECK_EQ(f.viewport.onMouse(mouseAt(PointerEventType::Down, Point{100.0, 100.0})), true);
    CHECK(log == (EventLog{"tool.mouse", "layer.mouse"}));
    log.clear();

    // Unconsumed: the viewport handles it (starts a text selection), then
    // afterMouse runs.
    layer.consumeMouse = false;
    CHECK_EQ(f.viewport.onMouse(mouseAt(PointerEventType::Down, Point{100.0, 100.0})), true);
    CHECK(log == (EventLog{"tool.mouse", "layer.mouse", "bridge.began", "layer.after"}));
    log.clear();

    // afterMouse also follows events the viewport did not handle at all.
    f.viewport.onMouse(mouseAt(PointerEventType::Up, Point{100.0, 100.0}));
    log.clear();
    CHECK_EQ(f.viewport.onMouse(mouseAt(PointerEventType::Move, Point{700.0, 590.0})), false);
    CHECK(log == (EventLog{"tool.mouse", "layer.mouse", "layer.after"}));

    // Uninstalling stops the calls.
    log.clear();
    f.viewport.setAnnotationLayer(nullptr);
    CHECK(f.viewport.annotationLayer() == nullptr);
    f.viewport.onMouse(mouseAt(PointerEventType::Move, Point{700.0, 590.0}));
    CHECK(log == (EventLog{"tool.mouse"}));
}

RIVET_TEST(everyPressRequestsKeyboardFocusEvenWhenAToolOrLayerConsumesIt) {
    Fixture f;
    EventLog log;
    LoggingTool tool(log);
    LoggingLayer layer(log);
    int focusRequests = 0;
    f.viewport.setOnFocusRequested([&] {
        ++focusRequests;
        log.push_back("focus");
    });
    f.viewport.setAnnotationLayer(&layer);

    // Layer consumes: focus was requested before the layer saw the press.
    layer.consumeMouse = true;
    CHECK(f.viewport.onMouse(mouseAt(PointerEventType::Down, Point{100.0, 100.0})));
    CHECK_EQ(focusRequests, 1);
    CHECK(log == (EventLog{"focus", "layer.mouse"}));
    f.viewport.onMouse(mouseAt(PointerEventType::Up, Point{100.0, 100.0}));

    // Tool consumes.
    log.clear();
    focusRequests = 0;
    f.viewport.setActiveTool(&tool);
    tool.consume = true;
    CHECK(f.viewport.onMouse(mouseAt(PointerEventType::Down, Point{100.0, 100.0})));
    CHECK_EQ(focusRequests, 1);
    CHECK(log == (EventLog{"focus", "tool.mouse"}));

    // Nobody consumes: still exactly one request per press; moves and
    // releases never ask for focus.
    log.clear();
    focusRequests = 0;
    f.viewport.setActiveTool(nullptr);
    layer.consumeMouse = false;
    f.viewport.onMouse(mouseAt(PointerEventType::Down, Point{100.0, 100.0}));
    f.viewport.onMouse(mouseAt(PointerEventType::Move, Point{110.0, 110.0}));
    f.viewport.onMouse(mouseAt(PointerEventType::Up, Point{110.0, 110.0}));
    CHECK_EQ(focusRequests, 1);
}

RIVET_TEST(layerNeverSeesWheelScrollOrPinch) {
    Fixture f;
    EventLog log;
    LoggingLayer layer(log);
    f.viewport.setAnnotationLayer(&layer);

    PointerEvent scroll = scrollEvent(Point{0.0, 120.0}, false, false);
    CHECK_EQ(f.viewport.onMouse(scroll), true);
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{0.0, 120.0}, 1e-9));
    PointerEvent pinch = scrollEvent(Point{0.0, -1.0}, true, false);
    pinch.position = Point{400.0, 300.0};
    CHECK_EQ(f.viewport.onMouse(pinch), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);
    CHECK(log.empty());

    // A layer that would consume everything still cannot block the wheel.
    layer.consumeMouse = true;
    CHECK_EQ(f.viewport.onMouse(scrollEvent(Point{0.0, 10.0}, false, false)), true);
    CHECK(log.empty());
}

RIVET_TEST(layerKeysComeBeforeTheViewportAndAnActiveToolOwnsTheKeyboard) {
    Fixture f;
    EventLog log;
    LoggingTool tool(log);
    LoggingLayer layer(log);
    f.viewport.setAnnotationLayer(&layer);

    KeyEvent zoomIn;
    zoomIn.key = Key::Plus;
    // No tool, the layer declines: the viewport zooms.
    CHECK_EQ(f.viewport.onKey(zoomIn), true);
    CHECK(log == (EventLog{"layer.key"}));
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);

    // The layer consumes: the viewport does not zoom.
    log.clear();
    layer.consumeKey = true;
    CHECK_EQ(f.viewport.onKey(zoomIn), true);
    CHECK(log == (EventLog{"layer.key"}));
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);

    // An active tool owns the keyboard: a key it leaves alone never reaches
    // the layer (Delete must not remove an annotation while cropping); it
    // falls through to the viewport's own handling.
    log.clear();
    f.viewport.setActiveTool(&tool);
    CHECK_EQ(f.viewport.onKey(zoomIn), true);
    CHECK(log == (EventLog{"tool.key"}));
    CHECK(f.viewport.zoom().zoom() > 1.25);

    // The tool consumes: nothing else runs.
    log.clear();
    const double zoom = f.viewport.zoom().zoom();
    tool.consume = true;
    CHECK_EQ(f.viewport.onKey(zoomIn), true);
    CHECK(log == (EventLog{"tool.key"}));
    CHECK_NEAR(f.viewport.zoom().zoom(), zoom, 1e-12);
}

RIVET_TEST(presentationModePaintsTheLayerButSendsItNoInput) {
    Fixture f;
    EventLog log;
    LoggingLayer layer(log);
    f.viewport.setAnnotationLayer(&layer);
    f.viewport.setPresentationMode(true);

    KeyEvent key;
    key.key = Key::PageDown;
    CHECK_EQ(f.viewport.onKey(key), true);
    f.viewport.onMouse(mouseAt(PointerEventType::Down, Point{100.0, 100.0}));
    f.viewport.onMouse(mouseAt(PointerEventType::Move, Point{100.0, 100.0}));
    CHECK(log.empty());

    FakePaintContext context;
    f.viewport.paint(context);
    CHECK(!layer.pageRects.empty());
    CHECK(std::find(log.begin(), log.end(), "layer.above") != log.end());

    // Leaving presentation restores the input routing.
    f.viewport.setPresentationMode(false);
    log.clear();
    f.viewport.onMouse(mouseAt(PointerEventType::Move, Point{700.0, 590.0}));
    CHECK(!log.empty());
}

RIVET_TEST(layerPaintOrderPageAfterTilesBeforeOverlaysAboveBeforeTool) {
    Fixture f;
    EventLog log;
    LoggingTool tool(log);
    LoggingLayer layer(log);
    LoggingBridge bridge(log);
    f.viewport.setTextBridge(&bridge);
    f.viewport.setActiveTool(&tool);
    f.viewport.setAnnotationLayer(&layer);
    f.viewport.setScrollOffsetPoints(Point{0.0, 100.0});
    f.source.insertTile(TileKey{DocumentId{1}, PageId{11}, physicalKeyFor(1.0, 1.0), 0, 0}, 512, 512);

    FakePaintContext context;
    f.viewport.paint(context);

    // Page 0 is the only visible page at scroll y=100 (page 1 starts at 832).
    CHECK_EQ(layer.pageRects.size(), std::size_t{1});
    CHECK_EQ(layer.pageRects[0].first, std::size_t{0});
    // The rect equals both the host's frame and where the tile was drawn.
    CHECK(Rect::nearlyEqual(layer.pageRects[0].second, *f.viewport.pageRectInViewport(0), 1e-9));
    CHECK_EQ(context.bitmaps.size(), std::size_t{1});
    CHECK(Rect::nearlyEqual(context.bitmaps[0].dest,
                            Rect{layer.pageRects[0].second.minX(), layer.pageRects[0].second.minY(),
                                 512.0, 512.0},
                            1e-9));
    // Tiles were already painted, overlays not yet; chrome after overlays.
    CHECK_EQ(layer.bitmapsAtPage[0], std::size_t{1});
    CHECK_EQ(layer.overlaysAtPage[0], std::size_t{0});
    CHECK_EQ(layer.overlaysAtAbove, std::size_t{1});
    CHECK_EQ(LoggingLayer::countOverlays(context), std::size_t{1});
    // Above-layer chrome precedes the active tool.
    CHECK(log == (EventLog{"layer.page", "layer.above", "tool.paint"}));
}

RIVET_TEST(hostPageCountAndPageAtMapViewportPointsToPageDisplayPoints) {
    PdfViewport empty;
    empty.setFrame(Rect{0.0, 0.0, 800.0, 600.0});
    CHECK_EQ(empty.pageCount(), std::size_t{0});
    CHECK(!empty.pageAt(Point{10.0, 10.0}).has_value());

    Fixture f;
    CHECK_EQ(f.viewport.pageCount(), std::size_t{2});
    // Zoom 1, no scroll: viewport (100, 100) is page-0 point (76, 76).
    auto hit = f.viewport.pageAt(Point{100.0, 100.0});
    CHECK(hit.has_value());
    CHECK_EQ(hit->first, std::size_t{0});
    CHECK(Point::nearlyEqual(hit->second, Point{76.0, 76.0}, 1e-9));
    // The margin outside the page maps to nothing.
    CHECK(!f.viewport.pageAt(Point{10.0, 10.0}).has_value());

    // Zoom 2, scrolled to content (100, 700).
    CHECK(f.viewport.zoom().setZoom(2.0));
    f.viewport.setScrollOffsetPoints(Point{100.0, 700.0});
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{100.0, 700.0}, 1e-9));
    // Page 0 frame in viewport = {(24-100)*2, (24-700)*2, ...} = {-152, -1352}.
    const auto onFirst = f.viewport.pageAt(Point{200.0, 100.0});
    CHECK(onFirst.has_value());
    CHECK_EQ(onFirst->first, std::size_t{0});
    CHECK(Point::nearlyEqual(onFirst->second, Point{176.0, 726.0}, 1e-9));
    // Page 1 frame in viewport = {60, 264, 800, 800}.
    const auto onSecond = f.viewport.pageAt(Point{200.0, 300.0});
    CHECK(onSecond.has_value());
    CHECK_EQ(onSecond->first, std::size_t{1});
    CHECK(Point::nearlyEqual(onSecond->second, Point{70.0, 18.0}, 1e-9));
    // The gap between the pages (page 0 ends at y=232, page 1 starts at 264).
    CHECK(!f.viewport.pageAt(Point{200.0, 248.0}).has_value());
    // Consistent with pageRectInViewport.
    CHECK(Rect::nearlyEqual(*f.viewport.pageRectInViewport(1), Rect{60.0, 264.0, 800.0, 800.0}, 1e-9));
}
