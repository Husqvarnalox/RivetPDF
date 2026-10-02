// SPDX-License-Identifier: MPL-2.0
// ContentInteraction: the pure state machine of the content tools. No widgets,
// editor or platform: pointer/key inputs in page display points go in, intents
// and previews come out.
#include "RivetTest.h"

#include "app/ContentInteraction.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

using rivet::app::ContentInteraction;
using rivet::app::ContentTool;
using rivet::core::ObjectId;
using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::editor::ContentCapability;
using Handle = ContentInteraction::Handle;
using HitInfo = ContentInteraction::HitInfo;
using Intent = ContentInteraction::Intent;
using IntentKind = ContentInteraction::Intent::Kind;
using KeyInput = ContentInteraction::KeyInput;
using Kind = ContentInteraction::Kind;

namespace {

constexpr Size kPage{600.0, 800.0};

std::array<Point, 4> quadOf(const Rect& r) {
    return {Point{r.minX(), r.maxY()}, Point{r.maxX(), r.maxY()}, Point{r.maxX(), r.minY()},
            Point{r.minX(), r.minY()}};
}

struct Fixture {
    ContentInteraction machine;
    std::vector<HitInfo> hits; // z-order, last = topmost
    int hitCalls = 0;
    double lastTolerance = 0.0;

    explicit Fixture(ContentTool tool = ContentTool::SelectObject) {
        machine.setHitTest([this](std::size_t page, Point point, double tolerance) {
            ++hitCalls;
            lastTolerance = tolerance;
            std::optional<HitInfo> found;
            if (page != 0) return found;
            for (const HitInfo& hit : hits) {
                const Rect grown{hit.bounds.minX() - tolerance, hit.bounds.minY() - tolerance,
                                 hit.bounds.size.width + 2.0 * tolerance, hit.bounds.size.height + 2.0 * tolerance};
                if (grown.contains(point)) found = hit;
            }
            return found;
        });
        machine.setTool(tool);
    }

    HitInfo& addImage(std::uint64_t id, Rect bounds, bool movable = true, Kind kind = Kind::Image) {
        HitInfo info;
        info.id = ObjectId{id};
        info.kind = kind;
        info.bounds = bounds;
        info.quad = quadOf(bounds);
        info.canMove = movable;
        info.canDelete = movable;
        info.canResize = movable;
        info.capability = movable ? ContentCapability::MoveOnly : ContentCapability::ReadOnly;
        hits.push_back(info);
        return hits.back();
    }
    HitInfo& addText(std::uint64_t id, Rect bounds, ContentCapability capability = ContentCapability::FullyEditable) {
        HitInfo info;
        info.id = ObjectId{id};
        info.isBlock = true;
        info.kind = Kind::Text;
        info.bounds = bounds;
        info.quad = quadOf(bounds);
        info.capability = capability;
        info.canMove = capability != ContentCapability::ReadOnly;
        info.canDelete = info.canMove;
        info.canEditText = capability == ContentCapability::Replaceable || capability == ContentCapability::FullyEditable;
        info.canWrap = info.canEditText;
        info.wrapBase = bounds.size.width;
        hits.push_back(info);
        return hits.back();
    }

    static ContentInteraction::PointerInput input(Point point, bool shift = false, double zoom = 1.0, int clicks = 1) {
        ContentInteraction::PointerInput in;
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
    Intent move(Point p, bool shift = false, double zoom = 1.0) { return machine.pointerMove(input(p, shift, zoom)); }
    Intent up(Point p, bool shift = false, double zoom = 1.0) { return machine.pointerUp(input(p, shift, zoom)); }
    Intent drag(Point from, Point to, bool shift = false) {
        (void)down(from, shift);
        (void)move(to, shift);
        return up(to, shift);
    }
    void select(const HitInfo& info) { machine.setSelection(ContentInteraction::Selected{0, info}); }
};

const Rect kImage{100.0, 100.0, 200.0, 100.0};

} // namespace

RIVET_TEST(contentHoverTracksTheObjectAtToleranceOverZoom) {
    Fixture f;
    f.addImage(1, kImage);
    const Intent miss = f.move(Point{10.0, 10.0});
    CHECK(!miss.consumed);
    CHECK(!f.machine.hover().has_value());
    (void)f.move(Point{96.0, 150.0}, false, 1.0); // 4 pt left of the image: inside the tolerance
    CHECK(f.machine.hover().has_value());
    CHECK_NEAR(f.lastTolerance, ContentInteraction::kHitTolerance, 1e-9);
    (void)f.move(Point{96.0, 150.0}, false, 2.0); // tolerance is 4 / zoom = 2 pt: now a miss
    CHECK_NEAR(f.lastTolerance, ContentInteraction::kHitTolerance / 2.0, 1e-9);
    CHECK(!f.machine.hover().has_value());
    // AddText does not hover.
    f.machine.setTool(ContentTool::AddText);
    (void)f.move(Point{150.0, 150.0});
    CHECK(!f.machine.hover().has_value());
}

RIVET_TEST(contentClickSelectsTheTopmostAndAMissClearsTheSelection) {
    Fixture f;
    f.addImage(1, kImage);
    f.addImage(2, Rect{150.0, 120.0, 50.0, 50.0});
    Intent intent = f.down(Point{160.0, 130.0});
    CHECK(intent.kind == IntentKind::Select);
    CHECK(intent.consumed);
    CHECK(intent.info.id == ObjectId{2});
    CHECK(f.machine.selection().has_value());
    (void)f.up(Point{160.0, 130.0});

    intent = f.down(Point{500.0, 700.0});
    CHECK(intent.kind == IntentKind::ClearSelection);
    CHECK(intent.consumed);
    CHECK(!f.machine.selection().has_value());
    // The consumed miss swallows the release.
    CHECK(f.up(Point{500.0, 700.0}).kind == IntentKind::None);
}

RIVET_TEST(contentToolNoneAndHeldReleaseArePassThrough) {
    Fixture f(ContentTool::None);
    f.addImage(1, kImage);
    const Intent intent = f.down(Point{150.0, 150.0});
    CHECK(intent.kind == IntentKind::PassThrough);
    CHECK(!intent.consumed);
    CHECK(f.up(Point{150.0, 150.0}).kind == IntentKind::PassThrough);
}

RIVET_TEST(contentDragEmitsExactlyOneMoveOnReleaseAndBelowThresholdNone) {
    Fixture f;
    f.addImage(1, kImage);
    (void)f.down(Point{150.0, 150.0});
    const Intent mid = f.move(Point{160.0, 155.0});
    CHECK(mid.kind == IntentKind::None); // moves never emit
    CHECK(f.machine.preview().kind == ContentInteraction::Preview::Kind::Move);
    const Intent release = f.up(Point{170.0, 160.0});
    CHECK(release.kind == IntentKind::Move);
    CHECK_NEAR(release.delta.x, 20.0, 1e-9);
    CHECK_NEAR(release.delta.y, 10.0, 1e-9);
    CHECK(release.info.id == ObjectId{1});
    CHECK(f.up(Point{170.0, 160.0}).kind == IntentKind::PassThrough); // gesture is over

    // Below the threshold a press only selects.
    (void)f.down(Point{150.0, 150.0});
    CHECK(f.up(Point{151.0, 150.0}).kind == IntentKind::None);
}

RIVET_TEST(contentReadOnlyObjectSelectsButDoesNotMove) {
    Fixture f;
    f.addImage(1, kImage, false);
    const Intent select = f.down(Point{150.0, 150.0});
    CHECK(select.kind == IntentKind::Select);
    CHECK(f.machine.selection().has_value());
    (void)f.move(Point{200.0, 200.0});
    CHECK(f.up(Point{200.0, 200.0}).kind == IntentKind::None);
}

RIVET_TEST(contentImageCornerResizeKeepsAspectUnlessShift) {
    Fixture f;
    const HitInfo image = f.addImage(1, kImage);
    f.select(image);
    // Bottom-right corner (300, 200) dragged to (400, 220): aspect wins the larger scale (1.5).
    Intent intent = f.drag(Point{300.0, 200.0}, Point{400.0, 220.0});
    CHECK(intent.kind == IntentKind::Resize);
    CHECK_NEAR(intent.rect.size.width, 300.0, 1e-9);
    CHECK_NEAR(intent.rect.size.height, 150.0, 1e-9);
    CHECK_NEAR(intent.rect.minX(), 100.0, 1e-9);
    CHECK_NEAR(intent.rect.minY(), 100.0, 1e-9);
    // Shift frees the ratio for images.
    f.select(image);
    intent = f.drag(Point{300.0, 200.0}, Point{400.0, 220.0}, true);
    CHECK(intent.kind == IntentKind::Resize);
    CHECK_NEAR(intent.rect.size.width, 300.0, 1e-9);
    CHECK_NEAR(intent.rect.size.height, 120.0, 1e-9);
    // Edge handles resize along one axis.
    f.select(image);
    intent = f.drag(Point{300.0, 150.0}, Point{350.0, 170.0});
    CHECK(intent.kind == IntentKind::Resize);
    CHECK_NEAR(intent.rect.size.width, 250.0, 1e-9);
    CHECK_NEAR(intent.rect.size.height, 100.0, 1e-9);
}

RIVET_TEST(contentPathCornerResizeIsFreeUnlessShift) {
    Fixture f;
    const HitInfo path = f.addImage(1, kImage, true, Kind::Path);
    f.select(path);
    Intent intent = f.drag(Point{300.0, 200.0}, Point{400.0, 220.0});
    CHECK(intent.kind == IntentKind::Resize);
    CHECK_NEAR(intent.rect.size.width, 300.0, 1e-9);
    CHECK_NEAR(intent.rect.size.height, 120.0, 1e-9);
    f.select(path);
    intent = f.drag(Point{300.0, 200.0}, Point{400.0, 220.0}, true);
    CHECK(intent.kind == IntentKind::Resize);
    CHECK_NEAR(intent.rect.size.height, 150.0, 1e-9);
}

RIVET_TEST(contentResizeTooSmallIsDropped) {
    Fixture f;
    const HitInfo image = f.addImage(1, kImage);
    f.select(image);
    // Dragging the bottom-right corner onto the top-left one collapses the box.
    CHECK(f.drag(Point{300.0, 200.0}, Point{101.0, 101.0}).kind == IntentKind::None);
}

RIVET_TEST(contentImagesAndPathsHaveEightHandles) {
    for (const Kind kind : {Kind::Image, Kind::Path}) {
        Fixture f;
        const HitInfo info = f.addImage(1, kImage, true, kind);
        for (const Handle handle : {Handle::TopLeft, Handle::Top, Handle::TopRight, Handle::Right,
                                    Handle::BottomRight, Handle::Bottom, Handle::BottomLeft, Handle::Left}) {
            f.select(info);
            const Point at = rivet::app::AnnotationInteraction::handlePoint(info.bounds, handle);
            const Intent intent = f.down(at);
            // A handle press starts a resize gesture: no Select intent.
            CHECK(intent.kind == IntentKind::None);
            CHECK(f.machine.gestureActive());
            f.machine.cancelGesture();
        }
    }
}

RIVET_TEST(contentTextBlockHasOneWrapHandleAndNeverResizes) {
    Fixture f;
    const HitInfo text = f.addText(7, Rect{50.0, 50.0, 200.0, 20.0});
    f.select(text);
    const Point handle = ContentInteraction::wrapHandlePoint(text);
    CHECK_NEAR(handle.x, 250.0, 1e-9);
    CHECK_NEAR(handle.y, 60.0, 1e-9);
    Intent intent = f.drag(handle, Point{290.0, 60.0});
    CHECK(intent.kind == IntentKind::SetWrap);
    CHECK_NEAR(intent.width, 240.0, 1e-9);
    CHECK(intent.info.id == ObjectId{7});

    // The corner of a text block is not a resize handle: pressing there selects (inside the frame).
    f.select(text);
    intent = f.down(Point{50.0, 70.0});
    CHECK(intent.kind == IntentKind::Select);
    (void)f.up(Point{50.0, 70.0});

    // Too small a wrap change is dropped; the width never goes below the minimum.
    f.select(text);
    CHECK(f.drag(handle, Point{250.2, 60.0}).kind == IntentKind::None);
    f.select(text);
    intent = f.drag(handle, Point{52.0, 60.0});
    CHECK(intent.kind == IntentKind::SetWrap);
    CHECK_NEAR(intent.width, ContentInteraction::kMinWrapWidth, 1e-9);
}

RIVET_TEST(contentDoubleClickOpensTheEditorOnlyForRetypableBlocks) {
    Fixture f;
    f.addText(1, Rect{50.0, 50.0, 200.0, 20.0});
    f.addText(2, Rect{50.0, 100.0, 200.0, 20.0}, ContentCapability::MoveOnly);
    Intent intent = f.down(Point{100.0, 60.0}, false, 1.0, 2);
    CHECK(intent.kind == IntentKind::OpenEditor);
    CHECK(intent.info.id == ObjectId{1});
    (void)f.up(Point{100.0, 60.0});
    intent = f.down(Point{100.0, 110.0}, false, 1.0, 2);
    CHECK(intent.kind == IntentKind::Select);
}

RIVET_TEST(contentKeysEnterDeleteArrowsAndEscape) {
    Fixture f;
    const HitInfo editable = f.addText(1, Rect{50.0, 50.0, 200.0, 20.0});
    const HitInfo moveOnly = f.addText(2, Rect{50.0, 100.0, 200.0, 20.0}, ContentCapability::MoveOnly);

    // Nothing selected.
    CHECK(f.machine.key(KeyInput::Delete).kind == IntentKind::PassThrough);
    CHECK(!f.machine.key(KeyInput::Enter).consumed);
    CHECK(f.machine.key(KeyInput::Escape).kind == IntentKind::PassThrough);

    f.select(editable);
    CHECK(f.machine.key(KeyInput::Enter).kind == IntentKind::OpenEditor);
    CHECK(f.machine.key(KeyInput::Delete).kind == IntentKind::Delete);
    CHECK(f.machine.key(KeyInput::Backspace).kind == IntentKind::Delete);
    for (const KeyInput arrow : {KeyInput::Left, KeyInput::Right, KeyInput::Up, KeyInput::Down}) {
        const Intent nudge = f.machine.key(arrow);
        CHECK(nudge.kind == IntentKind::Nudge);
        CHECK(nudge.key == arrow);
        CHECK(nudge.info.id == ObjectId{1});
    }

    f.select(moveOnly);
    CHECK(f.machine.key(KeyInput::Enter).kind == IntentKind::PassThrough);
    CHECK(f.machine.key(KeyInput::Delete).kind == IntentKind::Delete);

    // Esc: clears the selection, then passes through.
    const Intent clear = f.machine.key(KeyInput::Escape);
    CHECK(clear.kind == IntentKind::ClearSelection);
    CHECK(f.machine.key(KeyInput::Escape).kind == IntentKind::PassThrough);
}

RIVET_TEST(contentEscapeCancelsARunningGestureBeforeClearingTheSelection) {
    Fixture f;
    f.addImage(1, kImage);
    (void)f.down(Point{150.0, 150.0});
    (void)f.move(Point{200.0, 200.0});
    CHECK(f.machine.gestureInProgress());
    const Intent esc = f.machine.key(KeyInput::Escape);
    CHECK(esc.kind == IntentKind::None);
    CHECK(esc.consumed);
    CHECK(!f.machine.gestureActive());
    CHECK(f.machine.selection().has_value()); // the selection survives the first Esc
    CHECK(f.machine.key(KeyInput::Escape).kind == IntentKind::ClearSelection);
}

RIVET_TEST(contentAddTextClickDragAndThinDrag) {
    Fixture f(ContentTool::AddText);
    f.addImage(1, kImage); // AddText ignores objects
    (void)f.down(Point{150.0, 150.0});
    CHECK(f.machine.gestureInProgress());
    Intent intent = f.up(Point{150.0, 150.0});
    CHECK(intent.kind == IntentKind::AddTextClick);
    CHECK_NEAR(intent.point.x, 150.0, 1e-9);
    CHECK_NEAR(intent.point.y, 150.0, 1e-9);
    CHECK_EQ(f.hitCalls, 0);

    intent = f.drag(Point{100.0, 300.0}, Point{200.0, 340.0});
    CHECK(intent.kind == IntentKind::AddTextBox);
    CHECK_NEAR(intent.rect.minX(), 100.0, 1e-9);
    CHECK_NEAR(intent.rect.minY(), 300.0, 1e-9);
    CHECK_NEAR(intent.rect.size.width, 100.0, 1e-9);
    CHECK_NEAR(intent.rect.size.height, 40.0, 1e-9);

    // Dragged past the threshold but too thin for a box: a click at the start.
    intent = f.drag(Point{100.0, 300.0}, Point{104.0, 400.0});
    CHECK(intent.kind == IntentKind::AddTextClick);
    CHECK_NEAR(intent.point.x, 100.0, 1e-9);
    CHECK_NEAR(intent.point.y, 300.0, 1e-9);
}

RIVET_TEST(contentSetToolResetsGestureAndHover) {
    Fixture f;
    f.addImage(1, kImage);
    (void)f.move(Point{150.0, 150.0});
    CHECK(f.machine.hover().has_value());
    f.machine.setTool(ContentTool::SelectObject);
    CHECK(!f.machine.hover().has_value());
    (void)f.down(Point{150.0, 150.0});
    (void)f.move(Point{200.0, 200.0});
    CHECK(f.machine.gestureActive());
    f.machine.setTool(ContentTool::AddText);
    CHECK(!f.machine.gestureActive());
    CHECK(f.machine.tool() == ContentTool::AddText);
}
