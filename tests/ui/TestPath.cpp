// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Rect.hpp"
#include "ui/Path.hpp"

#include <cstddef>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::ui::Color;
using rivet::ui::LineCap;
using rivet::ui::LineJoin;
using rivet::ui::Path;
using rivet::ui::PathSegment;
using rivet::ui::testing::FakePaintContext;

RIVET_TEST(pathAddRectBuildsClosedFourSidedSubpath) {
    Path path;
    path.addRect(Rect{10.0, 20.0, 30.0, 40.0});
    CHECK_EQ(path.segments.size(), static_cast<std::size_t>(5));
    CHECK_EQ(path.segments[0].op, PathSegment::Op::MoveTo);
    CHECK_EQ(path.segments[2].p, (Point{40.0, 60.0}));
    CHECK_EQ(path.segments[4].op, PathSegment::Op::Close);
}

RIVET_TEST(pathAddEllipseUsesFourCubicsStartingAtRightmostPoint) {
    Path path;
    path.addEllipse(Rect{0.0, 0.0, 20.0, 10.0});
    CHECK_EQ(path.segments.size(), static_cast<std::size_t>(6));
    CHECK_EQ(path.segments[0].p, (Point{20.0, 5.0}));
    for (std::size_t i = 1; i <= 4; ++i) CHECK_EQ(path.segments[i].op, PathSegment::Op::CubicTo);
    // The last arc returns to the start point.
    CHECK_NEAR(path.segments[4].p.x, 20.0, 1e-9);
    CHECK_NEAR(path.segments[4].p.y, 5.0, 1e-9);
    // The bottom-most point of the second arc's end.
    CHECK_NEAR(path.segments[1].p.y, 10.0, 1e-9);
}

RIVET_TEST(fakePaintContextRecordsPathAndBoxTextCalls) {
    FakePaintContext context;
    Path path;
    path.moveTo({0.0, 0.0});
    path.lineTo({5.0, 5.0});
    path.cubicTo({6.0, 6.0}, {7.0, 7.0}, {8.0, 8.0});
    context.fillPath(path, Color::black());
    context.strokePath(path, Color::white(), 2.0, LineCap::Round, LineJoin::Round);
    context.drawTextInBox("OK", Rect{0.0, 0.0, 50.0, 20.0}, 1, rivet::ui::Font{}, Color::black());

    CHECK_EQ(context.paths.size(), static_cast<std::size_t>(2));
    CHECK_EQ(context.paths[0].stroke, false);
    CHECK_EQ(context.paths[1].stroke, true);
    CHECK_NEAR(context.paths[1].strokeWidth, 2.0, 1e-12);
    CHECK_EQ(context.paths[1].countOf(PathSegment::Op::CubicTo), static_cast<std::size_t>(1));
    CHECK_EQ(context.boxTexts.size(), static_cast<std::size_t>(1));
    CHECK_EQ(context.boxTexts[0].quarterTurns, 1);
}
