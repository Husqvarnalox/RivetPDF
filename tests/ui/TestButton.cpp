#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/Button.hpp"
#include "ui/UiTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::ui::Button;
using rivet::ui::Color;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::testing::CountingRedrawSink;
using rivet::ui::testing::FakePaintContext;

namespace {

PointerEvent makeEvent(PointerEventType type, Point position, int button = 1) {
    PointerEvent event;
    event.type = type;
    event.position = position;
    event.button = button;
    return event;
}

bool sameColor(const Color& a, const Color& b) {
    return std::fabs(a.r - b.r) <= 1e-9 && std::fabs(a.g - b.g) <= 1e-9 &&
           std::fabs(a.b - b.b) <= 1e-9 && std::fabs(a.a - b.a) <= 1e-9;
}

// Matches the fill constants used by Button::paintSelf.
constexpr Color kNormalFill = Color::rgba(231.0 / 255.0, 231.0 / 255.0, 231.0 / 255.0, 1.0); // #E7E7E7
constexpr Color kHoverFill = Color::rgba(220.0 / 255.0, 220.0 / 255.0, 220.0 / 255.0, 1.0); // #DCDCDC
constexpr Color kPressedFill = Color::rgba(201.0 / 255.0, 201.0 / 255.0, 201.0 / 255.0, 1.0); // #C9C9C9

} // namespace

RIVET_TEST(clickFiresOnMouseUpInside) {
    Button button("OK");
    button.setFrame(Rect{0.0, 0.0, 80.0, 28.0});

    int clicks = 0;
    button.setOnClick([&clicks] { ++clicks; });

    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Down, Point{10.0, 10.0})), true);
    CHECK_EQ(clicks, 0);

    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{10.0, 10.0})), true);
    CHECK_EQ(clicks, 1);

    // A stray Up without a matching Down must not fire again.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{10.0, 10.0})), true);
    CHECK_EQ(clicks, 1);
}

RIVET_TEST(draggingOutCancelsPendingClick) {
    Button button("Go");
    button.setFrame(Rect{0.0, 0.0, 80.0, 28.0});

    int clicks = 0;
    button.setOnClick([&clicks] { ++clicks; });

    // Down inside, released outside (mouse-capture style delivery straight
    // to the button): no click, and the press is cleared.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Down, Point{10.0, 10.0})), true);
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{500.0, 500.0})), true);
    CHECK_EQ(clicks, 0);

    // The stale Up no longer counts as a press completion.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{10.0, 10.0})), true);
    CHECK_EQ(clicks, 0);

    // A fresh press/click still works afterwards.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Down, Point{10.0, 10.0})), true);
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{10.0, 10.0})), true);
    CHECK_EQ(clicks, 1);
}

RIVET_TEST(downsOutsideTheButtonAreIgnored) {
    Button button("Go");
    button.setFrame(Rect{0.0, 0.0, 80.0, 28.0});

    int clicks = 0;
    button.setOnClick([&clicks] { ++clicks; });

    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Down, Point{100.0, 100.0})), false);
    // Up inside without an armed press: consumed but no click.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{10.0, 10.0})), true);
    CHECK_EQ(clicks, 0);
}

RIVET_TEST(movesWithNoButtonDownDisarmThePress) {
    Button button("Go");
    button.setFrame(Rect{0.0, 0.0, 80.0, 28.0});

    int clicks = 0;
    button.setOnClick([&clicks] { ++clicks; });

    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Down, Point{10.0, 10.0})), true);
    // Button-less move while armed: the matching Up was lost elsewhere.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Move, Point{10.0, 10.0}, 0)), true);
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{10.0, 10.0})), true);
    CHECK_EQ(clicks, 0);
}

RIVET_TEST(buttonPaintsPressAndHoverStates) {
    Button button("Go");
    button.setFrame(Rect{0.0, 0.0, 80.0, 28.0});
    FakePaintContext context;

    // Normal: filled rounded rect, subtle border, centered label.
    button.paint(context);
    CHECK_EQ(context.roundedFills.size(), std::size_t{1});
    CHECK(sameColor(context.roundedFills.back().color, kNormalFill));
    CHECK_EQ(context.strokes.size(), std::size_t{1});
    CHECK_EQ(context.texts.size(), std::size_t{1});
    CHECK_EQ(context.texts.back().text, "Go");
    CHECK(context.texts.back().align == rivet::ui::TextAlign::Center);

    // Hovered after a move inside.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Move, Point{40.0, 14.0})), true);
    button.paint(context);
    CHECK(sameColor(context.roundedFills.back().color, kHoverFill));

    // Pressed while armed and hovered.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Down, Point{40.0, 14.0})), true);
    button.paint(context);
    CHECK(sameColor(context.roundedFills.back().color, kPressedFill));

    // Released: back to normal via hover.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Up, Point{40.0, 14.0})), true);
    button.paint(context);
    CHECK(sameColor(context.roundedFills.back().color, kHoverFill));

    // Pointer left again: normal fill.
    CHECK_EQ(button.onMouse(makeEvent(PointerEventType::Move, Point{200.0, 200.0})), true);
    button.paint(context);
    CHECK(sameColor(context.roundedFills.back().color, kNormalFill));
}

RIVET_TEST(preferredSizeUsesMeasureText) {
    FakePaintContext context;
    Button button("OK"); // 2 chars -> 14 px wide, 13 px tall in the fake

    const Size size = button.preferredSize(context);
    CHECK_NEAR(size.width, 14.0 + 2.0 * Button::kHorizontalPadding, 1e-9);
    CHECK_NEAR(size.height, std::max(13.0 + 2.0 * Button::kVerticalPadding, Button::kMinHeight), 1e-9);
}

RIVET_TEST(labelChangesInvalidate) {
    Button button("Start");
    CountingRedrawSink sink;
    button.setRedrawSink(&sink);

    CHECK_EQ(button.label(), "Start");
    CHECK_EQ(sink.count, 0);

    button.setLabel("Stop");
    CHECK_EQ(button.label(), "Stop");
    CHECK_EQ(sink.count, 1);

    // Unchanged label: no invalidation.
    button.setLabel("Stop");
    CHECK_EQ(sink.count, 1);
}
