#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/Sidebar.hpp"
#include "ui/Toolbar.hpp"
#include "ui/UiTypes.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::ui::Button;
using rivet::ui::Container;
using rivet::ui::IRedrawSink;
using rivet::ui::KeyEvent;
using rivet::ui::Key;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::Sidebar;
using rivet::ui::Toolbar;
using rivet::ui::Widget;
using rivet::ui::testing::CountingRedrawSink;
using rivet::ui::testing::FakePaintContext;

namespace {

// Minimal probe widget that records what it received.
class RecordingWidget final : public Widget {
public:
    ~RecordingWidget() override {
        if (destroyedFlag != nullptr) *destroyedFlag = true;
    }

    bool onMouse(const PointerEvent& event) override {
        lastMousePosition = event.position;
        ++mouseEvents;
        return consumeMouse;
    }

    bool onKey(const KeyEvent& /*event*/) override {
        ++keyEvents;
        return consumeKeys;
    }

    std::optional<Point> lastMousePosition;
    int mouseEvents = 0;
    int keyEvents = 0;
    bool consumeMouse = true;
    bool consumeKeys = true;
    bool* destroyedFlag = nullptr; // set by the destructor when non-null
};

} // namespace

RIVET_TEST(hitTestTopmostChildWins) {
    Container root;
    root.setFrame(Rect{0.0, 0.0, 200.0, 200.0});

    auto first = std::make_unique<RecordingWidget>();
    first->setFrame(Rect{10.0, 10.0, 50.0, 50.0});
    auto second = std::make_unique<RecordingWidget>();
    second->setFrame(Rect{30.0, 30.0, 50.0, 50.0});
    RecordingWidget* firstPtr = first.get();
    RecordingWidget* secondPtr = second.get();

    root.addChild(std::move(first));
    root.addChild(std::move(second));

    // Overlap: the last-added (topmost) child wins.
    CHECK_EQ(root.hitTest(Point{40.0, 40.0}), secondPtr);
    // Only the first child covers this point.
    CHECK_EQ(root.hitTest(Point{15.0, 15.0}), firstPtr);
    // Outside every child: the container itself.
    CHECK_EQ(root.hitTest(Point{5.0, 5.0}), &root);
    CHECK_EQ(root.hitTest(Point{150.0, 150.0}), &root);
}

RIVET_TEST(mouseInputIsConvertedToChildCoordinates) {
    Container root;
    root.setFrame(Rect{0.0, 0.0, 200.0, 200.0});

    auto first = std::make_unique<RecordingWidget>();
    first->setFrame(Rect{10.0, 10.0, 50.0, 50.0});
    auto second = std::make_unique<RecordingWidget>();
    second->setFrame(Rect{30.0, 30.0, 50.0, 50.0});
    RecordingWidget* firstPtr = first.get();
    RecordingWidget* secondPtr = second.get();

    root.addChild(std::move(first));
    root.addChild(std::move(second));

    PointerEvent down;
    down.type = PointerEventType::Down;
    down.position = Point{35.0, 35.0};
    down.button = 1;
    CHECK_EQ(root.onMouse(down), true);
    CHECK_EQ(secondPtr->mouseEvents, 1);
    CHECK(secondPtr->lastMousePosition.has_value());
    CHECK_NEAR(secondPtr->lastMousePosition->x, 5.0, 1e-12); // 35 - 30
    CHECK_NEAR(secondPtr->lastMousePosition->y, 5.0, 1e-12);
    CHECK_EQ(firstPtr->mouseEvents, 0);

    // A non-consuming topmost child lets the event fall through to the
    // sibling underneath (both overlap at this point).
    secondPtr->consumeMouse = false;
    PointerEvent secondDown;
    secondDown.type = PointerEventType::Down;
    secondDown.position = Point{35.0, 35.0};
    CHECK_EQ(root.onMouse(secondDown), true);
    CHECK_EQ(firstPtr->mouseEvents, 1);
    CHECK_NEAR(firstPtr->lastMousePosition->x, 25.0, 1e-12); // 35 - 10
    CHECK_NEAR(firstPtr->lastMousePosition->y, 25.0, 1e-12);

    // Nobody covers the point: not consumed.
    PointerEvent miss;
    miss.type = PointerEventType::Down;
    miss.position = Point{150.0, 150.0};
    CHECK_EQ(root.onMouse(miss), false);
}

RIVET_TEST(invalidationPropagatesToSink) {
    Container root;
    auto child = std::make_unique<RecordingWidget>();
    auto grandchild = std::make_unique<RecordingWidget>();
    RecordingWidget* childPtr = child.get();
    RecordingWidget* grandchildPtr = grandchild.get();
    child->addChild(std::move(grandchild));
    root.addChild(std::move(child));

    CountingRedrawSink sink;
    root.setRedrawSink(&sink);
    CHECK_EQ(sink.count, 0);

    childPtr->invalidate();
    CHECK_EQ(sink.count, 1);
    grandchildPtr->invalidate();
    CHECK_EQ(sink.count, 2);

    // Children added after the sink was installed inherit it. The structural
    // change itself also invalidates the tree.
    auto late = std::make_unique<RecordingWidget>();
    RecordingWidget* latePtr = late.get();
    root.addChild(std::move(late));
    CHECK_EQ(sink.count, 3); // addChild invalidates on its own
    latePtr->invalidate();
    CHECK_EQ(sink.count, 4);

    // Without a sink, invalidate() is a harmless no-op.
    root.setRedrawSink(nullptr);
    childPtr->invalidate();
    CHECK_EQ(sink.count, 4);
}

RIVET_TEST(removeChildRestoresInvariants) {
    Container root;
    root.setFrame(Rect{0.0, 0.0, 300.0, 300.0});

    auto first = std::make_unique<RecordingWidget>();
    auto middle = std::make_unique<RecordingWidget>();
    auto last = std::make_unique<RecordingWidget>();
    RecordingWidget* firstPtr = first.get();
    RecordingWidget* middlePtr = middle.get();
    RecordingWidget* lastPtr = last.get();

    root.addChild(std::move(first));
    root.addChild(std::move(middle));
    root.addChild(std::move(last));

    CHECK_EQ(root.children().size(), std::size_t{3});
    // detachChild transfers ownership out and keeps the widget alive, so its
    // detached invariants can be verified.
    std::unique_ptr<Widget> detached = root.detachChild(middlePtr);
    CHECK(detached != nullptr);
    CHECK_EQ(detached.get(), middlePtr);
    CHECK_EQ(root.children().size(), std::size_t{2});
    CHECK_EQ(root.children()[0].get(), firstPtr);
    CHECK_EQ(root.children()[1].get(), lastPtr);
    CHECK_EQ(firstPtr->parent(), &root);
    CHECK_EQ(lastPtr->parent(), &root);
    CHECK_EQ(middlePtr->parent(), nullptr);

    // Removing an already-detached (or unknown) child fails cleanly.
    CHECK_EQ(root.removeChild(middlePtr), false);
    CHECK_EQ(root.removeChild(nullptr), false);

    // removeChild detaches and DESTROYS the child.
    bool destroyed = false;
    auto doomed = std::make_unique<RecordingWidget>();
    doomed->destroyedFlag = &destroyed;
    RecordingWidget* doomedPtr = doomed.get();
    root.addChild(std::move(doomed));
    CHECK_EQ(root.children().size(), std::size_t{3});
    CHECK(root.removeChild(doomedPtr));
    CHECK_EQ(destroyed, true);
    CHECK_EQ(root.children().size(), std::size_t{2});

    // Removing detaches the invalidation path: a detached subtree no longer
    // reaches the root's sink.
    CountingRedrawSink sink;
    root.setRedrawSink(&sink);
    auto orphan = std::make_unique<RecordingWidget>();
    RecordingWidget* orphanPtr = orphan.get();
    root.addChild(std::move(orphan));
    std::unique_ptr<Widget> detachedOrphan = root.detachChild(orphanPtr); // keeps it alive
    CHECK(detachedOrphan != nullptr);
    CHECK_EQ(orphanPtr->parent(), nullptr);
    // The structural changes invalidated the root themselves; the detached
    // orphan must no longer reach the root's sink.
    const int countBefore = sink.count;
    orphanPtr->invalidate();
    CHECK_EQ(sink.count, countBefore);
}

RIVET_TEST(keyEventsRouteToFocusedChildren) {
    Container root;
    auto focused = std::make_unique<RecordingWidget>();
    auto unfocused = std::make_unique<RecordingWidget>();
    RecordingWidget* focusedPtr = focused.get();
    RecordingWidget* unfocusedPtr = unfocused.get();
    root.addChild(std::move(focused));
    root.addChild(std::move(unfocused));

    focusedPtr->setFocused(true);
    CHECK(focusedPtr->isFocused());
    CHECK_EQ(unfocusedPtr->isFocused(), false);

    KeyEvent event;
    event.key = Key::Enter;
    CHECK_EQ(root.onKey(event), true);
    CHECK_EQ(focusedPtr->keyEvents, 1);
    CHECK_EQ(unfocusedPtr->keyEvents, 0);

    // No focused child: unconsumed.
    focusedPtr->setFocused(false);
    CHECK_EQ(focusedPtr->isFocused(), false);
    CHECK_EQ(root.onKey(event), false);
    CHECK_EQ(focusedPtr->keyEvents, 1);
}

RIVET_TEST(toolbarPositionsItemsLeftToRight) {
    Toolbar toolbar{40.0};
    toolbar.setFrame(Rect{0.0, 0.0, 400.0, 40.0});
    CHECK_NEAR(toolbar.height(), 40.0, 1e-12);

    auto left = std::make_unique<Button>("One");
    left->setFrame(Rect{0.0, 0.0, 60.0, 28.0});
    auto right = std::make_unique<Button>("Two");
    right->setFrame(Rect{0.0, 0.0, 80.0, 28.0});
    Button* leftPtr = left.get();
    Button* rightPtr = right.get();

    toolbar.addItem(std::move(left));              // default spacing 8
    toolbar.addItem(std::move(right), 20.0);       // custom spacing

    CHECK_NEAR(leftPtr->frame().origin.x, Toolbar::kPadding, 1e-12);            // 10
    CHECK_NEAR(leftPtr->frame().origin.y, (40.0 - 28.0) / 2.0, 1e-12);          // 6
    CHECK_NEAR(leftPtr->frame().size.width, 60.0, 1e-12);                       // width preserved
    CHECK_NEAR(leftPtr->frame().size.height, 28.0, 1e-12);
    CHECK_NEAR(rightPtr->frame().origin.x, Toolbar::kPadding + 60.0 + 8.0, 1e-12); // 78
    CHECK_NEAR(rightPtr->frame().origin.y, 6.0, 1e-12);

    FakePaintContext context;
    const Size preferred = toolbar.preferredSize(context);
    CHECK_NEAR(preferred.height, 40.0, 1e-12);
    // padding + items + inter-item gaps (no trailing gap after the last item).
    CHECK_NEAR(preferred.width, 2.0 * Toolbar::kPadding + 60.0 + 8.0 + 80.0, 1e-12);
}

RIVET_TEST(sidebarSelectsRowOnMouseDown) {
    Sidebar sidebar;
    sidebar.setFrame(Rect{0.0, 0.0, 160.0, 200.0});

    std::vector<std::size_t> selections;
    sidebar.setOnSelectionChanged([&selections](std::size_t index) { selections.push_back(index); });

    sidebar.setItems({"Pages", "Info", "Export"});
    CHECK(!sidebar.selectedIndex().has_value());

    // Row 1 spans y [24, 48): padding 10 + row height 24 + 12.
    CHECK_EQ(sidebar.rowIndexAt(Point{80.0, Sidebar::kPadding + Sidebar::kRowHeight + 12.0})
                 .value_or(std::size_t{99}),
             std::size_t{1});
    CHECK(!sidebar.rowIndexAt(Point{80.0, 5.0}).has_value());   // above the first row
    CHECK(!sidebar.rowIndexAt(Point{80.0, 195.0}).has_value()); // below the last row

    PointerEvent down;
    down.type = PointerEventType::Down;
    down.position = Point{80.0, Sidebar::kPadding + Sidebar::kRowHeight + 12.0};
    CHECK_EQ(sidebar.onMouse(down), true);
    CHECK(sidebar.selectedIndex().has_value());
    CHECK_EQ(*sidebar.selectedIndex(), std::size_t{1});
    CHECK_EQ(selections.size(), std::size_t{1});
    CHECK_EQ(selections[0], std::size_t{1});

    // Re-picking the selected row fires nothing new.
    CHECK_EQ(sidebar.onMouse(down), true);
    CHECK_EQ(selections.size(), std::size_t{1});

    // A click below the rows is not consumed and changes nothing.
    PointerEvent miss;
    miss.type = PointerEventType::Down;
    miss.position = Point{80.0, 195.0};
    CHECK_EQ(sidebar.onMouse(miss), false);
    CHECK_EQ(selections.size(), std::size_t{1});

    // Selecting paints exactly one highlighted row.
    sidebar.setSelectedIndex(std::size_t{2});
    FakePaintContext context;
    sidebar.paint(context);
    CHECK_EQ(context.roundedFills.size(), std::size_t{1});

    // Shrinking the item list drops an out-of-range selection.
    sidebar.setItems({"Only"});
    CHECK(!sidebar.selectedIndex().has_value());
}
