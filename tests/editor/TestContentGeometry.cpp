// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "editor/ContentGeometry.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <array>
#include <cmath>
#include <numbers>

// The user <-> display mapping of content objects with exact numeric
// expectations through every page /Rotate, a CropBox offset, skewed and
// nested object matrices.

using namespace rivet;
using namespace rivet::editor;
using core::Matrix;
using core::PageRotation;
using core::Point;
using core::Rect;

namespace {

constexpr double kEps = 1e-9;
const std::array<PageRotation, 4> kRotations{PageRotation::None, PageRotation::Clockwise90,
                                             PageRotation::Clockwise180, PageRotation::Clockwise270};

pdf::PdfPageView viewOf(PageRotation rotation, pdf::PdfBox crop = {0.0, 0.0, 600.0, 800.0}) {
    return pdf::PdfPageView{rotation, crop};
}

bool near(Point a, Point b, double eps = kEps) {
    return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps;
}

} // namespace

RIVET_TEST(ContentGeometry_translation_maps_through_every_rotation) {
    // Object at user (10, 20) on a 600x800 page.
    const Matrix m = Matrix::translation(10.0, 20.0);
    const Point expected[4] = {{10.0, 780.0}, {20.0, 10.0}, {590.0, 20.0}, {780.0, 590.0}};
    for (std::size_t i = 0; i < 4; ++i) {
        const Matrix display = geometry::objectToDisplay(viewOf(kRotations[i]), m);
        CHECK(near(display.map(Point{0.0, 0.0}), expected[i]));
    }
}

RIVET_TEST(ContentGeometry_crop_box_offset) {
    const pdf::PdfPageView view = viewOf(PageRotation::None, {50.0, 100.0, 650.0, 900.0});
    const Matrix display = geometry::objectToDisplay(view, Matrix::translation(60.0, 150.0));
    CHECK(near(display.map(Point{0.0, 0.0}), Point{10.0, 750.0}));
    const core::Rect box = geometry::boxToDisplay(view, pdf::PdfBox{60.0, 150.0, 160.0, 200.0});
    CHECK_NEAR(box.minX(), 10.0, kEps);
    CHECK_NEAR(box.minY(), 700.0, kEps);
    CHECK_NEAR(box.size.width, 100.0, kEps);
    CHECK_NEAR(box.size.height, 50.0, kEps);
}

RIVET_TEST(ContentGeometry_box_to_display_rotated) {
    // user box (10,20)-(110,60) on 600x800, rotated 90: display (y, x).
    const Rect box = geometry::boxToDisplay(viewOf(PageRotation::Clockwise90), pdf::PdfBox{10.0, 20.0, 110.0, 60.0});
    CHECK_NEAR(box.minX(), 20.0, kEps);
    CHECK_NEAR(box.minY(), 10.0, kEps);
    CHECK_NEAR(box.size.width, 40.0, kEps);
    CHECK_NEAR(box.size.height, 100.0, kEps);
}

RIVET_TEST(ContentGeometry_pointer_delta_maps_back_with_the_inverse_linear_part) {
    const Point delta{5.0, 7.0};
    const Point expected[4] = {{5.0, -7.0}, {7.0, 5.0}, {-5.0, 7.0}, {-7.0, -5.0}};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto t = geometry::userTranslationForDisplayDelta(viewOf(kRotations[i]), delta);
        CHECK(t.has_value());
        CHECK_NEAR(t->tx, expected[i].x, kEps);
        CHECK_NEAR(t->ty, expected[i].y, kEps);
        CHECK_NEAR(t->a, 1.0, kEps);
        CHECK_NEAR(t->d, 1.0, kEps);
    }
    CHECK(!geometry::userTranslationForDisplayDelta(viewOf(PageRotation::None), Point{std::nan(""), 0.0}).has_value());
}

RIVET_TEST(ContentGeometry_rect_transform_lands_on_the_target_display_rect) {
    const pdf::PdfBox userBox{100.0, 200.0, 220.0, 260.0};
    const Rect to{Point{30.0, 40.0}, core::Size{240.0, 30.0}};
    for (PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation, {20.0, 30.0, 620.0, 830.0});
        const Rect from = geometry::boxToDisplay(view, userBox);
        const auto t = geometry::userTransformForDisplayRects(view, from, to);
        CHECK(t.has_value());
        // Map the user box corners through T, then to display.
        double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
        const double xs[2] = {userBox.left, userBox.right};
        const double ys[2] = {userBox.bottom, userBox.top};
        for (double x : xs) {
            for (double y : ys) {
                const Point u = t->map(Point{x, y});
                const Point d = pdf::userToDisplay(view, u.x, u.y);
                minX = std::min(minX, d.x);
                minY = std::min(minY, d.y);
                maxX = std::max(maxX, d.x);
                maxY = std::max(maxY, d.y);
            }
        }
        CHECK_NEAR(minX, to.minX(), 1e-7);
        CHECK_NEAR(minY, to.minY(), 1e-7);
        CHECK_NEAR(maxX - minX, to.size.width, 1e-7);
        CHECK_NEAR(maxY - minY, to.size.height, 1e-7);
    }
    const pdf::PdfPageView view = viewOf(PageRotation::None);
    CHECK(!geometry::userTransformForDisplayRects(view, Rect{}, to).has_value());
    CHECK(!geometry::userTransformForDisplayRects(view, to, Rect{}).has_value());
}

RIVET_TEST(ContentGeometry_scale_is_exact_without_rotation) {
    const pdf::PdfPageView view = viewOf(PageRotation::None);
    const Rect from{Point{10.0, 10.0}, core::Size{100.0, 50.0}};
    const Rect to{Point{10.0, 10.0}, core::Size{200.0, 100.0}};
    const auto t = geometry::userTransformForDisplayRects(view, from, to);
    CHECK(t.has_value());
    CHECK_NEAR(t->a, 2.0, kEps);
    CHECK_NEAR(t->d, 2.0, kEps);
    CHECK_NEAR(t->b, 0.0, kEps);
    CHECK_NEAR(t->c, 0.0, kEps);
    // Display anchor (10,10) stays: user (10, 790) -> (10, 790 - 2*? ): top-left anchored in display.
    const Point anchor = pdf::userToDisplay(view, t->map(Point{10.0, 790.0}).x, t->map(Point{10.0, 790.0}).y);
    CHECK(near(anchor, Point{10.0, 10.0}));
}

RIVET_TEST(ContentGeometry_display_angle_of_the_local_x_axis) {
    const Matrix identity = Matrix::identity();
    CHECK_NEAR(geometry::displayAngleDegrees(viewOf(PageRotation::None), identity), 0.0, kEps);
    CHECK_NEAR(geometry::displayAngleDegrees(viewOf(PageRotation::Clockwise90), identity), 90.0, kEps);
    CHECK_NEAR(geometry::displayAngleDegrees(viewOf(PageRotation::Clockwise180), identity), 180.0, kEps);
    CHECK_NEAR(geometry::displayAngleDegrees(viewOf(PageRotation::Clockwise270), identity), -90.0, kEps);
    // 90 degrees counter-clockwise in user space shows as -90 on an unrotated page.
    const Matrix up{0.0, 1.0, -1.0, 0.0, 0.0, 0.0};
    CHECK_NEAR(geometry::displayAngleDegrees(viewOf(PageRotation::None), up), -90.0, kEps);
    CHECK_NEAR(geometry::userAngleRadians(up), std::numbers::pi / 2.0, kEps);
    // A page rotated 90 clockwise turns the same text upright again... after a quarter turn back.
    CHECK_NEAR(geometry::displayAngleDegrees(viewOf(PageRotation::Clockwise90), up), 0.0, kEps);
}

RIVET_TEST(ContentGeometry_skew_is_neither_rigid_nor_similar) {
    const Matrix skew{1.0, 0.0, 0.5, 1.0, 0.0, 0.0};
    CHECK(!geometry::isRigid(skew));
    CHECK(!geometry::isSimilarity(skew));
    const Matrix stretch{2.0, 0.0, 0.0, 1.0, 0.0, 0.0};
    CHECK(!geometry::isSimilarity(stretch));
    const Matrix mirror{1.0, 0.0, 0.0, -1.0, 0.0, 0.0};
    CHECK(!geometry::isSimilarity(mirror));
    const Matrix scaled = Matrix::scaling(3.0, 3.0);
    CHECK(geometry::isSimilarity(scaled));
    CHECK(!geometry::isRigid(scaled));
    const Matrix rotated = Matrix::rotation(0.3) * Matrix::translation(5.0, 6.0);
    CHECK(geometry::isRigid(rotated));
    CHECK(geometry::isSimilarity(rotated));
    CHECK(!geometry::isRigid(Matrix{std::nan(""), 0.0, 0.0, 1.0, 0.0, 0.0}));
}

RIVET_TEST(ContentGeometry_skewed_object_quad_maps_exactly) {
    // A unit square sheared in x by 0.5, translated to (100, 200), on a 90 degree page.
    const Matrix m{20.0, 0.0, 10.0, 20.0, 100.0, 200.0};
    const pdf::PdfPageView view = viewOf(PageRotation::Clockwise90);
    const Matrix display = geometry::objectToDisplay(view, m);
    // Local (1,1) -> user (130, 220) -> display (y, x) = (220, 130).
    CHECK(near(display.map(Point{1.0, 1.0}), Point{220.0, 130.0}));
    CHECK(near(display.map(Point{1.0, 0.0}), Point{200.0, 120.0}));
}

RIVET_TEST(ContentGeometry_nested_matrices_compose) {
    const Matrix inner = Matrix::translation(10.0, 5.0);
    const Matrix outer = Matrix::rotation(std::numbers::pi / 2.0) * Matrix::translation(100.0, 0.0);
    const Matrix nested = outer * inner;
    for (PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation);
        const Point local{3.0, 4.0};
        const Point viaMatrix = geometry::objectToDisplay(view, nested).map(local);
        const Point step = inner.map(local);
        const Point user = outer.map(step);
        const Point direct = pdf::userToDisplay(view, user.x, user.y);
        CHECK(near(viaMatrix, direct, 1e-9));
    }
}

RIVET_TEST(ContentGeometry_quad_is_canonical_clockwise_from_the_top_left_vertex) {
    const pdf::PdfPageView view = viewOf(PageRotation::None);
    const std::array<pdf::PdfPoint, 4> userQuad{pdf::PdfPoint{10.0, 20.0}, pdf::PdfPoint{110.0, 20.0},
                                                pdf::PdfPoint{110.0, 60.0}, pdf::PdfPoint{10.0, 60.0}};
    const auto quad = geometry::quadToDisplay(view, userQuad);
    CHECK(near(quad[0], Point{10.0, 740.0}));
    CHECK(near(quad[1], Point{110.0, 740.0}));
    CHECK(near(quad[2], Point{110.0, 780.0}));
    CHECK(near(quad[3], Point{10.0, 780.0}));
    // Any input order gives the same result.
    const std::array<pdf::PdfPoint, 4> shuffled{userQuad[2], userQuad[0], userQuad[3], userQuad[1]};
    const auto again = geometry::quadToDisplay(view, shuffled);
    for (std::size_t i = 0; i < 4; ++i) CHECK(near(quad[i], again[i]));
}

RIVET_TEST(ContentGeometry_upright_placement_reads_upright_on_every_rotated_page) {
    const Point origin{120.0, 340.0};
    for (PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation, {10.0, 20.0, 610.0, 820.0});
        const Matrix placement = geometry::uprightPlacement(view, origin);
        CHECK(geometry::isRigid(placement, 1e-9));
        const Matrix toDisplay = pdf::userToDisplayMatrix(view) * placement;
        CHECK(near(toDisplay.map(Point{0.0, 0.0}), origin));
        CHECK(near(toDisplay.map(Point{1.0, 0.0}), Point{origin.x + 1.0, origin.y}));
        // The block frame's +y (up) reads as screen-up.
        CHECK(near(toDisplay.map(Point{0.0, 1.0}), Point{origin.x, origin.y - 1.0}));
    }
}

RIVET_TEST(ContentGeometry_point_in_quad_with_tolerance) {
    // A square rotated 45 degrees around (50, 50), half-diagonal 40.
    const std::array<Point, 4> quad{Point{50.0, 10.0}, Point{90.0, 50.0}, Point{50.0, 90.0}, Point{10.0, 50.0}};
    CHECK(geometry::pointInConvexQuad(Point{50.0, 50.0}, quad, 0.0));
    CHECK(geometry::pointInConvexQuad(Point{80.0, 50.0}, quad, 0.0));
    CHECK(!geometry::pointInConvexQuad(Point{85.0, 85.0}, quad, 0.0));
    // 3 points outside the right-top edge, within tolerance 5 only.
    CHECK(geometry::pointInConvexQuad(Point{72.0, 72.0}, quad, 5.0));
    CHECK(!geometry::pointInConvexQuad(Point{72.0, 72.0}, quad, 0.5));
    // Vertex order does not matter.
    const std::array<Point, 4> shuffled{quad[2], quad[0], quad[3], quad[1]};
    CHECK(geometry::pointInConvexQuad(Point{50.0, 50.0}, shuffled, 0.0));
    CHECK(!geometry::pointInConvexQuad(Point{200.0, 200.0}, shuffled, 5.0));
}
