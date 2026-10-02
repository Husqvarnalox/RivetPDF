// SPDX-License-Identifier: MPL-2.0
// ContentLayer: viewport events -> interaction inputs -> client intents, the
// inactive state, selection chrome painting, and (through a real viewport)
// the rule that an active viewport tool sees keys before the content layer.
#include "RivetTest.h"

#include "ContentTestKit.hpp"
#include "Fakes.hpp"

#include "app/ContentLayer.hpp"

#include <optional>
#include <utility>
#include <vector>

using namespace rivet;
using app::ContentInteraction;
using app::ContentLayer;
using app::ContentTool;
using core::ObjectId;
using core::Point;
using core::Rect;
using IntentKind = ContentInteraction::Intent::Kind;
using ui::testing::FakePaintContext;

namespace {

class FakeHost final : public ui::ViewportToolHost {
public:
    Rect pageRect{10.0, 10.0, 600.0, 800.0};
    int repaints = 0;

    std::optional<Rect> pageRectInViewport(std::size_t index) const override {
        if (index != 0) return std::nullopt;
        return pageRect;
    }
    double zoomFactor() const override { return 1.0; }
    Rect viewportBounds() const override { return Rect{0.0, 0.0, 700.0, 500.0}; }
    void requestRepaint() override { ++repaints; }
    std::size_t pageCount() const override { return 1; }
    std::optional<std::pair<std::size_t, Point>> pageAt(Point p) const override {
        if (!pageRect.contains(p)) return std::nullopt;
        return std::pair<std::size_t, Point>{0, Point{p.x - pageRect.minX(), p.y - pageRect.minY()}};
    }
};

class StubClient final : public app::ContentLayerClient {
public:
    ContentInteraction machine;
    bool isActive = true;
    std::optional<ContentInteraction::Selected> selection;
    std::vector<ContentInteraction::Intent> intents;
    int pressed = 0;
    int placed = 0;
    ContentInteraction::HitInfo target;

    StubClient() {
        machine.setHitTest([this](std::size_t, Point p, double tolerance) {
            std::optional<ContentInteraction::HitInfo> hit;
            const Rect grown{target.bounds.minX() - tolerance, target.bounds.minY() - tolerance,
                             target.bounds.size.width + 2.0 * tolerance, target.bounds.size.height + 2.0 * tolerance};
            if (grown.contains(p)) hit = target;
            return hit;
        });
        machine.setTool(ContentTool::SelectObject);
    }

    ContentInteraction& interaction() override { return machine; }
    const ContentInteraction& interaction() const override { return machine; }
    bool active() const override { return isActive; }
    std::optional<ContentInteraction::Selected> currentSelection() const override { return selection; }
    void pointerPressed() override { ++pressed; }
    void applyIntent(const ContentInteraction::Intent& intent, bool) override {
        intents.push_back(intent);
        if (intent.kind == IntentKind::Select || intent.kind == IntentKind::OpenEditor) {
            selection = ContentInteraction::Selected{intent.page, intent.info};
        } else if (intent.kind == IntentKind::ClearSelection) {
            selection.reset();
        }
    }
    std::optional<EditingOutline> editingOutline() const override { return std::nullopt; }
    void placeEditor(const ui::ViewportToolHost&) override { ++placed; }
};

ContentInteraction::HitInfo imageInfo() {
    ContentInteraction::HitInfo info;
    info.id = ObjectId{1};
    info.kind = ContentInteraction::Kind::Image;
    info.bounds = Rect{100.0, 100.0, 200.0, 100.0};
    info.quad = test::content::quadOf(info.bounds);
    info.canMove = info.canDelete = info.canResize = true;
    info.capability = editor::ContentCapability::MoveOnly;
    return info;
}

ContentInteraction::HitInfo textInfo() {
    ContentInteraction::HitInfo info;
    info.id = ObjectId{2};
    info.isBlock = true;
    info.kind = ContentInteraction::Kind::Text;
    info.bounds = Rect{50.0, 300.0, 200.0, 20.0};
    info.quad = test::content::quadOf(info.bounds);
    info.canMove = info.canDelete = info.canEditText = info.canWrap = true;
    info.wrapBase = 200.0;
    info.capability = editor::ContentCapability::FullyEditable;
    return info;
}

ui::PointerEvent pointer(ui::PointerEventType type, Point p, int button = 1) {
    ui::PointerEvent event;
    event.type = type;
    event.position = p;
    event.button = button;
    return event;
}

// Viewport-local point of a page point of FakeHost (page origin 10, 10).
Point at(double x, double y) { return Point{x + 10.0, y + 10.0}; }

// A tool that records what it saw and may consume (stands in for the crop tool).
class StubTool final : public ui::ViewportTool {
public:
    bool consume = false;
    int keys = 0;
    bool onMouse(ui::ViewportToolHost&, const ui::PointerEvent&) override { return consume; }
    bool onKey(ui::ViewportToolHost&, const ui::KeyEvent&) override {
        ++keys;
        return consume;
    }
    void paint(const ui::ViewportToolHost&, ui::PaintContext&) const override {}
};

} // namespace

RIVET_TEST(contentLayerIgnoresEverythingWhileTheClientIsInactive) {
    StubClient client;
    client.isActive = false;
    client.target = imageInfo();
    ContentLayer layer(client);
    FakeHost host;
    CHECK(!layer.onMouse(host, pointer(ui::PointerEventType::Down, at(150.0, 150.0))));
    CHECK(!layer.onMouse(host, pointer(ui::PointerEventType::Move, at(150.0, 150.0), 0)));
    ui::KeyEvent key;
    key.key = ui::Key::Delete;
    CHECK(!layer.onKey(host, key));
    CHECK_EQ(client.pressed, 0);
    CHECK(client.intents.empty());
    CHECK(!client.machine.hover().has_value());

    FakePaintContext paint;
    layer.paintAbove(host, paint);
    CHECK_EQ(client.placed, 1); // the inline editor is still re-placed
    CHECK(paint.lines.empty());
    CHECK(paint.strokes.empty());
    CHECK(paint.dashedStrokes.empty());
}

RIVET_TEST(contentLayerDownCommitsTheEditorThenSelectsAndConsumes) {
    StubClient client;
    client.target = imageInfo();
    ContentLayer layer(client);
    FakeHost host;
    CHECK(layer.onMouse(host, pointer(ui::PointerEventType::Down, at(150.0, 150.0))));
    CHECK_EQ(client.pressed, 1);
    CHECK_EQ(client.intents.size(), std::size_t{1});
    CHECK(client.intents.front().kind == IntentKind::Select);
    CHECK(client.intents.front().info.id == ObjectId{1});
    CHECK_GT(host.repaints, 0);
    CHECK(layer.onMouse(host, pointer(ui::PointerEventType::Up, at(150.0, 150.0))));

    // Non-primary buttons and modified clicks are left to the viewport.
    CHECK(!layer.onMouse(host, pointer(ui::PointerEventType::Down, at(150.0, 150.0), 2)));
    ui::PointerEvent command = pointer(ui::PointerEventType::Down, at(150.0, 150.0));
    command.modifiers.command = true;
    CHECK(!layer.onMouse(host, command));
    CHECK_EQ(client.pressed, 1);

    // Between pages: the selection is dropped, the viewport keeps the event.
    CHECK(!layer.onMouse(host, pointer(ui::PointerEventType::Down, Point{650.0, 495.0})));
    CHECK(client.intents.back().kind == IntentKind::ClearSelection);
    CHECK(!client.intents.back().consumed);
}

RIVET_TEST(contentLayerDoubleClickOnEditableTextOpensTheEditor) {
    StubClient client;
    client.target = textInfo();
    double now = 0.0;
    ContentLayer layer(client, [&now] { return now; });
    FakeHost host;
    CHECK(layer.onMouse(host, pointer(ui::PointerEventType::Down, at(100.0, 310.0))));
    layer.onMouse(host, pointer(ui::PointerEventType::Up, at(100.0, 310.0)));
    now = 0.2;
    CHECK(layer.onMouse(host, pointer(ui::PointerEventType::Down, at(100.0, 310.0))));
    CHECK(client.intents.back().kind == IntentKind::OpenEditor);
    layer.onMouse(host, pointer(ui::PointerEventType::Up, at(100.0, 310.0)));
    // Too slow: a plain select again.
    now = 5.0;
    layer.onMouse(host, pointer(ui::PointerEventType::Down, at(100.0, 310.0)));
    CHECK(client.intents.back().kind == IntentKind::Select);
}

RIVET_TEST(contentLayerPaintsEightHandlesForImagesAndOneWrapHandleForText) {
    FakeHost host;
    {
        StubClient client;
        client.selection = ContentInteraction::Selected{0, imageInfo()};
        ContentLayer layer(client);
        FakePaintContext paint;
        layer.paintAbove(host, paint);
        CHECK_EQ(paint.strokes.size(), std::size_t{8});   // one stroked box per handle
        CHECK_EQ(paint.dashedStrokes.size(), std::size_t{1}); // the dashed outline
        CHECK_EQ(paint.clipDepth, 0);
    }
    {
        StubClient client;
        client.selection = ContentInteraction::Selected{0, textInfo()};
        ContentLayer layer(client);
        FakePaintContext paint;
        layer.paintAbove(host, paint);
        CHECK_EQ(paint.strokes.size(), std::size_t{1});
        CHECK_EQ(paint.lines.size(), std::size_t{4}); // the quad outline
        CHECK_EQ(paint.dashedStrokes.size(), std::size_t{0});
    }
}

RIVET_TEST(contentLayerPaintsTheEditorOutlineEvenWhenInactive) {
    class OutlineClient final : public app::ContentLayerClient {
    public:
        ContentInteraction machine;
        ContentInteraction& interaction() override { return machine; }
        const ContentInteraction& interaction() const override { return machine; }
        bool active() const override { return false; }
        std::optional<ContentInteraction::Selected> currentSelection() const override { return std::nullopt; }
        void pointerPressed() override {}
        void applyIntent(const ContentInteraction::Intent&, bool) override {}
        std::optional<EditingOutline> editingOutline() const override {
            return EditingOutline{0, test::content::quadOf(Rect{20.0, 20.0, 100.0, 20.0})};
        }
        void placeEditor(const ui::ViewportToolHost&) override {}
    } client;
    ContentLayer layer(client);
    FakeHost host;
    FakePaintContext paint;
    layer.paintAbove(host, paint);
    CHECK_EQ(paint.lines.size(), std::size_t{4});
}

RIVET_TEST(contentLayerDeleteGoesToAnActiveViewportToolBeforeTheContentSelection) {
    test::content::ContentShell shell;
    CHECK(shell.open("layer") != nullptr);
    shell.addImage(1, Rect{100.0, 100.0, 200.0, 100.0});
    shell.content->setTool(ContentTool::SelectObject);
    shell.click(Point{150.0, 150.0});
    CHECK(shell.content->selected().has_value());

    StubTool crop;
    crop.consume = true;
    shell.viewport->setActiveTool(&crop);
    CHECK(shell.key(ui::Key::Delete));
    CHECK_EQ(crop.keys, 1);
    CHECK_EQ(shell.fake->deleteCalls, 0);
    CHECK_EQ(shell.page().objects.size(), std::size_t{1});

    // An active tool owns the keyboard: keys it leaves alone never reach the layers.
    crop.consume = false;
    shell.key(ui::Key::Delete);
    CHECK_EQ(crop.keys, 2);
    CHECK_EQ(shell.fake->deleteCalls, 0);
    CHECK_EQ(shell.page().objects.size(), std::size_t{1});

    // Without the tool the key reaches the content selection.
    shell.viewport->setActiveTool(nullptr);
    CHECK(shell.key(ui::Key::Delete));
    CHECK_EQ(shell.fake->deleteCalls, 1);
}
