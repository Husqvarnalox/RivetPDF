// SPDX-License-Identifier: MPL-2.0
// AnnotationPainter (recording paint context) and AnnotationLayer (stub
// host + stub client): coordinate mapping, chrome, previews, event routing.
#include "RivetTest.h"

#include "Fakes.hpp"
#include "app/AnnotationLayer.hpp"
#include "app/AnnotationPainter.hpp"

#include <cmath>
#include <utility>
#include <vector>

using namespace rivet;
using app::AnnotationInteraction;
using app::AnnotationLayer;
using app::AnnotationLayerClient;
using app::AnnotationPainter;
using app::AnnotationTool;
using Intent = AnnotationInteraction::Intent;
using IntentKind = AnnotationInteraction::Intent::Kind;
using PreviewKind = AnnotationInteraction::Preview::Kind;
using ui::testing::FakePaintContext;

namespace {

constexpr core::Rect kPageRect{100.0, 50.0, 300.0, 400.0}; // page 150x200 at zoom 2
constexpr double kZoom = 2.0;

editor::DisplayPathSegment seg(pdf::PdfPathSegment::Op op, core::Point p) {
    editor::DisplayPathSegment s;
    s.op = op;
    s.p = p;
    return s;
}

editor::AnnotationView squareView(bool overlay = true) {
    editor::AnnotationView view;
    view.id = core::AnnotationId{1};
    view.kind = pdf::PdfAnnotationKind::Square;
    view.drawnByOverlay = overlay;
    editor::DisplayAppearance appearance;
    appearance.opacity = 0.5F;
    editor::DisplayPath path;
    using Op = pdf::PdfPathSegment::Op;
    path.segments = {seg(Op::MoveTo, {10.0, 20.0}), seg(Op::LineTo, {60.0, 20.0}), seg(Op::LineTo, {60.0, 70.0}),
                     seg(Op::Close, {})};
    path.fill = pdf::PdfColor{1.0F, 0.0F, 0.0F};
    path.stroke = pdf::PdfColor{0.0F, 0.0F, 1.0F};
    path.strokeWidth = 3.0F;
    path.roundJoins = true;
    appearance.paths.push_back(path);
    editor::DisplayText text;
    text.text = "APPROVED";
    text.box = core::Rect{10.0, 20.0, 50.0, 10.0};
    text.rotation = 270;
    text.color = pdf::PdfColor{0.0F, 1.0F, 0.0F};
    appearance.texts.push_back(text);
    if (overlay) view.appearance = appearance;
    return view;
}

AnnotationInteraction::Selected selected(bool resize, bool line = false) {
    AnnotationInteraction::Selected s;
    s.page = 0;
    s.info.id = core::AnnotationId{1};
    s.info.canResize = resize;
    s.info.bounds = core::Rect{10.0, 20.0, 50.0, 40.0};
    s.info.isLine = line;
    s.info.lineStart = core::Point{10.0, 20.0};
    s.info.lineEnd = core::Point{60.0, 60.0};
    return s;
}

bool near(double a, double b, double eps = 1e-6) { return std::abs(a - b) <= eps; }

} // namespace

// --- Painter ----------------------------------------------------------------------

RIVET_TEST(painterMapsAppearancePathsAndTextsIntoTheViewport) {
    FakePaintContext ctx;
    AnnotationPainter::paintAnnotation(ctx, squareView(), kPageRect, kZoom);
    CHECK_EQ(ctx.paths.size(), 2u); // fill, then stroke
    const auto& fill = ctx.paths[0];
    CHECK(!fill.stroke);
    CHECK_NEAR(fill.color.a, 0.5, 1e-9); // appearance opacity multiplies alpha
    CHECK_NEAR(fill.color.r, 1.0, 1e-6);
    CHECK_NEAR(fill.path.segments[0].p.x, 100.0 + 10.0 * 2.0, 1e-9);
    CHECK_NEAR(fill.path.segments[0].p.y, 50.0 + 20.0 * 2.0, 1e-9);
    CHECK_NEAR(fill.path.segments[1].p.x, 100.0 + 60.0 * 2.0, 1e-9);
    const auto& stroke = ctx.paths[1];
    CHECK(stroke.stroke);
    CHECK_NEAR(stroke.strokeWidth, 6.0, 1e-9); // 3 pt * zoom
    CHECK(stroke.join == ui::LineJoin::Round);
    CHECK_NEAR(stroke.color.b, 1.0, 1e-6);
    CHECK_EQ(ctx.boxTexts.size(), 1u);
    const auto& text = ctx.boxTexts[0];
    CHECK(text.text == "APPROVED");
    CHECK_EQ(text.quarterTurns, 3);
    CHECK_NEAR(text.box.origin.x, 120.0, 1e-9);
    CHECK_NEAR(text.box.origin.y, 90.0, 1e-9);
    CHECK_NEAR(text.box.size.width, 100.0, 1e-9);
    CHECK_NEAR(text.color.a, 0.5, 1e-9);
}

RIVET_TEST(painterSkipsRasterDrawnAnnotationsAndViewsWithoutAppearance) {
    FakePaintContext ctx;
    AnnotationPainter::paintAnnotation(ctx, squareView(false), kPageRect, kZoom);
    editor::AnnotationView bare = squareView();
    bare.appearance.reset();
    AnnotationPainter::paintAnnotation(ctx, bare, kPageRect, kZoom);
    CHECK(ctx.paths.empty());
    CHECK(ctx.boxTexts.empty());
}

RIVET_TEST(painterPaintsOnlyTheStrokeWhenThereIsNoFill) {
    editor::AnnotationView view = squareView();
    view.appearance->paths[0].fill.reset();
    FakePaintContext ctx;
    AnnotationPainter::paintAnnotation(ctx, view, kPageRect, kZoom);
    CHECK_EQ(ctx.paths.size(), 1u);
    CHECK(ctx.paths[0].stroke);
}

RIVET_TEST(painterSelectionDrawsOutlineAndEightFixedSizeHandles) {
    for (const double zoom : {0.5, 2.0, 4.0}) {
        FakePaintContext ctx;
        AnnotationPainter::paintSelection(ctx, selected(true), kPageRect, zoom);
        CHECK_EQ(ctx.dashedStrokes.size(), 1u);
        CHECK_NEAR(ctx.dashedStrokes[0].rect.size.width, 50.0 * zoom, 1e-9);
        CHECK_EQ(ctx.fills.size(), 8u);
        CHECK_EQ(ctx.strokes.size(), 8u);
        for (const auto& handle : ctx.fills) {
            CHECK_NEAR(handle.rect.size.width, AnnotationInteraction::kHandleSize, 1e-9);
            CHECK_NEAR(handle.rect.size.height, AnnotationInteraction::kHandleSize, 1e-9);
        }
        // The top-left handle is centered on the top-left corner.
        const core::Rect first = ctx.fills[0].rect;
        CHECK_NEAR(first.center().x, 100.0 + 10.0 * zoom, 1e-9);
        CHECK_NEAR(first.center().y, 50.0 + 20.0 * zoom, 1e-9);
    }
}

RIVET_TEST(painterSelectionWithoutResizeCapabilityHasNoHandles) {
    FakePaintContext ctx;
    AnnotationPainter::paintSelection(ctx, selected(false), kPageRect, kZoom);
    CHECK_EQ(ctx.dashedStrokes.size(), 1u);
    CHECK(ctx.fills.empty());
}

RIVET_TEST(painterSelectionOfALineHasTwoEndpointHandles) {
    FakePaintContext ctx;
    AnnotationPainter::paintSelection(ctx, selected(true, true), kPageRect, kZoom);
    CHECK_EQ(ctx.fills.size(), 2u);
    CHECK_NEAR(ctx.fills[1].rect.center().x, 100.0 + 60.0 * 2.0, 1e-9);
    CHECK_NEAR(ctx.fills[1].rect.center().y, 50.0 + 60.0 * 2.0, 1e-9);
}

RIVET_TEST(painterPreviewDrawsEachCreationKind) {
    editor::AnnotationStyle style;
    style.color = pdf::PdfColor{1.0F, 0.0F, 0.0F};
    style.opacity = 0.5F;
    style.borderWidth = 2.0F;

    AnnotationInteraction::Preview ink;
    ink.kind = PreviewKind::Ink;
    ink.points = {core::Point{0.0, 0.0}, core::Point{10.0, 10.0}, core::Point{20.0, 0.0}};
    FakePaintContext inkCtx;
    AnnotationPainter::paintPreview(inkCtx, ink, style, kPageRect, kZoom);
    CHECK_EQ(inkCtx.paths.size(), 1u);
    CHECK(inkCtx.paths[0].stroke);
    CHECK_NEAR(inkCtx.paths[0].strokeWidth, 4.0, 1e-9);
    CHECK_NEAR(inkCtx.paths[0].color.a, 0.5, 1e-9);
    CHECK_EQ(inkCtx.paths[0].path.segments.size(), 3u);

    AnnotationInteraction::Preview rect;
    rect.kind = PreviewKind::Rect;
    rect.tool = AnnotationTool::Rectangle;
    rect.rect = core::Rect{10.0, 10.0, 40.0, 30.0};
    FakePaintContext rectCtx;
    AnnotationPainter::paintPreview(rectCtx, rect, style, kPageRect, kZoom);
    CHECK_EQ(rectCtx.paths.size(), 1u);
    CHECK_EQ(rectCtx.paths[0].countOf(ui::PathSegment::Op::LineTo), 3u);

    rect.tool = AnnotationTool::Ellipse;
    style.interiorColor = pdf::PdfColor{0.0F, 1.0F, 0.0F};
    FakePaintContext ellipseCtx;
    AnnotationPainter::paintPreview(ellipseCtx, rect, style, kPageRect, kZoom);
    CHECK_EQ(ellipseCtx.paths.size(), 2u); // fill + stroke
    CHECK_EQ(ellipseCtx.paths[0].countOf(ui::PathSegment::Op::CubicTo), 4u);

    rect.tool = AnnotationTool::Stamp;
    FakePaintContext stampCtx;
    AnnotationPainter::paintPreview(stampCtx, rect, style, kPageRect, kZoom);
    CHECK_EQ(stampCtx.dashedStrokes.size(), 1u);
    CHECK(stampCtx.paths.empty());

    AnnotationInteraction::Preview line;
    line.kind = PreviewKind::Line;
    line.tool = AnnotationTool::Line;
    line.a = core::Point{10.0, 10.0};
    line.b = core::Point{50.0, 10.0};
    FakePaintContext lineCtx;
    AnnotationPainter::paintPreview(lineCtx, line, style, kPageRect, kZoom);
    CHECK_EQ(lineCtx.lines.size(), 1u);
    CHECK(lineCtx.paths.empty());
    CHECK_NEAR(lineCtx.lines[0].to.x, 100.0 + 50.0 * 2.0, 1e-9);
    line.tool = AnnotationTool::Arrow;
    FakePaintContext arrowCtx;
    AnnotationPainter::paintPreview(arrowCtx, line, style, kPageRect, kZoom);
    CHECK_EQ(arrowCtx.lines.size(), 1u);
    CHECK_EQ(arrowCtx.paths.size(), 1u); // the head
    CHECK(!arrowCtx.paths[0].stroke);
}

RIVET_TEST(painterEditPreviewsAreOutlinesOnly) {
    editor::AnnotationStyle style;
    AnnotationInteraction::Preview bounds;
    bounds.kind = PreviewKind::Bounds;
    bounds.editing = true;
    bounds.rect = core::Rect{10.0, 10.0, 40.0, 30.0};
    FakePaintContext ctx;
    AnnotationPainter::paintPreview(ctx, bounds, style, kPageRect, kZoom);
    CHECK_EQ(ctx.dashedStrokes.size(), 1u);
    CHECK(ctx.paths.empty());

    AnnotationInteraction::Preview line;
    line.kind = PreviewKind::Line;
    line.editing = true;
    FakePaintContext lineCtx;
    AnnotationPainter::paintPreview(lineCtx, line, style, kPageRect, kZoom);
    CHECK_EQ(lineCtx.lines.size(), 1u);

    FakePaintContext none;
    AnnotationPainter::paintPreview(none, AnnotationInteraction::Preview{}, style, kPageRect, kZoom);
    CHECK(none.paths.empty() && none.lines.empty() && none.dashedStrokes.empty());
}

// --- Layer --------------------------------------------------------------------------

namespace {

class StubHost final : public ui::ViewportToolHost {
public:
    core::Rect page0{100.0, 50.0, 300.0, 400.0};
    double zoom = 2.0;
    int repaints = 0;

    std::optional<core::Rect> pageRectInViewport(std::size_t pageIndex) const override {
        if (pageIndex == 0) return page0;
        return std::nullopt;
    }
    double zoomFactor() const override { return zoom; }
    core::Rect viewportBounds() const override { return core::Rect{0.0, 0.0, 800.0, 600.0}; }
    void requestRepaint() override { ++repaints; }
    std::size_t pageCount() const override { return 1; }
    std::optional<std::pair<std::size_t, core::Point>> pageAt(core::Point p) const override {
        if (!page0.contains(p)) return std::nullopt;
        return std::pair<std::size_t, core::Point>{
            0, core::Point{(p.x - page0.origin.x) / zoom, (p.y - page0.origin.y) / zoom}};
    }
};

class StubClient final : public AnnotationLayerClient {
public:
    AnnotationInteraction machine;
    std::optional<AnnotationInteraction::Selected> selection;
    std::shared_ptr<const std::vector<editor::AnnotationView>> annotations;
    bool textSelection = false;
    int pressed = 0;
    std::vector<Intent> intents;

    AnnotationInteraction& interaction() override { return machine; }
    const AnnotationInteraction& interaction() const override { return machine; }
    std::optional<AnnotationInteraction::Selected> currentSelection() const override { return selection; }
    std::shared_ptr<const std::vector<editor::AnnotationView>> pageAnnotations(std::size_t page) const override {
        return page == 0 ? annotations : nullptr;
    }
    editor::AnnotationStyle previewStyle() const override { return {}; }
    bool textSelectionNonEmpty() const override { return textSelection; }
    void pointerPressed() override { ++pressed; }
    void applyIntent(const Intent& intent) override {
        if (intent.kind == IntentKind::Select) {
            selection = AnnotationInteraction::Selected{intent.page, hit};
        }
        if (intent.kind == IntentKind::ClearSelection) selection.reset();
        intents.push_back(intent);
    }
    AnnotationInteraction::HitInfo hit;
};

ui::PointerEvent pointer(ui::PointerEventType type, core::Point position, bool shift = false) {
    ui::PointerEvent event;
    event.type = type;
    event.position = position;
    event.button = 1;
    event.modifiers.shift = shift;
    return event;
}

struct LayerFixture {
    StubHost host;
    StubClient client;
    double now = 0.0;
    AnnotationLayer layer;
    LayerFixture() : layer(client, [this] { return now; }) {}
};

} // namespace

RIVET_TEST(layerNoteClickMapsViewportPointToPageDisplaySpace) {
    LayerFixture f;
    f.client.machine.setTool(AnnotationTool::Note);
    // viewport (200, 150) -> page display ((200-100)/2, (150-50)/2) = (50, 50)
    CHECK(f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, {200.0, 150.0})));
    CHECK(f.layer.onMouse(f.host, pointer(ui::PointerEventType::Up, {200.0, 150.0})));
    CHECK_EQ(f.client.pressed, 1);
    const Intent& created = f.client.intents.back();
    CHECK(created.kind == IntentKind::CreateNote);
    CHECK_NEAR(created.rect.origin.x, 50.0, 1e-9);
    CHECK_NEAR(created.rect.origin.y, 50.0, 1e-9);
}

RIVET_TEST(layerPassesThroughMissesUnderSelectAndTheirMovesAndRelease) {
    LayerFixture f;
    CHECK(!f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, {200.0, 150.0})));
    CHECK(!f.layer.onMouse(f.host, pointer(ui::PointerEventType::Move, {210.0, 150.0})));
    CHECK(!f.layer.onMouse(f.host, pointer(ui::PointerEventType::Up, {210.0, 150.0})));
    CHECK(f.client.intents.back().kind == IntentKind::PassThrough);
}

RIVET_TEST(layerIgnoresNonPrimaryAndModifiedPresses) {
    LayerFixture f;
    f.client.machine.setTool(AnnotationTool::Rectangle);
    ui::PointerEvent right = pointer(ui::PointerEventType::Down, {200.0, 150.0});
    right.button = 2;
    CHECK(!f.layer.onMouse(f.host, right));
    ui::PointerEvent command = pointer(ui::PointerEventType::Down, {200.0, 150.0});
    command.modifiers.command = true;
    CHECK(!f.layer.onMouse(f.host, command));
    CHECK_EQ(f.client.pressed, 0);
    CHECK(f.client.intents.empty());
}

RIVET_TEST(layerKeepsTrackingADragOutsideThePageAndClampsIt) {
    LayerFixture f;
    f.client.machine.setTool(AnnotationTool::Rectangle);
    CHECK(f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, {200.0, 150.0})));
    // The pointer leaves the page (and the host's pageAt would say nothing).
    CHECK(f.layer.onMouse(f.host, pointer(ui::PointerEventType::Move, {700.0, 560.0})));
    CHECK(f.layer.onMouse(f.host, pointer(ui::PointerEventType::Up, {700.0, 560.0})));
    const Intent& created = f.client.intents.back();
    CHECK(created.kind == IntentKind::CreateShape);
    CHECK_NEAR(created.rect.maxX(), 150.0, 1e-9); // page width in display points
    CHECK_NEAR(created.rect.maxY(), 200.0, 1e-9);
    CHECK_GT(f.host.repaints, 0);
}

RIVET_TEST(layerShiftConstrainsTheDrag) {
    LayerFixture f;
    f.client.machine.setTool(AnnotationTool::Rectangle);
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, {200.0, 150.0}, true));
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Move, {300.0, 200.0}, true));
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Up, {300.0, 200.0}, true));
    const Intent& created = f.client.intents.back();
    CHECK(near(created.rect.size.width, created.rect.size.height));
}

RIVET_TEST(layerDetectsDoubleClicksByTimeAndDistance) {
    LayerFixture f;
    AnnotationInteraction::HitInfo hit;
    hit.id = core::AnnotationId{4};
    hit.bounds = core::Rect{40.0, 40.0, 20.0, 20.0};
    hit.canEditContents = true;
    f.client.hit = hit;
    f.client.machine.setHitTest([&](std::size_t, core::Point p, double tol) -> std::optional<AnnotationInteraction::HitInfo> {
        if (hit.bounds.contains(p) || (std::abs(p.x - 50.0) <= tol && std::abs(p.y - 50.0) <= tol)) return hit;
        return std::nullopt;
    });
    const core::Point where{200.0, 150.0}; // page (50, 50)
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, where));
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Up, where));
    f.now = 0.2;
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, {201.0, 150.0}));
    CHECK(f.client.intents.back().kind == IntentKind::OpenNoteEditor);
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Up, {201.0, 150.0}));
    // Too slow: a plain click again (already selected -> nothing).
    f.now = 5.0;
    const std::size_t before = f.client.intents.size();
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, where));
    CHECK(f.client.intents.size() == before + 1);
    CHECK(f.client.intents.back().kind != IntentKind::OpenNoteEditor);
}

RIVET_TEST(layerClickBetweenPagesClearsTheSelectionAndPassesThrough) {
    LayerFixture f;
    f.client.selection = selected(true);
    CHECK(!f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, {10.0, 10.0})));
    CHECK(!f.client.selection.has_value());
    CHECK(f.client.intents.back().kind == IntentKind::ClearSelection);
}

RIVET_TEST(layerAfterMouseConvertsATextSelectionUnderMarkupTools) {
    LayerFixture f;
    f.client.machine.setTool(AnnotationTool::Highlight);
    f.client.textSelection = true;
    ui::PointerEvent up = pointer(ui::PointerEventType::Up, {200.0, 150.0});
    f.layer.afterMouse(f.host, up);
    CHECK(f.client.intents.back().kind == IntentKind::ConvertTextSelection);

    f.client.intents.clear();
    f.client.textSelection = false;
    f.layer.afterMouse(f.host, up);
    CHECK(f.client.intents.empty());
    f.client.textSelection = true;
    f.layer.afterMouse(f.host, pointer(ui::PointerEventType::Down, {200.0, 150.0}));
    CHECK(f.client.intents.empty());
    f.client.machine.setTool(AnnotationTool::Select);
    f.layer.afterMouse(f.host, up);
    CHECK(f.client.intents.empty());
}

RIVET_TEST(layerKeysEscapeDeleteBackspaceEnter) {
    LayerFixture f;
    f.client.machine.setTool(AnnotationTool::Rectangle);
    ui::KeyEvent esc;
    esc.key = ui::Key::Escape;
    CHECK(f.layer.onKey(f.host, esc)); // Rectangle -> Select
    CHECK(f.client.intents.back().kind == IntentKind::SelectTool);
    CHECK(!f.layer.onKey(f.host, esc)); // nothing left

    f.client.selection = selected(true);
    ui::KeyEvent del;
    del.key = ui::Key::Delete;
    CHECK(f.layer.onKey(f.host, del));
    CHECK(f.client.intents.back().kind == IntentKind::Delete);

    ui::KeyEvent other;
    other.key = ui::Key::Character;
    other.text = "a";
    CHECK(!f.layer.onKey(f.host, other));
    ui::KeyEvent cmdDelete = del;
    cmdDelete.modifiers.command = true;
    f.client.selection = selected(true);
    CHECK(!f.layer.onKey(f.host, cmdDelete));
}

RIVET_TEST(layerPaintsPageAnnotationsAndAboveChrome) {
    LayerFixture f;
    f.client.annotations = std::make_shared<const std::vector<editor::AnnotationView>>(
        std::vector<editor::AnnotationView>{squareView()});
    FakePaintContext page;
    f.layer.paintPage(f.host, 0, f.host.page0, page);
    CHECK_EQ(page.paths.size(), 2u);
    FakePaintContext other;
    f.layer.paintPage(f.host, 1, f.host.page0, other);
    CHECK(other.paths.empty());

    f.client.selection = selected(true);
    FakePaintContext above;
    f.layer.paintAbove(f.host, above);
    CHECK_EQ(above.dashedStrokes.size(), 1u);
    CHECK_EQ(above.fills.size(), 8u);
    CHECK_EQ(above.clipDepth, 0);

    // During a move of the selection only the outline preview shows.
    f.client.machine.setTool(AnnotationTool::Select);
    AnnotationInteraction::HitInfo info = selected(true).info;
    info.canMove = true;
    f.client.machine.setHitTest([info](std::size_t, core::Point, double) { return std::optional(info); });
    f.client.selection = AnnotationInteraction::Selected{0, info};
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Down, {140.0, 130.0}));
    (void)f.layer.onMouse(f.host, pointer(ui::PointerEventType::Move, {180.0, 160.0}));
    FakePaintContext moving;
    f.layer.paintAbove(f.host, moving);
    CHECK(moving.fills.empty());
    CHECK_EQ(moving.dashedStrokes.size(), 1u);
    CHECK_EQ(moving.clipDepth, 0);
}
