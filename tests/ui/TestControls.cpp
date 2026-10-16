// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "Fakes.hpp"

#include "ui/Animation.hpp"
#include "ui/IconButton.hpp"
#include "ui/Icons.hpp"
#include "ui/PopupMenu.hpp"
#include "ui/SegmentedControl.hpp"
#include "ui/Theme.hpp"
#include "ui/Toolbar.hpp"
#include "ui/Tooltip.hpp"
#include "ui/ZoomControl.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// ADR-0033 controls: icons, icon buttons, segmented control, zoom control,
// popover menu, tooltip timing and toolbar keyboard navigation.

using rivet::core::Point;
using rivet::core::Rect;
using rivet::ui::Icon;
using rivet::ui::IconButton;
using rivet::ui::KeyEvent;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::testing::FakePaintContext;
namespace ui = rivet::ui;

namespace {

PointerEvent mouse(PointerEventType type, Point p, int button = 1) {
    PointerEvent e;
    e.type = type;
    e.position = p;
    e.button = type == PointerEventType::Move ? 0 : button;
    return e;
}

KeyEvent key(ui::Key k, bool shift = false) {
    KeyEvent e;
    e.key = k;
    e.modifiers.shift = shift;
    return e;
}

struct ManualRunner final : ui::IDelayedRunner {
    std::vector<std::pair<std::chrono::milliseconds, std::function<void()>>> tasks;
    void runAfter(std::chrono::milliseconds delay, std::function<void()> task) override {
        tasks.emplace_back(delay, std::move(task));
    }
    void runAll() {
        auto pending = std::move(tasks);
        tasks.clear();
        for (auto& t : pending) t.second();
    }
};

struct FakeClock {
    double now = 100.0;
    FakeClock() {
        ui::setAnimationClockForTesting([this] { return now; });
    }
    ~FakeClock() { ui::setAnimationClockForTesting(nullptr); }
};

} // namespace

RIVET_TEST(everyIconDrawsInsideTheGrid) {
    std::set<std::string> names;
    for (std::size_t i = 0; i < ui::kIconCount; ++i) {
        const auto icon = static_cast<Icon>(i);
        const auto shapes = ui::iconShapes(icon);
        CHECK(!shapes.empty());
        names.insert(std::string(ui::iconName(icon)));
        for (const ui::IconShape& shape : shapes) {
            CHECK(!shape.path.empty());
            for (const ui::PathSegment& s : shape.path.segments) {
                if (s.op == ui::PathSegment::Op::Close) continue;
                CHECK(s.p.x >= 0.0 && s.p.x <= 20.0 && s.p.y >= 0.0 && s.p.y <= 20.0);
            }
        }
    }
    CHECK_EQ(names.size(), ui::kIconCount); // unique names

    // Strokes are 1.5 px at 20 px, rounded, and scale with the box.
    FakePaintContext context;
    ui::drawIcon(context, Icon::Search, Rect{10.0, 10.0, 40.0, 40.0}, ui::colors().textPrimary);
    CHECK(!context.paths.empty());
    CHECK_NEAR(context.paths.front().strokeWidth, 3.0, 1e-9);
    CHECK(context.paths.front().cap == ui::LineCap::Round);
}

RIVET_TEST(iconButtonHoverFadesAndClicks) {
    FakeClock clock;
    IconButton button(Icon::Print, "Print");
    button.setFrame(Rect{0.0, 0.0, 28.0, 28.0});
    CHECK_EQ(button.tooltip(), std::string("Print"));
    int clicks = 0;
    button.setOnClick([&] { ++clicks; });

    FakePaintContext rest;
    button.paint(rest);
    CHECK(rest.roundedFills.empty()); // borderless: nothing at rest

    CHECK(button.onMouse(mouse(PointerEventType::Move, Point{10.0, 10.0})));
    FakePaintContext start;
    button.paint(start);
    CHECK(start.roundedFills.empty()); // fade at 0
    clock.now += 0.2;                  // past the 120 ms fade
    FakePaintContext done;
    button.paint(done);
    CHECK_EQ(done.roundedFills.size(), std::size_t{1});
    CHECK(done.roundedFills.back().color == ui::colors().controlFillHover);

    CHECK(button.onMouse(mouse(PointerEventType::Down, Point{10.0, 10.0})));
    CHECK(button.onMouse(mouse(PointerEventType::Up, Point{10.0, 10.0})));
    CHECK_EQ(clicks, 1);

    // Disabled: drawn in textDisabled, never clicks.
    button.setEnabled(false);
    button.onMouse(mouse(PointerEventType::Down, Point{10.0, 10.0}));
    button.onMouse(mouse(PointerEventType::Up, Point{10.0, 10.0}));
    CHECK_EQ(clicks, 1);
    FakePaintContext disabled;
    button.paint(disabled);
    CHECK(!disabled.paths.empty());
    CHECK(disabled.paths.front().color == ui::colors().textDisabled);

    // Active: accent icon on the subtle tint.
    button.setEnabled(true);
    button.setActive(true);
    FakePaintContext active;
    button.paint(active);
    CHECK(active.roundedFills.front().color == ui::colors().accentSubtle);
    CHECK(active.paths.front().color == ui::colors().accentText);
}

RIVET_TEST(segmentedControlSelectsEnabledSegments) {
    ui::SegmentedControl control({{"View", {}, {}, 60.0, true},
                                  {"Annotate", {}, {}, 80.0, true},
                                  {"Edit", {}, {}, 60.0, false}});
    control.setFrame(Rect{0.0, 0.0, control.naturalSize().width, 28.0});
    std::vector<std::size_t> picked;
    control.setOnSelect([&](std::size_t i) { picked.push_back(i); });

    const Rect annotate = control.segmentRect(1);
    control.onMouse(mouse(PointerEventType::Down, annotate.center()));
    control.onMouse(mouse(PointerEventType::Up, annotate.center()));
    const Rect edit = control.segmentRect(2);
    control.onMouse(mouse(PointerEventType::Down, edit.center()));
    control.onMouse(mouse(PointerEventType::Up, edit.center()));
    CHECK_EQ(picked.size(), std::size_t{1});
    CHECK_EQ(picked.front(), std::size_t{1});

    control.setSelected(1);
    FakePaintContext context;
    control.paint(context);
    bool selectedFill = false;
    for (const auto& fill : context.roundedFills) selectedFill = selectedFill || fill.color == ui::colors().controlSelectedFill;
    CHECK(selectedFill);
    CHECK_EQ(control.keyboardStopCount(), 3);
}

RIVET_TEST(zoomControlRoutesItsParts) {
    ui::ZoomControl zoom;
    zoom.setFrame(Rect{0.0, 0.0, ui::ZoomControl::kWidth, 28.0});
    int in = 0;
    int out = 0;
    std::optional<Rect> percent;
    zoom.setOnZoomIn([&] { ++in; });
    zoom.setOnZoomOut([&] { ++out; });
    zoom.setOnPercent([&](const Rect& r) { percent = r; });
    for (const auto part : {ui::ZoomControl::Part::ZoomOut, ui::ZoomControl::Part::Percent, ui::ZoomControl::Part::ZoomIn}) {
        const Point p = zoom.partRect(part).center();
        zoom.onMouse(mouse(PointerEventType::Down, p));
        zoom.onMouse(mouse(PointerEventType::Up, p));
    }
    CHECK_EQ(in, 1);
    CHECK_EQ(out, 1);
    CHECK(percent.has_value());
    zoom.setPercentText("173%");
    FakePaintContext context;
    zoom.paint(context);
    bool sawPercent = false;
    for (const auto& t : context.texts) sawPercent = sawPercent || t.text == "173%";
    CHECK(sawPercent);
}

RIVET_TEST(popupMenuActivatesAndDismisses) {
    ui::PopupMenu menu;
    int chosen = 0;
    int closed = 0;
    menu.setOnClosed([&] { ++closed; });
    std::vector<ui::PopupMenu::Item> items(3);
    items[0].title = "Fit Width";
    items[0].action = [&] { chosen = 1; };
    items[1].title = "Disabled";
    items[1].enabled = false;
    items[2].title = "More";
    items[2].submenu.resize(1);
    items[2].submenu[0].title = "Deep";
    items[2].submenu[0].action = [&] { chosen = 3; };
    const Rect area{0.0, 0.0, 800.0, 600.0};
    menu.open(items, Rect{100.0, 10.0, 60.0, 28.0}, area);
    CHECK(menu.isOpen());
    CHECK(menu.menuRect().minY() > 38.0);

    // Keyboard: Down picks the first enabled item, Enter runs it.
    menu.onKey(key(ui::Key::Down));
    menu.onKey(key(ui::Key::Enter));
    CHECK_EQ(chosen, 1);
    CHECK(!menu.isOpen());
    CHECK_EQ(closed, 1);

    // Submenu by keyboard: Down, Down (skips disabled) -> More, Right, Enter.
    menu.open(items, Rect{100.0, 10.0, 60.0, 28.0}, area);
    menu.onKey(key(ui::Key::Down));
    menu.onKey(key(ui::Key::Down));
    menu.onKey(key(ui::Key::Right));
    menu.onKey(key(ui::Key::Enter));
    CHECK_EQ(chosen, 3);

    // A click outside dismisses without running anything.
    chosen = 0;
    menu.open(items, Rect{100.0, 10.0, 60.0, 28.0}, area);
    menu.onMouse(mouse(PointerEventType::Down, Point{700.0, 500.0}));
    CHECK(!menu.isOpen());
    CHECK_EQ(chosen, 0);
}

RIVET_TEST(tooltipShowsAfterTheDelayOnlyForTheSameTarget) {
    FakeClock clock;
    ManualRunner runner;
    ui::Tooltip tooltip(&runner);
    ui::Widget a;
    ui::Widget b;
    const Rect area{0.0, 0.0, 800.0, 600.0};
    tooltip.hoverTarget(&a, "Open  \xE2\x8C\x98O", Rect{10.0, 10.0, 28.0, 28.0}, area);
    CHECK_EQ(runner.tasks.size(), std::size_t{1});
    CHECK(runner.tasks.front().first == std::chrono::milliseconds(600));
    CHECK(!tooltip.visible());
    // Moving to another widget before the delay cancels the first show.
    tooltip.hoverTarget(&b, "Save", Rect{40.0, 10.0, 28.0, 28.0}, area);
    runner.runAll();
    CHECK(tooltip.visible());
    CHECK_EQ(tooltip.text(), std::string("Save"));
    CHECK(tooltip.frame().minY() > 38.0); // under the anchor
    tooltip.dismiss();
    CHECK(!tooltip.visible());
    CHECK(tooltip.frame().isEmpty());
}

RIVET_TEST(toolbarKeyboardNavigationVisitsStopsInVisualOrder) {
    ui::Toolbar toolbar(44.0);
    toolbar.setFrame(Rect{0.0, 0.0, 600.0, 44.0});
    int opened = 0;
    int printed = 0;
    auto open = std::make_unique<IconButton>(Icon::Open, "Open");
    open->setFrame(Rect{0.0, 0.0, 28.0, 28.0});
    open->setOnClick([&] { ++opened; });
    IconButton* openPtr = open.get();
    auto print = std::make_unique<IconButton>(Icon::Print, "Print");
    print->setFrame(Rect{0.0, 0.0, 28.0, 28.0});
    print->setOnClick([&] { ++printed; });
    IconButton* printPtr = print.get();
    toolbar.addItem(std::move(print), 8.0, ui::Toolbar::Group::Trailing);
    toolbar.addItem(std::move(open), 8.0, ui::Toolbar::Group::Leading);
    CHECK(printPtr->frame().minX() > openPtr->frame().minX());
    CHECK_NEAR(printPtr->frame().maxX(), 600.0 - ui::Toolbar::kPadding, 1e-9);

    toolbar.beginKeyboardNavigation();
    CHECK(openPtr->isFocused());
    CHECK(toolbar.handleKey(key(ui::Key::Tab)));
    CHECK(printPtr->isFocused());
    CHECK(!openPtr->isFocused());
    CHECK(toolbar.handleKey(key(ui::Key::Space)));
    CHECK_EQ(printed, 1);
    CHECK(toolbar.handleKey(key(ui::Key::Right))); // wraps
    CHECK(openPtr->isFocused());
    CHECK(toolbar.handleKey(key(ui::Key::Enter)));
    CHECK_EQ(opened, 1);
    CHECK(toolbar.handleKey(key(ui::Key::Escape)));
    CHECK(!toolbar.keyboardNavigating());
    CHECK(!openPtr->isFocused());
}
