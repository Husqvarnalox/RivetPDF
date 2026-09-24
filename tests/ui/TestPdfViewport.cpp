#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/PageLayout.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderScaleKey.hpp"
#include "render/TileKey.hpp"
#include "render/ZoomState.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/UiTypes.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using rivet::core::DocumentId;
using rivet::core::PageId;
using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::render::PageLayout;
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
    PdfViewport viewport;

    Fixture() {
        std::vector<PageLayout::PageInfo> pages;
        pages.push_back(PageLayout::PageInfo{PageId{11}, Size{612.0, 792.0}});
        pages.push_back(PageLayout::PageInfo{PageId{22}, Size{400.0, 400.0}});
        layout.setPages(pages);
        layout.setPageGapPoints(16.0);
        layout.setPageMarginPoints(24.0);

        viewport.setFrame(Rect{0.0, 0.0, 800.0, 600.0});
        viewport.setDocument(DocumentId{1}, &layout, &source, [] { return kRevision; });
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
    const TileKey originTile{DocumentId{1}, PageId{11}, RenderScaleKey::fromZoom(1.0), 0, 0};
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
    // paints. It is 612x792 points -> a 2x2 tile grid at zoom 1, all missing.
    CHECK_EQ(f.source.requests.size(), std::size_t{4});
    for (const auto& record : f.source.requests) {
        CHECK(record.priority == RenderPriority::Visible);
        CHECK_EQ(record.request.key.documentId, DocumentId{1});
        CHECK_EQ(record.request.key.pageId, PageId{11});
        CHECK_EQ(record.request.key.scale, RenderScaleKey::fromZoom(1.0));
        CHECK_NEAR(record.request.params.devicePixelsPerPoint, 1.0, 1e-12);
    }

    // Tile (1, 0) is clipped to the page's 612pt width: 100 points wide.
    bool sawRightTile = false;
    for (const auto& record : f.source.requests) {
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

RIVET_TEST(commandWheelZoomsAnchoredAtCenter) {
    Fixture f;

    int zoomCallbacks = 0;
    double reportedZoom = 0.0;
    f.viewport.setZoomChangedCallback([&zoomCallbacks, &reportedZoom](double value) {
        ++zoomCallbacks;
        reportedZoom = value;
    });

    // deltaY < 0 with command: zoom in one step.
    CHECK_EQ(f.viewport.onMouse(scrollEvent(Point{0.0, -1.0}, true, false)), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);
    CHECK_EQ(zoomCallbacks, 1);
    CHECK_NEAR(reportedZoom, 1.25, 1e-12);

    // Anchor: the content point at the viewport center {400, 300} stays put
    // -> {400, 300} - {400, 300}/1.25 = {80, 60}; x clamps to 660 - 640 = 20.
    CHECK(Point::nearlyEqual(f.viewport.scrollOffsetPoints(), Point{20.0, 60.0}, 1e-9));

    // deltaY >= 0 with command: zoom back out, re-anchored.
    CHECK_EQ(f.viewport.onMouse(scrollEvent(Point{0.0, 1.0}, true, false)), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.0, 1e-12);
    CHECK_EQ(zoomCallbacks, 2);

    // Control behaves like command.
    CHECK_EQ(f.viewport.onMouse(scrollEvent(Point{0.0, -1.0}, false, true)), true);
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.25, 1e-12);
    CHECK_EQ(zoomCallbacks, 3);
}

RIVET_TEST(zoomChangesQuantizeTileRequests) {
    Fixture f;
    CHECK(f.viewport.zoom().setZoom(2.0));

    FakePaintContext context;
    f.viewport.paint(context);

    // Visible content rect at zoom 2: {0, 0, 400, 300} -> page 0 only.
    // Tile extent is 512/2 = 256 points; the 376x276 visible slice of the
    // page touches a 2x2 tile block.
    CHECK_EQ(f.source.requests.size(), std::size_t{4});
    const RenderScaleKey quantized = RenderScaleKey::fromZoom(2.0);
    CHECK_EQ(quantized.value, std::uint32_t{128}); // ceil(2.0 * 64) / 64 == 2.0
    for (const auto& record : f.source.requests) {
        CHECK_EQ(record.request.key.scale, quantized);
        CHECK_NEAR(record.request.params.devicePixelsPerPoint, record.request.key.scale.scale(), 1e-12);
        CHECK_NEAR(record.request.params.devicePixelsPerPoint, 2.0, 1e-12);
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

RIVET_TEST(resizeReclampsScrollAndResolvesFitWidth) {
    Fixture f;
    f.viewport.setScrollOffsetPoints(Point{0.0, 656.0});

    // Taller viewport extends past the content: max y becomes 1256 - 700 = 556.
    f.viewport.setFrame(Rect{0.0, 0.0, 800.0, 700.0});
    CHECK_NEAR(f.viewport.scrollOffsetPoints().y, 556.0, 1e-9);

    // Fit-width intent resolves once on the next layout pass: 612 / 612 = 1.0
    // (zoom was user-set to 1.25, so this is a real change back to 1.0).
    CHECK(f.viewport.zoom().setZoom(1.25));
    f.viewport.zoom().setFitMode(ZoomState::FitMode::Width);
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::Width);
    f.viewport.setFrame(Rect{0.0, 0.0, 612.0, 700.0});
    CHECK_NEAR(f.viewport.zoom().zoom(), 1.0, 1e-9);
    CHECK(f.viewport.zoom().fitMode() == ZoomState::FitMode::None); // one-shot
}

RIVET_TEST(clearDocumentCancelsPendingAndRepaintsEmptyState) {
    Fixture f;
    FakePaintContext before;
    f.viewport.paint(before);
    CHECK_EQ(f.source.requests.size(), std::size_t{4});

    f.viewport.clearDocument();
    CHECK_EQ(f.source.cancelAllCount, 1);

    FakePaintContext after;
    f.viewport.paint(after);
    CHECK(after.bitmaps.empty());
    CHECK_EQ(f.source.requests.size(), std::size_t{4}); // no new requests
    CHECK_EQ(hasText(after, "Open a PDF to begin"), true);
}
