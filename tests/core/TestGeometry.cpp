#include "RivetTest.h"

#include "core/geometry/Insets.hpp"
#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"

#include <limits>

using namespace rivet::core;

static constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
static constexpr double kInf = std::numeric_limits<double>::infinity();

RIVET_TEST(pointArithmetic) {
    const Point a{1.0, 2.0};
    const Point b{3.0, -1.0};
    CHECK_EQ(a + b, (Point{4.0, 1.0}));
    CHECK_EQ(a - b, (Point{-2.0, 3.0}));
    CHECK_EQ(-a, (Point{-1.0, -2.0}));
    CHECK_EQ(a * 2.0, (Point{2.0, 4.0}));
    CHECK_EQ(3.0 * a, (Point{3.0, 6.0}));
    CHECK_EQ(b / 2.0, (Point{1.5, -0.5}));

    Point c = a;
    c += b;
    CHECK_EQ(c, (Point{4.0, 1.0}));

    CHECK((Point::nearlyEqual(a, Point{1.0 + 1e-12, 2.0})));
    CHECK((!Point::nearlyEqual(a, Point{1.1, 2.0})));
}

RIVET_TEST(pointFinite) {
    CHECK((Point{0.0, 0.0}.isFinite()));
    CHECK((!Point{kInf, 0.0}.isFinite()));
    CHECK((!Point{0.0, kNaN}.isFinite()));
}

RIVET_TEST(sizeBasics) {
    const Size s{100.0, 50.0};
    CHECK_EQ(s.area(), 5000.0);
    CHECK(!s.isEmpty());
    CHECK(Size{0.0, 10.0}.isEmpty());
    CHECK(Size{10.0, -1.0}.isEmpty());
    CHECK_EQ((s * 2.0), (Size{200.0, 100.0}));
    CHECK_EQ((s + Size{1.0, 1.0}), (Size{101.0, 51.0}));
    CHECK(Size{1.0, 1.0}.isFinite());
    CHECK(!Size{kNaN, 1.0}.isFinite());
}

RIVET_TEST(rectEdgesAndContains) {
    const Rect r{10.0, 20.0, 30.0, 40.0};
    CHECK_EQ(r.minX(), 10.0);
    CHECK_EQ(r.minY(), 20.0);
    CHECK_EQ(r.maxX(), 40.0);
    CHECK_EQ(r.maxY(), 60.0);
    CHECK_EQ(r.center(), (Point{25.0, 40.0}));
    CHECK(r.contains(Point{10.0, 20.0}));
    CHECK(r.contains(Point{40.0, 60.0}));
    CHECK(r.contains(Point{25.0, 40.0}));
    CHECK(!r.contains(Point{9.9, 40.0}));
    CHECK(!r.contains(Point{40.1, 40.0}));
    CHECK(Rect{}.isEmpty());
}

RIVET_TEST(rectIntersectionAndUnion) {
    const Rect a{0.0, 0.0, 10.0, 10.0};
    const Rect b{5.0, 5.0, 10.0, 10.0};
    CHECK(a.intersects(b));
    CHECK((Rect::nearlyEqual(a.intersection(b), Rect{5.0, 5.0, 5.0, 5.0})));

    const Rect c{20.0, 0.0, 5.0, 5.0};
    CHECK(!a.intersects(c));
    CHECK(a.intersection(c).isEmpty());

    const Rect u = a.united(Rect{15.0, 15.0, 5.0, 5.0});
    CHECK((Rect::nearlyEqual(u, Rect{0.0, 0.0, 20.0, 20.0})));
}

RIVET_TEST(rectInsetAndTranslate) {
    const Rect r{10.0, 10.0, 30.0, 20.0};
    const Rect inset = r.inset(Insets::uniform(5.0));
    CHECK((Rect::nearlyEqual(inset, Rect{15.0, 15.0, 20.0, 10.0})));

    // Over-large insets clamp to an empty rect, not a negative one.
    const Rect clamped = r.inset(Insets::uniform(100.0));
    CHECK(clamped.isEmpty());
    CHECK_GE(clamped.size.width, 0.0);

    const Rect moved = r.translated(Point{5.0, -5.0});
    CHECK((Rect::nearlyEqual(moved, Rect{15.0, 5.0, 30.0, 20.0})));
    CHECK((Rect::nearlyEqual(r + Point{5.0, -5.0}, moved)));
}

RIVET_TEST(rectContainsRect) {
    const Rect outer{0.0, 0.0, 100.0, 100.0};
    CHECK(outer.containsRect(Rect{10.0, 10.0, 20.0, 20.0}));
    CHECK(!outer.containsRect(Rect{10.0, 10.0, 95.0, 20.0}));
    CHECK(outer.containsRect(Rect{}));
}

RIVET_TEST(insets) {
    const Insets i{1.0, 2.0, 3.0, 4.0};
    CHECK_EQ(i.horizontal(), 4.0);
    CHECK_EQ(i.vertical(), 6.0);
    CHECK_EQ(Insets::uniform(2.0), (Insets{2.0, 2.0, 2.0, 2.0}));
    CHECK_EQ(Insets::horizontal(2.0), (Insets{2.0, 0.0, 2.0, 0.0}));
}

RIVET_TEST(matrixComposition) {
    // (A * B).map(p) == A.map(B.map(p))
    const Matrix t = Matrix::translation(10.0, 0.0);
    const Matrix s = Matrix::scaling(2.0, 2.0);
    const Point p{1.0, 1.0};
    CHECK((Point::nearlyEqual((t * s).map(p), t.map(s.map(p)))));
    // scale then translate: (1,1) -> (2,2) -> (12,2)
    CHECK((Point::nearlyEqual((t * s).map(p), Point{12.0, 2.0})));
    // translate then scale: (1,1) -> (11,1) -> (22,2)
    CHECK((Point::nearlyEqual((s * t).map(p), Point{22.0, 2.0})));
}

RIVET_TEST(matrixInverseRoundtrip) {
    const Matrix m = Matrix::translation(5.0, -3.0) * Matrix::scaling(2.0, 4.0);
    const auto inv = m.inverted();
    CHECK(inv.has_value());
    const Point p{7.0, 9.0};
    CHECK((Point::nearlyEqual(inv->map(m.map(p)), p, 1e-9)));
    CHECK((Point::nearlyEqual(m.map(inv->map(p)), p, 1e-9)));
}

RIVET_TEST(matrixSingularHasNoInverse) {
    const Matrix singular = Matrix::scaling(0.0, 1.0);
    CHECK(!singular.inverted().has_value());
}

RIVET_TEST(matrixMapRectBounds) {
    const Rect r{0.0, 0.0, 10.0, 10.0};
    const Rect mapped = Matrix::rotation(M_PI / 2.0).mapRect(r);
    // Rotating the unit square 90 degrees: x in [-10, 0], y in [0, 10].
    CHECK_NEAR(mapped.minX(), -10.0, 1e-9);
    CHECK_NEAR(mapped.maxX(), 0.0, 1e-9);
    CHECK_NEAR(mapped.minY(), 0.0, 1e-9);
    CHECK_NEAR(mapped.maxY(), 10.0, 1e-9);
}

RIVET_TEST(rotationArithmetic) {
    CHECK_EQ(rotationDegrees(PageRotation::None), 0);
    CHECK_EQ(rotationDegrees(PageRotation::Clockwise270), 270);
    CHECK_EQ(addRotation(PageRotation::Clockwise90, PageRotation::Clockwise90), PageRotation::Clockwise180);
    CHECK_EQ(addRotation(PageRotation::Clockwise270, PageRotation::Clockwise90), PageRotation::None);
    CHECK_EQ(rotationFromQuarterTurns(5), PageRotation::Clockwise90);
    CHECK_EQ(rotationFromQuarterTurns(-1), PageRotation::Clockwise270);
}
