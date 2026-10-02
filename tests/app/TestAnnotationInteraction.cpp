// SPDX-License-Identifier: MPL-2.0
// AnnotationInteraction: the pure state machine of the annotation tools. No
// widgets, editor or platform: pointer/key inputs in page display points go in,
// intents and previews come out.
#include "RivetTest.h"

#include "app/AnnotationInteraction.hpp"

#include <cmath>
#include <vector>

using rivet::app::AnnotationInteraction;
using rivet::app::AnnotationTool;
using rivet::core::AnnotationId;
using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using Handle = AnnotationInteraction::Handle;
using Intent = AnnotationInteraction::Intent;
using IntentKind = AnnotationInteraction::Intent::Kind;
using PreviewKind = AnnotationInteraction::Preview::Kind;

namespace {

constexpr Size kPage{600.0, 800.0};

// A scripted hit callback: annotations of page 0 in z-order (last = topmost).
struct Fixture {
    AnnotationInteraction machine;
    std::vector<AnnotationInteraction::HitInfo> hits;
    int hitCalls = 0;

    explicit Fixture(AnnotationTool tool = AnnotationTool::Select) {
        machine.setHitTest([this](std::size_t page, Point point, double tolerance) {
            ++hitCalls;
            std::optional<AnnotationInteraction::HitInfo> found;
            if (page != 0) return found;
            for (const auto& hit : hits) {
                const Rect grown{hit.bounds.minX() - tolerance, hit.bounds.minY() - tolerance,
                                 hit.bounds.size.width + 2.0 * tolerance,
                                 hit.bounds.size.height + 2.0 * tolerance};
                if (grown.contains(point)) found = hit;
            }
            return found;
        });
        machine.setTool(tool);
    }

    AnnotationInteraction::HitInfo& addHit(std::uint64_t id, Rect bounds, bool move = true, bool resize = true) {
        AnnotationInteraction::HitInfo info;
        info.id = AnnotationId{id};
        info.bounds = bounds;
        info.canMove = move;
        info.canResize = resize;
        info.canEditContents = false;
        hits.push_back(info);
        return hits.back();
    }

    static AnnotationInteraction::PointerInput input(Point point, bool shift = false, double zoom = 1.0,
                                                     int clicks = 1) {
        AnnotationInteraction::PointerInput in;
        in.page = 0;
        in.point = point;
        in.pageSize = kPage;
        in.shift = shift;
        in.zoom = zoom;
        in.clickCount = clicks;
        return in;
    }
    Intent down(Point p, bool shift = false, double zoom = 1.0, int clicks = 1) {
        return machine.pointerDown(input(p, shift, zoom, clicks));
    }
    Intent move(Point p, bool shift = false, double zoom = 1.0) {
        return machine.pointerMove(input(p, shift, zoom));
    }
    Intent up(Point p, bool shift = false, double zoom = 1.0) { return machine.pointerUp(input(p, shift, zoom)); }
    // Press, drag, release.
    Intent drag(Point from, Point to, bool shift = false, double zoom = 1.0) {
        (void)down(from, shift, zoom);
        (void)move(Point{(from.x + to.x) / 2.0, (from.y + to.y) / 2.0}, shift, zoom);
        return up(to, shift, zoom);
    }
};

bool near(Point a, Point b, double eps = 1e-6) { return std::abs(a.x - b.x) <= eps && std::abs(a.y - b.y) <= eps; }
bool nearRect(const Rect& a, const Rect& b, double eps = 1e-6) {
    return near(a.origin, b.origin, eps) && std::abs(a.size.width - b.size.width) <= eps &&
           std::abs(a.size.height - b.size.height) <= eps;
}

} // namespace

// --- Note -----------------------------------------------------------------------

RIVET_TEST(noteClickCreatesAnIconBoxAtThePoint) {
    Fixture f(AnnotationTool::Note);
    const Intent pressed = f.down(Point{100.0, 200.0});
    CHECK(pressed.consumed);
    CHECK(pressed.kind == IntentKind::None);
    const Intent released = f.up(Point{100.0, 200.0});
    CHECK(released.consumed);
    CHECK(released.kind == IntentKind::CreateNote);
    CHECK(nearRect(released.rect, Rect{100.0, 200.0, 20.0, 20.0}));
    CHECK_EQ(released.page, 0u);
    CHECK(!f.machine.gestureActive());
}

RIVET_TEST(noteBoxIsKeptInsideThePage) {
    Fixture f(AnnotationTool::Note);
    (void)f.down(Point{599.0, 799.0});
    const Intent released = f.up(Point{599.0, 799.0});
    CHECK(released.kind == IntentKind::CreateNote);
    CHECK(nearRect(released.rect, Rect{580.0, 780.0, 20.0, 20.0}));
}

RIVET_TEST(noteDragPastTheThresholdCreatesNothing) {
    Fixture f(AnnotationTool::Note);
    (void)f.down(Point{100.0, 100.0});
    (void)f.move(Point{110.0, 100.0});
    const Intent released = f.up(Point{110.0, 100.0});
    CHECK(released.kind == IntentKind::None);
    CHECK(released.consumed);
}

RIVET_TEST(noteThresholdIsInLogicalPointsSoItScalesWithZoom) {
    // 3 logical points = 0.75 page points at zoom 4: a 1 page point wobble is
    // already a drag; at zoom 0.5 (6 page points) it is still a click.
    Fixture zoomedIn(AnnotationTool::Note);
    (void)zoomedIn.down(Point{100.0, 100.0}, false, 4.0);
    CHECK(zoomedIn.up(Point{101.0, 100.0}, false, 4.0).kind == IntentKind::None);
    Fixture zoomedOut(AnnotationTool::Note);
    (void)zoomedOut.down(Point{100.0, 100.0}, false, 0.5);
    CHECK(zoomedOut.up(Point{105.0, 100.0}, false, 0.5).kind == IntentKind::CreateNote);
}

// --- Ink --------------------------------------------------------------------------

RIVET_TEST(inkCapturesPointsAndCreatesOneStrokeOnRelease) {
    Fixture f(AnnotationTool::Ink);
    CHECK(f.down(Point{10.0, 10.0}).consumed);
    CHECK(f.move(Point{30.0, 20.0}).kind == IntentKind::None);
    CHECK(f.machine.gestureInProgress());
    const auto preview = f.machine.preview();
    CHECK(preview.kind == PreviewKind::Ink);
    CHECK_EQ(preview.points.size(), 2u);
    (void)f.move(Point{50.0, 10.0});
    const Intent released = f.up(Point{70.0, 30.0}, false, 2.0);
    CHECK(released.kind == IntentKind::CreateInk);
    CHECK_EQ(released.strokes.size(), 1u);
    CHECK_EQ(released.strokes[0].size(), 4u);
    CHECK(near(released.strokes[0].front(), Point{10.0, 10.0}));
    CHECK(near(released.strokes[0].back(), Point{70.0, 30.0}));
    CHECK_NEAR(released.zoom, 2.0, 1e-9);
    CHECK(f.machine.preview().kind == PreviewKind::None);
}

RIVET_TEST(inkClickWithoutTravelCreatesNothing) {
    Fixture f(AnnotationTool::Ink);
    (void)f.down(Point{10.0, 10.0});
    (void)f.move(Point{10.5, 10.0});
    const Intent released = f.up(Point{10.5, 10.0});
    CHECK(released.kind == IntentKind::None);
    CHECK(released.consumed);
}

RIVET_TEST(inkDrawsOverAnnotationsAndIgnoresHandles) {
    Fixture f(AnnotationTool::Ink);
    f.addHit(7, Rect{50.0, 50.0, 100.0, 100.0});
    f.machine.setSelection(AnnotationInteraction::Selected{0, f.hits.back()});
    // Press exactly on the selection's top-left handle, which Ink ignores.
    CHECK(f.down(Point{50.0, 50.0}).kind == IntentKind::None);
    (void)f.move(Point{80.0, 80.0});
    const Intent released = f.up(Point{120.0, 120.0});
    CHECK(released.kind == IntentKind::CreateInk);
    CHECK_EQ(f.hitCalls, 0);
}

RIVET_TEST(inkPointsAreClampedIntoThePageAndCapped) {
    Fixture f(AnnotationTool::Ink);
    (void)f.down(Point{-10.0, 5.0});
    (void)f.move(Point{700.0, 900.0});
    const Intent released = f.up(Point{700.0, 900.0});
    CHECK(released.kind == IntentKind::CreateInk);
    CHECK(near(released.strokes[0].front(), Point{0.0, 5.0}));
    CHECK(near(released.strokes[0].back(), Point{600.0, 800.0}));

    Fixture many(AnnotationTool::Ink);
    (void)many.down(Point{0.0, 0.0});
    for (std::size_t i = 1; i < AnnotationInteraction::kMaxInkPoints + 500; ++i) {
        (void)many.move(Point{static_cast<double>(i % 500), static_cast<double>(i % 7 + (i % 2 ? 1 : 0))});
    }
    const Intent big = many.up(Point{599.0, 799.0});
    CHECK(big.kind == IntentKind::CreateInk);
    CHECK_LE(big.strokes[0].size(), AnnotationInteraction::kMaxInkPoints);
}

// --- Rectangle / Ellipse ----------------------------------------------------------------

RIVET_TEST(rectangleDragCreatesANormalizedRect) {
    Fixture f(AnnotationTool::Rectangle);
    const Intent released = f.drag(Point{200.0, 300.0}, Point{100.0, 250.0});
    CHECK(released.kind == IntentKind::CreateShape);
    CHECK(released.tool == AnnotationTool::Rectangle);
    CHECK(nearRect(released.rect, Rect{100.0, 250.0, 100.0, 50.0}));
}

RIVET_TEST(ellipseUsesTheSameDragRect) {
    Fixture f(AnnotationTool::Ellipse);
    const Intent released = f.drag(Point{10.0, 10.0}, Point{60.0, 40.0});
    CHECK(released.kind == IntentKind::CreateShape);
    CHECK(released.tool == AnnotationTool::Ellipse);
    CHECK(nearRect(released.rect, Rect{10.0, 10.0, 50.0, 30.0}));
}

RIVET_TEST(rectangleClickAndSubThresholdDragCreateNothing) {
    Fixture f(AnnotationTool::Rectangle);
    (void)f.down(Point{100.0, 100.0});
    CHECK(f.up(Point{100.0, 100.0}).kind == IntentKind::None);
    (void)f.down(Point{100.0, 100.0});
    (void)f.move(Point{101.0, 101.0});
    CHECK(f.up(Point{101.0, 101.0}).kind == IntentKind::None);
    CHECK(f.machine.preview().kind == PreviewKind::None);
}

RIVET_TEST(rectangleDragThatIsFlatOnOneAxisCreatesNothing) {
    Fixture f(AnnotationTool::Rectangle);
    CHECK(f.drag(Point{100.0, 100.0}, Point{200.0, 100.0}).kind == IntentKind::None);
}

RIVET_TEST(shiftMakesSquaresAndCirclesInTheDragDirection) {
    Fixture f(AnnotationTool::Rectangle);
    const Intent released = f.drag(Point{200.0, 200.0}, Point{120.0, 150.0}, true);
    CHECK(released.kind == IntentKind::CreateShape);
    CHECK(nearRect(released.rect, Rect{120.0, 120.0, 80.0, 80.0}));

    Fixture circle(AnnotationTool::Ellipse);
    const Intent c = circle.drag(Point{100.0, 100.0}, Point{130.0, 190.0}, true);
    CHECK(nearRect(c.rect, Rect{100.0, 100.0, 90.0, 90.0}));
}

RIVET_TEST(shiftSquareStaysInsideThePage) {
    const Rect square = AnnotationInteraction::dragRect(Point{550.0, 100.0}, Point{600.0, 400.0}, true, kPage);
    CHECK(nearRect(square, Rect{550.0, 100.0, 50.0, 50.0}));
}

RIVET_TEST(shapePreviewFollowsTheDragAndShiftToggleLive) {
    Fixture f(AnnotationTool::Rectangle);
    (void)f.down(Point{100.0, 100.0});
    CHECK(f.machine.preview().kind == PreviewKind::None); // below the threshold
    (void)f.move(Point{160.0, 130.0});
    auto preview = f.machine.preview();
    CHECK(preview.kind == PreviewKind::Rect);
    CHECK(!preview.editing);
    CHECK(nearRect(preview.rect, Rect{100.0, 100.0, 60.0, 30.0}));
    (void)f.move(Point{160.0, 130.0}, true);
    preview = f.machine.preview();
    CHECK(nearRect(preview.rect, Rect{100.0, 100.0, 60.0, 60.0}));
}

RIVET_TEST(escapeCancelsTheGestureAndTheReleaseIsSwallowed) {
    Fixture f(AnnotationTool::Rectangle);
    (void)f.down(Point{100.0, 100.0});
    (void)f.move(Point{200.0, 200.0});
    CHECK(f.machine.gestureInProgress());
    const Intent esc = f.machine.key(AnnotationInteraction::KeyInput::Escape);
    CHECK(esc.consumed);
    CHECK(esc.kind == IntentKind::None);
    CHECK(!f.machine.gestureInProgress());
    CHECK(f.machine.preview().kind == PreviewKind::None);
    (void)f.move(Point{250.0, 250.0});
    const Intent released = f.up(Point{250.0, 250.0});
    CHECK(released.consumed);
    CHECK(released.kind == IntentKind::None);
    // The tool is still Rectangle (a second Esc goes down the chain).
    CHECK(f.machine.tool() == AnnotationTool::Rectangle);
}

// --- Line / Arrow ---------------------------------------------------------------------

RIVET_TEST(lineDragCreatesEndpoints) {
    Fixture f(AnnotationTool::Line);
    const Intent released = f.drag(Point{10.0, 20.0}, Point{110.0, 70.0});
    CHECK(released.kind == IntentKind::CreateShape);
    CHECK(released.tool == AnnotationTool::Line);
    CHECK(near(released.a, Point{10.0, 20.0}));
    CHECK(near(released.b, Point{110.0, 70.0}));
}

RIVET_TEST(arrowClickCreatesNothingAndShiftSnapsTo45Degrees) {
    Fixture f(AnnotationTool::Arrow);
    (void)f.down(Point{100.0, 100.0});
    CHECK(f.up(Point{101.0, 100.0}).kind == IntentKind::None);

    // Nearly horizontal -> exactly horizontal, same length.
    const Intent horizontal = f.drag(Point{100.0, 100.0}, Point{200.0, 112.0}, true);
    CHECK(horizontal.kind == IntentKind::CreateShape);
    CHECK(horizontal.tool == AnnotationTool::Arrow);
    CHECK_NEAR(horizontal.b.y, 100.0, 1e-6);
    CHECK_NEAR(horizontal.b.x - 100.0, std::hypot(100.0, 12.0), 1e-6);
    // Nearly diagonal -> exact 45 degrees.
    const Intent diagonal = f.drag(Point{100.0, 100.0}, Point{190.0, 200.0}, true);
    CHECK_NEAR(diagonal.b.x - 100.0, diagonal.b.y - 100.0, 1e-6);
    // Vertical.
    const Intent vertical = f.drag(Point{300.0, 300.0}, Point{305.0, 100.0}, true);
    CHECK_NEAR(vertical.b.x, 300.0, 1e-6);
}

RIVET_TEST(linePreviewIsALine) {
    Fixture f(AnnotationTool::Line);
    (void)f.down(Point{10.0, 10.0});
    (void)f.move(Point{50.0, 10.0});
    const auto preview = f.machine.preview();
    CHECK(preview.kind == PreviewKind::Line);
    CHECK(preview.tool == AnnotationTool::Line);
    CHECK(near(preview.a, Point{10.0, 10.0}));
    CHECK(near(preview.b, Point{50.0, 10.0}));
}

// --- Stamp ----------------------------------------------------------------------------

RIVET_TEST(stampClickPlacesTheDefaultSizeCentered) {
    Fixture f(AnnotationTool::Stamp);
    (void)f.down(Point{300.0, 400.0});
    const Intent released = f.up(Point{300.0, 400.0});
    CHECK(released.kind == IntentKind::CreateStamp);
    CHECK(nearRect(released.rect, Rect{225.0, 375.0, 150.0, 50.0}));
}

RIVET_TEST(stampClickNearTheEdgeStaysInsideThePage) {
    const Rect corner = AnnotationInteraction::defaultStampRect(Point{5.0, 5.0}, kPage);
    CHECK(nearRect(corner, Rect{0.0, 0.0, 150.0, 50.0}));
    const Rect far = AnnotationInteraction::defaultStampRect(Point{599.0, 799.0}, kPage);
    CHECK(nearRect(far, Rect{450.0, 750.0, 150.0, 50.0}));
}

RIVET_TEST(stampDragSetsTheSize) {
    Fixture f(AnnotationTool::Stamp);
    const Intent released = f.drag(Point{100.0, 100.0}, Point{260.0, 160.0});
    CHECK(released.kind == IntentKind::CreateStamp);
    CHECK(nearRect(released.rect, Rect{100.0, 100.0, 160.0, 60.0}));
    // A flat drag creates nothing.
    CHECK(f.drag(Point{100.0, 100.0}, Point{260.0, 100.0}).kind == IntentKind::None);
}

RIVET_TEST(stampDefaultSizeDoesNotDependOnZoom) {
    Fixture f(AnnotationTool::Stamp);
    (void)f.down(Point{300.0, 400.0}, false, 4.0);
    const Intent released = f.up(Point{300.0, 400.0}, false, 4.0);
    CHECK_NEAR(released.rect.size.width, 150.0, 1e-9);
    CHECK_NEAR(released.rect.size.height, 50.0, 1e-9);
}

// --- Handles ------------------------------------------------------------------------------

RIVET_TEST(handleHitRadiusIsFixedInLogicalPoints) {
    const Rect bounds{100.0, 100.0, 200.0, 100.0};
    for (const double zoom : {0.5, 1.0, 4.0}) {
        const double radius = AnnotationInteraction::kHandleHitRadius / zoom;
        // Just inside / outside the radius of the top-left corner.
        CHECK(AnnotationInteraction::handleAt(bounds, Point{100.0 + radius * 0.9, 100.0 - radius * 0.9}, zoom) ==
              Handle::TopLeft);
        CHECK(AnnotationInteraction::handleAt(bounds, Point{100.0 - radius * 1.2, 100.0}, zoom) == Handle::None);
    }
}

RIVET_TEST(allEightHandlesAreFoundAndCornersWinOverEdges) {
    const Rect bounds{100.0, 100.0, 200.0, 100.0};
    using AI = AnnotationInteraction;
    CHECK(AI::handleAt(bounds, Point{100.0, 100.0}, 1.0) == Handle::TopLeft);
    CHECK(AI::handleAt(bounds, Point{200.0, 100.0}, 1.0) == Handle::Top);
    CHECK(AI::handleAt(bounds, Point{300.0, 100.0}, 1.0) == Handle::TopRight);
    CHECK(AI::handleAt(bounds, Point{300.0, 150.0}, 1.0) == Handle::Right);
    CHECK(AI::handleAt(bounds, Point{300.0, 200.0}, 1.0) == Handle::BottomRight);
    CHECK(AI::handleAt(bounds, Point{200.0, 200.0}, 1.0) == Handle::Bottom);
    CHECK(AI::handleAt(bounds, Point{100.0, 200.0}, 1.0) == Handle::BottomLeft);
    CHECK(AI::handleAt(bounds, Point{100.0, 150.0}, 1.0) == Handle::Left);
    CHECK(AI::handleAt(bounds, Point{200.0, 150.0}, 1.0) == Handle::None); // interior
    // A tiny box: the corner handles overlap the edge handles; corners win.
    CHECK(AI::handleAt(Rect{100.0, 100.0, 4.0, 4.0}, Point{102.0, 100.0}, 1.0) == Handle::TopLeft);
}

RIVET_TEST(lineEndpointHandles) {
    using AI = AnnotationInteraction;
    CHECK(AI::lineHandleAt(Point{10.0, 10.0}, Point{200.0, 100.0}, Point{12.0, 8.0}, 1.0) == Handle::LineStart);
    CHECK(AI::lineHandleAt(Point{10.0, 10.0}, Point{200.0, 100.0}, Point{198.0, 103.0}, 1.0) == Handle::LineEnd);
    CHECK(AI::lineHandleAt(Point{10.0, 10.0}, Point{200.0, 100.0}, Point{100.0, 55.0}, 1.0) == Handle::None);
    // At zoom 4 the hit radius is 1.75 page points.
    CHECK(AI::lineHandleAt(Point{10.0, 10.0}, Point{200.0, 100.0}, Point{12.0, 8.0}, 4.0) == Handle::None);
}

RIVET_TEST(resizedRectMovesTheDraggedEdgesAndFlips) {
    using AI = AnnotationInteraction;
    const Rect r{100.0, 100.0, 100.0, 50.0};
    CHECK(nearRect(AI::resizedRect(r, Handle::BottomRight, Point{250.0, 200.0}), Rect{100.0, 100.0, 150.0, 100.0}));
    CHECK(nearRect(AI::resizedRect(r, Handle::Left, Point{80.0, 0.0}), Rect{80.0, 100.0, 120.0, 50.0}));
    CHECK(nearRect(AI::resizedRect(r, Handle::Top, Point{0.0, 120.0}), Rect{100.0, 120.0, 100.0, 30.0}));
    // Past the opposite edge: normalized.
    CHECK(nearRect(AI::resizedRect(r, Handle::Right, Point{60.0, 0.0}), Rect{60.0, 100.0, 40.0, 50.0}));
}

// --- Selection by hit ------------------------------------------------------------------------

RIVET_TEST(selectToolClickOnAnAnnotationSelectsIt) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0}, false, false);
    const Intent pressed = f.down(Point{120.0, 120.0});
    CHECK(pressed.consumed);
    CHECK(pressed.kind == IntentKind::Select);
    CHECK(pressed.id == AnnotationId{5});
    CHECK(f.machine.selection().has_value());
    // The release of a select-only press is consumed and does nothing.
    const Intent released = f.up(Point{120.0, 120.0});
    CHECK(released.consumed);
    CHECK(released.kind == IntentKind::None);
    // Clicking the selected one again changes nothing.
    CHECK(f.down(Point{121.0, 121.0}).kind == IntentKind::None);
    (void)f.up(Point{121.0, 121.0});
}

RIVET_TEST(hitToleranceIsFourLogicalPoints) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    // 3 page points outside at zoom 1: still a hit; 5: a miss.
    CHECK(f.down(Point{97.0, 120.0}).kind == IntentKind::Select);
    (void)f.up(Point{97.0, 120.0});
    f.machine.setSelection(std::nullopt);
    CHECK(f.down(Point{95.0, 120.0}).kind == IntentKind::PassThrough);
    (void)f.up(Point{95.0, 120.0});
    // At zoom 4 the tolerance shrinks to 1 page point.
    CHECK(f.down(Point{97.0, 120.0}, false, 4.0).kind == IntentKind::PassThrough);
}

RIVET_TEST(selectToolMissPassesThroughAndClearsTheSelection) {
    Fixture f(AnnotationTool::Select);
    const Intent none = f.down(Point{300.0, 300.0});
    CHECK(!none.consumed);
    CHECK(none.kind == IntentKind::PassThrough);
    // The viewport's moves and release pass through too.
    CHECK(!f.move(Point{310.0, 300.0}).consumed);
    CHECK(!f.up(Point{310.0, 300.0}).consumed);

    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    (void)f.down(Point{120.0, 120.0});
    (void)f.up(Point{120.0, 120.0});
    CHECK(f.machine.selection().has_value());
    const Intent clear = f.down(Point{400.0, 400.0});
    CHECK(clear.kind == IntentKind::ClearSelection);
    CHECK(!clear.consumed);
    CHECK(!f.machine.selection().has_value());
}

RIVET_TEST(aHitSelectsUnderEveryToolExceptInk) {
    for (const AnnotationTool tool :
         {AnnotationTool::Highlight, AnnotationTool::Underline, AnnotationTool::StrikeOut, AnnotationTool::Note,
          AnnotationTool::Rectangle, AnnotationTool::Ellipse, AnnotationTool::Line, AnnotationTool::Arrow,
          AnnotationTool::Stamp}) {
        Fixture f(tool);
        f.addHit(9, Rect{100.0, 100.0, 80.0, 40.0});
        const Intent pressed = f.down(Point{120.0, 120.0});
        CHECK(pressed.kind == IntentKind::Select);
        CHECK(pressed.consumed);
        const Intent released = f.up(Point{120.0, 120.0});
        CHECK(released.kind == IntentKind::None); // no creation
    }
}

RIVET_TEST(markupToolsLeaveTextSelectionToTheViewportOnAMiss) {
    for (const AnnotationTool tool :
         {AnnotationTool::Highlight, AnnotationTool::Underline, AnnotationTool::StrikeOut}) {
        Fixture f(tool);
        const Intent pressed = f.down(Point{300.0, 300.0});
        CHECK(!pressed.consumed);
        CHECK(pressed.kind == IntentKind::PassThrough);
        CHECK(!f.up(Point{350.0, 300.0}).consumed);
        // After the viewport handled the release: markup from a text selection.
        const Intent convert = f.machine.afterUnconsumedUp(true);
        CHECK(convert.kind == IntentKind::ConvertTextSelection);
        CHECK(convert.tool == tool);
        CHECK(f.machine.afterUnconsumedUp(false).kind == IntentKind::PassThrough);
    }
    Fixture select(AnnotationTool::Select);
    CHECK(select.machine.afterUnconsumedUp(true).kind == IntentKind::PassThrough);
    Fixture rect(AnnotationTool::Rectangle);
    CHECK(!rect.machine.afterUnconsumedUp(true).consumed);
}

// --- Move ---------------------------------------------------------------------------------------

RIVET_TEST(dragInsideAMovableSelectionPreviewsAndCommitsOneMove) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    CHECK(f.down(Point{120.0, 120.0}).kind == IntentKind::Select);
    (void)f.move(Point{130.0, 125.0});
    auto preview = f.machine.preview();
    CHECK(preview.kind == PreviewKind::Bounds);
    CHECK(preview.editing);
    CHECK(nearRect(preview.rect, Rect{110.0, 105.0, 80.0, 40.0}));
    (void)f.move(Point{150.0, 140.0});
    preview = f.machine.preview();
    CHECK(nearRect(preview.rect, Rect{130.0, 120.0, 80.0, 40.0}));
    const Intent released = f.up(Point{150.0, 140.0});
    CHECK(released.kind == IntentKind::Move);
    CHECK(released.id == AnnotationId{5});
    CHECK(near(released.delta, Point{30.0, 20.0}));
    CHECK(f.machine.preview().kind == PreviewKind::None);
}

RIVET_TEST(moveBelowTheThresholdCommitsNothing) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    (void)f.down(Point{120.0, 120.0});
    (void)f.move(Point{121.0, 121.0});
    CHECK(f.machine.preview().kind == PreviewKind::None);
    CHECK(f.up(Point{121.0, 121.0}).kind == IntentKind::None);
}

RIVET_TEST(escapeDuringAMoveCancelsIt) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    (void)f.down(Point{120.0, 120.0});
    (void)f.move(Point{160.0, 160.0});
    CHECK(f.machine.key(AnnotationInteraction::KeyInput::Escape).consumed);
    CHECK(f.up(Point{160.0, 160.0}).kind == IntentKind::None);
    // The annotation is still selected.
    CHECK(f.machine.selection().has_value());
}

RIVET_TEST(aNonMovableAnnotationSelectsButDoesNotMove) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0}, false, false);
    (void)f.down(Point{120.0, 120.0});
    (void)f.move(Point{200.0, 200.0});
    CHECK(f.machine.preview().kind == PreviewKind::None);
    CHECK(f.up(Point{200.0, 200.0}).kind == IntentKind::None);
}

RIVET_TEST(movingALinePreviewsTheLine) {
    Fixture f(AnnotationTool::Select);
    auto& hit = f.addHit(5, Rect{100.0, 100.0, 100.0, 50.0});
    hit.isLine = true;
    hit.lineStart = Point{100.0, 100.0};
    hit.lineEnd = Point{200.0, 150.0};
    (void)f.down(Point{150.0, 125.0});
    (void)f.move(Point{160.0, 125.0});
    const auto preview = f.machine.preview();
    CHECK(preview.kind == PreviewKind::Line);
    CHECK(near(preview.a, Point{110.0, 100.0}));
    CHECK(near(preview.b, Point{210.0, 150.0}));
}

RIVET_TEST(movePointIsClampedToThePage) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{500.0, 100.0, 50.0, 40.0});
    (void)f.down(Point{520.0, 120.0});
    (void)f.move(Point{900.0, 120.0});
    const Intent released = f.up(Point{900.0, 120.0});
    CHECK(released.kind == IntentKind::Move);
    CHECK_NEAR(released.delta.x, 80.0, 1e-9);
}

// --- Resize ---------------------------------------------------------------------------------------

RIVET_TEST(draggingAHandleResizesWithOneCommit) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    f.machine.setSelection(AnnotationInteraction::Selected{0, f.hits.back()});
    // Press on the bottom-right handle (180, 140).
    const Intent pressed = f.down(Point{181.0, 139.0});
    CHECK(pressed.consumed);
    CHECK(pressed.kind == IntentKind::None);
    (void)f.move(Point{200.0, 150.0});
    auto preview = f.machine.preview();
    CHECK(preview.kind == PreviewKind::Bounds);
    CHECK(preview.editing);
    CHECK(nearRect(preview.rect, Rect{100.0, 100.0, 99.0, 51.0}, 1.5));
    const Intent released = f.up(Point{220.0, 180.0});
    CHECK(released.kind == IntentKind::Resize);
    CHECK(released.id == AnnotationId{5});
    CHECK(nearRect(released.rect, Rect{100.0, 100.0, 120.0, 80.0}));
}

RIVET_TEST(handlesWorkAtEveryZoom) {
    for (const double zoom : {0.5, 1.0, 4.0}) {
        Fixture f(AnnotationTool::Select);
        f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
        f.machine.setSelection(AnnotationInteraction::Selected{0, f.hits.back()});
        // 5 logical points left of the top-left corner is within the radius.
        const Point press{100.0 - 5.0 / zoom, 100.0};
        CHECK(f.down(press, false, zoom).consumed);
        (void)f.move(Point{60.0, 80.0}, false, zoom);
        const Intent released = f.up(Point{60.0, 80.0}, false, zoom);
        CHECK(released.kind == IntentKind::Resize);
        CHECK(nearRect(released.rect, Rect{60.0, 80.0, 120.0, 60.0}));
    }
}

RIVET_TEST(resizeNeedsTheResizeCapabilityAndTheSelectionPage) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0}, true, false);
    f.machine.setSelection(AnnotationInteraction::Selected{0, f.hits.back()});
    // No handles: the press on the corner hits the annotation (a move gesture).
    CHECK(f.down(Point{180.0, 140.0}).kind == IntentKind::None); // already selected
    (void)f.move(Point{200.0, 160.0});
    CHECK(f.up(Point{200.0, 160.0}).kind == IntentKind::Move);

    // Another page: handles of the page-0 selection do not apply.
    Fixture g(AnnotationTool::Select);
    g.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    g.machine.setSelection(AnnotationInteraction::Selected{0, g.hits.back()});
    auto in = Fixture::input(Point{180.0, 140.0});
    in.page = 1;
    // A miss on another page: not a handle press; it deselects and passes on.
    const Intent other = g.machine.pointerDown(in);
    CHECK(other.kind == IntentKind::ClearSelection);
    CHECK(!other.consumed);
}

RIVET_TEST(resizePastTheOppositeEdgeFlips) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    f.machine.setSelection(AnnotationInteraction::Selected{0, f.hits.back()});
    (void)f.down(Point{180.0, 120.0}); // right-middle handle
    (void)f.move(Point{150.0, 120.0});
    const Intent released = f.up(Point{60.0, 120.0});
    CHECK(released.kind == IntentKind::Resize);
    CHECK(nearRect(released.rect, Rect{60.0, 100.0, 40.0, 40.0}));
}

RIVET_TEST(resizeCollapsedToNothingCommitsNothing) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    f.machine.setSelection(AnnotationInteraction::Selected{0, f.hits.back()});
    (void)f.down(Point{180.0, 120.0});
    (void)f.move(Point{140.0, 120.0});
    CHECK(f.up(Point{100.5, 120.0}).kind == IntentKind::None);
}

RIVET_TEST(lineEndpointHandleDragCommitsLineEndpoints) {
    Fixture f(AnnotationTool::Select);
    auto& hit = f.addHit(5, Rect{100.0, 100.0, 100.0, 50.0});
    hit.isLine = true;
    hit.lineStart = Point{100.0, 100.0};
    hit.lineEnd = Point{200.0, 150.0};
    f.machine.setSelection(AnnotationInteraction::Selected{0, hit});
    (void)f.down(Point{201.0, 149.0});
    (void)f.move(Point{230.0, 160.0});
    auto preview = f.machine.preview();
    CHECK(preview.kind == PreviewKind::Line);
    CHECK(preview.editing);
    CHECK(near(preview.a, Point{100.0, 100.0}));
    const Intent released = f.up(Point{250.0, 180.0});
    CHECK(released.kind == IntentKind::LineEndpoints);
    CHECK(near(released.a, Point{100.0, 100.0}));
    CHECK(near(released.b, Point{250.0, 180.0}));

    // Dragging the start handle keeps the end; shift snaps around the end.
    (void)f.down(Point{101.0, 99.0});
    (void)f.move(Point{50.0, 60.0}, true);
    const Intent startMoved = f.up(Point{100.0, 190.0}, true);
    CHECK(startMoved.kind == IntentKind::LineEndpoints);
    CHECK(near(startMoved.b, Point{200.0, 150.0}));
    // Snapped to a multiple of 45 degrees around the fixed end.
    const double dx = startMoved.a.x - 200.0;
    const double dy = startMoved.a.y - 150.0;
    CHECK(std::abs(dx) < 1e-6 || std::abs(dy) < 1e-6 || std::abs(std::abs(dx) - std::abs(dy)) < 1e-6);
}

// --- Keys ----------------------------------------------------------------------------------------

RIVET_TEST(escapeChainGestureThenSelectionThenToolThenPassThrough) {
    Fixture f(AnnotationTool::Rectangle);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    (void)f.down(Point{120.0, 120.0}); // selects the annotation
    (void)f.up(Point{120.0, 120.0});
    // Start a gesture on empty space and escape it.
    (void)f.down(Point{300.0, 300.0});
    (void)f.move(Point{350.0, 350.0});
    CHECK(f.machine.key(AnnotationInteraction::KeyInput::Escape).kind == IntentKind::None);
    (void)f.up(Point{350.0, 350.0});
    CHECK(f.machine.selection().has_value());
    // Next Esc clears the selection.
    const Intent clear = f.machine.key(AnnotationInteraction::KeyInput::Escape);
    CHECK(clear.kind == IntentKind::ClearSelection);
    CHECK(clear.consumed);
    CHECK(f.machine.tool() == AnnotationTool::Rectangle);
    // Next Esc returns to Select.
    const Intent tool = f.machine.key(AnnotationInteraction::KeyInput::Escape);
    CHECK(tool.kind == IntentKind::SelectTool);
    CHECK(tool.consumed);
    CHECK(f.machine.tool() == AnnotationTool::Select);
    // Then nothing is left: not consumed.
    const Intent last = f.machine.key(AnnotationInteraction::KeyInput::Escape);
    CHECK(!last.consumed);
    CHECK(last.kind == IntentKind::PassThrough);
}

RIVET_TEST(deleteAndBackspaceDeleteTheSelection) {
    for (const auto key : {AnnotationInteraction::KeyInput::Delete, AnnotationInteraction::KeyInput::Backspace}) {
        Fixture f(AnnotationTool::Select);
        // Without a selection the key is not ours.
        CHECK(!f.machine.key(key).consumed);
        f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
        (void)f.down(Point{120.0, 120.0});
        (void)f.up(Point{120.0, 120.0});
        const Intent deleted = f.machine.key(key);
        CHECK(deleted.kind == IntentKind::Delete);
        CHECK(deleted.consumed);
        CHECK(deleted.id == AnnotationId{5});
        CHECK(!f.machine.selection().has_value());
    }
}

RIVET_TEST(deleteIsIgnoredWhileAGestureRuns) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0});
    (void)f.down(Point{120.0, 120.0});
    (void)f.move(Point{150.0, 150.0});
    CHECK(!f.machine.key(AnnotationInteraction::KeyInput::Delete).consumed);
}

RIVET_TEST(aNonDeletableSelectionIsNotDeleted) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 80.0, 40.0}).canDelete = false;
    (void)f.down(Point{120.0, 120.0});
    (void)f.up(Point{120.0, 120.0});
    CHECK(!f.machine.key(AnnotationInteraction::KeyInput::Delete).consumed);
}

RIVET_TEST(doubleClickOnAnEditableAnnotationOpensTheNoteEditor) {
    Fixture f(AnnotationTool::Select);
    f.addHit(5, Rect{100.0, 100.0, 20.0, 20.0}).canEditContents = true;
    CHECK(f.down(Point{110.0, 110.0}).kind == IntentKind::Select);
    (void)f.up(Point{110.0, 110.0});
    const Intent second = f.down(Point{110.0, 110.0}, false, 1.0, 2);
    CHECK(second.kind == IntentKind::OpenNoteEditor);
    CHECK(second.id == AnnotationId{5});
    CHECK(second.consumed);
    CHECK(f.up(Point{110.0, 110.0}).kind == IntentKind::None);
    // A double click on something without editable contents just selects.
    Fixture g(AnnotationTool::Select);
    g.addHit(6, Rect{100.0, 100.0, 20.0, 20.0});
    CHECK(g.down(Point{110.0, 110.0}, false, 1.0, 2).kind == IntentKind::Select);
}

RIVET_TEST(enterOpensTheEditorForAnEditableSelection) {
    Fixture f(AnnotationTool::Select);
    CHECK(!f.machine.key(AnnotationInteraction::KeyInput::Enter).consumed);
    f.addHit(5, Rect{100.0, 100.0, 20.0, 20.0}).canEditContents = true;
    (void)f.down(Point{110.0, 110.0});
    (void)f.up(Point{110.0, 110.0});
    const Intent enter = f.machine.key(AnnotationInteraction::KeyInput::Enter);
    CHECK(enter.kind == IntentKind::OpenNoteEditor);
    CHECK(enter.id == AnnotationId{5});
}

RIVET_TEST(changingTheToolCancelsTheGesture) {
    Fixture f(AnnotationTool::Rectangle);
    (void)f.down(Point{100.0, 100.0});
    (void)f.move(Point{200.0, 200.0});
    f.machine.setTool(AnnotationTool::Ellipse);
    CHECK(!f.machine.gestureActive());
    CHECK(f.machine.preview().kind == PreviewKind::None);
    CHECK(!f.up(Point{200.0, 200.0}).consumed);
}

RIVET_TEST(topmostHitWinsAndPointerMovesWithoutAPressPassThrough) {
    Fixture f(AnnotationTool::Select);
    f.addHit(1, Rect{100.0, 100.0, 100.0, 100.0});
    f.addHit(2, Rect{150.0, 150.0, 100.0, 100.0});
    CHECK(f.down(Point{160.0, 160.0}).id == AnnotationId{2});
    (void)f.up(Point{160.0, 160.0});
    CHECK(!f.move(Point{500.0, 500.0}).consumed);
}
