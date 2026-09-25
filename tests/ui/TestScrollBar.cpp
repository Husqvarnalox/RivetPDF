// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/UiTypes.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::ScrollBar;
using rivet::ui::ScrollOrientation;
using rivet::ui::testing::FakePaintContext;

namespace {

PointerEvent makeEvent(PointerEventType type, Point position, int button = 1) {
    PointerEvent event;
    event.type = type;
    event.position = position;
    event.button = button;
    return event;
}

// Vertical bar 12x100 with viewport 100 / content 300: track runs from y=2
// for 96 points, thumb is 32 points long (96 * 100/300).
void configureVerticalBar(ScrollBar& bar) {
    bar.setFrame(Rect{0.0, 0.0, 12.0, 100.0});
    bar.setExtents(100.0, 300.0);
}

} // namespace

RIVET_TEST(hiddenWhenContentFitsViewport) {
    ScrollBar bar(ScrollOrientation::Vertical);
    bar.setFrame(Rect{0.0, 0.0, 12.0, 100.0});

    bar.setExtents(100.0, 100.0);
    CHECK_EQ(bar.isUsable(), false);
    CHECK_NEAR(bar.maxOffset(), 0.0, 1e-9);

    bar.setExtents(100.0, 80.0); // content < viewport
    CHECK_EQ(bar.isUsable(), false);

    bar.setExtents(100.0, 300.0);
    CHECK_EQ(bar.isUsable(), true);
    CHECK_NEAR(bar.maxOffset(), 200.0, 1e-9);
}

RIVET_TEST(setOffsetClampsToValidRange) {
    ScrollBar bar(ScrollOrientation::Vertical);
    configureVerticalBar(bar);

    bar.setOffset(-50.0);
    CHECK_NEAR(bar.offset(), 0.0, 1e-9);

    bar.setOffset(500.0); // beyond maxOffset 200
    CHECK_NEAR(bar.offset(), 200.0, 1e-9);

    bar.setOffset(120.0);
    CHECK_NEAR(bar.offset(), 120.0, 1e-9);
}

RIVET_TEST(thumbGeometryIsProportional) {
    ScrollBar bar(ScrollOrientation::Vertical);
    configureVerticalBar(bar);

    const Rect track = bar.trackRect();
    CHECK_NEAR(track.minY(), ScrollBar::kTrackPadding, 1e-9);
    CHECK_NEAR(track.size.height, 96.0, 1e-9);

    // Thumb length: 96 * (100/300) = 32.
    bar.setOffset(0.0);
    Rect thumb = bar.thumbRect();
    CHECK_NEAR(thumb.size.height, 32.0, 1e-9);
    CHECK_NEAR(thumb.minY(), 2.0, 1e-9);

    bar.setOffset(100.0); // mid: thumb starts halfway along the free span
    thumb = bar.thumbRect();
    CHECK_NEAR(thumb.minY(), 34.0, 1e-9);
    CHECK_NEAR(thumb.size.height, 32.0, 1e-9);

    bar.setOffset(200.0); // max
    thumb = bar.thumbRect();
    CHECK_NEAR(thumb.minY(), 66.0, 1e-9);
    CHECK_NEAR(thumb.maxY(), 98.0, 1e-9);
}

RIVET_TEST(thumbDragFiresScrollWithClampedOffsets) {
    ScrollBar bar(ScrollOrientation::Vertical);
    configureVerticalBar(bar);
    bar.setOffset(100.0); // thumb spans y 34..66

    std::vector<double> offsets;
    bar.setOnScroll([&offsets](double offset) { offsets.push_back(offset); });

    // Down 6 points into the thumb (grab offset 6): no scroll yet.
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Down, Point{6.0, 40.0})), true);
    CHECK_EQ(offsets.size(), std::size_t{0});

    // Move down 10 points: thumb start 44 -> offset (44-2)/(96-32) * 200.
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Move, Point{6.0, 50.0})), true);
    CHECK_EQ(offsets.size(), std::size_t{1});
    CHECK_NEAR(offsets.back(), 131.25, 1e-9);
    CHECK_NEAR(bar.offset(), 131.25, 1e-9);

    // Up ends the drag; later moves neither consume nor fire.
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Up, Point{6.0, 50.0})), true);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Move, Point{6.0, 90.0})), false);
    CHECK_EQ(offsets.size(), std::size_t{1});

    // Dragging above the track clamps to 0.
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Down, Point{6.0, 40.0})), true);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Move, Point{6.0, -30.0})), true);
    CHECK_NEAR(bar.offset(), 0.0, 1e-9);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Up, Point{6.0, -30.0})), true);
}

RIVET_TEST(trackClickCentersThumbUnderPointer) {
    ScrollBar bar(ScrollOrientation::Vertical);
    configureVerticalBar(bar); // offset 0: thumb spans y 2..34

    std::vector<double> offsets;
    bar.setOnScroll([&offsets](double offset) { offsets.push_back(offset); });

    // Click below the thumb: offset chosen so the thumb center lands on y=50.
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Down, Point{6.0, 50.0})), true);
    CHECK_EQ(offsets.size(), std::size_t{1});
    CHECK_NEAR(offsets.back(), 100.0, 1e-9);
    CHECK_NEAR(bar.thumbRect().center().y, 50.0, 1e-9);

    // The drag continues centered: a zero-pixel move is a no-op.
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Move, Point{6.0, 50.0})), true);
    CHECK_EQ(offsets.size(), std::size_t{1});
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Up, Point{6.0, 50.0})), true);

    // Click above the thumb clamps to offset 0.
    bar.setOffset(200.0);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Down, Point{6.0, 10.0})), true);
    CHECK_EQ(offsets.size(), std::size_t{2});
    CHECK_NEAR(offsets.back(), 0.0, 1e-9);
    CHECK_NEAR(bar.thumbRect().center().y, 18.0, 1e-9);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Up, Point{6.0, 10.0})), true);
}

RIVET_TEST(notUsableConsumesNothingAndPaintsNothing) {
    ScrollBar bar(ScrollOrientation::Vertical);
    bar.setFrame(Rect{0.0, 0.0, 12.0, 100.0});
    bar.setExtents(100.0, 100.0); // not usable

    int scrolls = 0;
    bar.setOnScroll([&scrolls](double) { ++scrolls; });

    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Down, Point{6.0, 50.0})), false);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Move, Point{6.0, 50.0})), false);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Up, Point{6.0, 50.0})), false);
    CHECK_EQ(scrolls, 0);

    FakePaintContext context;
    bar.paint(context);
    CHECK_EQ(context.roundedFills.size(), std::size_t{0});

    // Usable again: track + thumb painted.
    bar.setExtents(100.0, 300.0);
    bar.paint(context);
    CHECK_EQ(context.roundedFills.size(), std::size_t{2});
}

RIVET_TEST(nonFiniteAndNegativeExtentsAreSanitized) {
    ScrollBar bar(ScrollOrientation::Vertical);
    configureVerticalBar(bar);
    bar.setOffset(50.0);

    // Non-finite offsets keep the current offset.
    bar.setOffset(std::nan(""));
    CHECK_NEAR(bar.offset(), 50.0, 1e-9);
    bar.setOffset(HUGE_VAL);
    CHECK_NEAR(bar.offset(), 50.0, 1e-9);

    // Invalid extents leave the state untouched.
    bar.setExtents(std::nan(""), 300.0);
    CHECK_NEAR(bar.viewportExtent(), 100.0, 1e-9);
    CHECK_NEAR(bar.contentExtent(), 300.0, 1e-9);
    CHECK_EQ(bar.isUsable(), true);
    bar.setExtents(100.0, -300.0);
    CHECK_NEAR(bar.contentExtent(), 300.0, 1e-9);
    bar.setExtents(-100.0, 300.0);
    CHECK_NEAR(bar.viewportExtent(), 100.0, 1e-9);

    // No crash when painting the sanitized state.
    FakePaintContext context;
    bar.paint(context);
    CHECK_EQ(context.roundedFills.size(), std::size_t{2});
}

RIVET_TEST(horizontalOrientationUsesXAxis) {
    ScrollBar bar(ScrollOrientation::Horizontal);
    bar.setFrame(Rect{0.0, 0.0, 100.0, 12.0});
    bar.setExtents(100.0, 300.0);
    bar.setOffset(100.0); // thumb spans x 34..66

    std::vector<double> offsets;
    bar.setOnScroll([&offsets](double offset) { offsets.push_back(offset); });

    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Down, Point{40.0, 6.0})), true);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Move, Point{50.0, 6.0})), true);
    CHECK_EQ(offsets.size(), std::size_t{1});
    CHECK_NEAR(offsets.back(), 131.25, 1e-9);
    CHECK_EQ(bar.onMouse(makeEvent(PointerEventType::Up, Point{50.0, 6.0})), true);
}

RIVET_TEST(preferredSizeHintsThicknessOnCrossAxis) {
    ScrollBar vertical(ScrollOrientation::Vertical);
    vertical.setFrame(Rect{0.0, 0.0, 12.0, 100.0});
    const Size verticalSize = vertical.preferredSize(FakePaintContext{});
    CHECK_NEAR(verticalSize.width, ScrollBar::kThickness, 1e-9);
    CHECK_NEAR(verticalSize.height, 100.0, 1e-9);

    ScrollBar horizontal(ScrollOrientation::Horizontal);
    horizontal.setFrame(Rect{0.0, 0.0, 100.0, 12.0});
    const Size horizontalSize = horizontal.preferredSize(FakePaintContext{});
    CHECK_NEAR(horizontalSize.width, 100.0, 1e-9);
    CHECK_NEAR(horizontalSize.height, ScrollBar::kThickness, 1e-9);
}
