// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "editor/AnnotationGeometry.hpp"
#include "editor/Annotations.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <array>
#include <cmath>
#include <vector>

// Pure annotation geometry: hit-test primitives, Ramer-Douglas-Peucker, the
// user <-> display mapping through every rotation and a cropped view, and
// per-kind hit testing of resolved views.

using namespace rivet;
using namespace rivet::editor;
using core::PageRotation;
using core::Point;
using core::Rect;

namespace {

pdf::PdfPageView viewOf(PageRotation rotation, pdf::PdfBox crop = {0.0, 0.0, 600.0, 800.0}) {
    return pdf::PdfPageView{rotation, crop};
}

const PageRotation kRotations[] = {PageRotation::None, PageRotation::Clockwise90, PageRotation::Clockwise180,
                                   PageRotation::Clockwise270};

pdf::PdfAnnotationData squareData() {
    pdf::PdfAnnotationData d;
    d.kind = pdf::PdfAnnotationKind::Square;
    d.rect = pdf::PdfBox{100.0, 200.0, 220.0, 260.0};
    d.borderWidth = 4.0F;
    return d;
}

} // namespace

RIVET_TEST(annotationGeometryDistanceToSegment) {
    using geometry::distanceToSegment;
    CHECK_NEAR(distanceToSegment({5, 5}, {0, 0}, {10, 0}), 5.0, 1e-9);
    CHECK_NEAR(distanceToSegment({-3, 4}, {0, 0}, {10, 0}), 5.0, 1e-9); // clamped to the start
    CHECK_NEAR(distanceToSegment({13, 4}, {0, 0}, {10, 0}), 5.0, 1e-9); // clamped to the end
    CHECK_NEAR(distanceToSegment({3, 4}, {0, 0}, {0, 0}), 5.0, 1e-9);   // degenerate = point
    CHECK_NEAR(distanceToSegment({4, 0}, {0, 0}, {10, 0}), 0.0, 1e-9);
}

RIVET_TEST(annotationGeometryPointInQuad) {
    // PDFium order TL, TR, BL, BR (display, y down).
    const DisplayQuad quad{Point{10, 10}, Point{50, 10}, Point{10, 30}, Point{50, 30}};
    CHECK(geometry::pointInQuad({30, 20}, quad));
    CHECK(!geometry::pointInQuad({55, 20}, quad));
    CHECK(geometry::pointInQuad({53, 20}, quad, 4.0));
    CHECK(!geometry::pointInQuad({60, 20}, quad, 4.0));
    // A slanted run (parallelogram).
    const DisplayQuad slanted{Point{20, 10}, Point{60, 10}, Point{10, 30}, Point{50, 30}};
    CHECK(geometry::pointInQuad({35, 20}, slanted));
    CHECK(!geometry::pointInQuad({15, 12}, slanted));
}

RIVET_TEST(annotationGeometryOutlineDistances) {
    const Rect r{10, 10, 100, 50};
    CHECK_NEAR(geometry::distanceToRectOutline({10, 30}, r), 0.0, 1e-9);
    CHECK_NEAR(geometry::distanceToRectOutline({60, 30}, r), 20.0, 1e-9); // inside: to the nearest edge
    CHECK_NEAR(geometry::distanceToRectOutline({0, 30}, r), 10.0, 1e-9);
    CHECK_NEAR(geometry::distanceToRectOutline({0, 0}, r), std::hypot(10.0, 10.0), 1e-9);

    const Rect e{0, 0, 200, 100}; // ellipse centred (100, 50), radii 100 x 50
    CHECK_NEAR(geometry::distanceToEllipseOutline({200, 50}, e), 0.0, 1e-6);
    CHECK_NEAR(geometry::distanceToEllipseOutline({100, 0}, e), 0.0, 1e-6);
    CHECK_NEAR(geometry::distanceToEllipseOutline({210, 50}, e), 10.0, 1e-6);
    CHECK_NEAR(geometry::distanceToEllipseOutline({100, 50}, e), 50.0, 1e-6); // centre: the nearer radius
    const double diagonal = geometry::distanceToEllipseOutline({200, 100}, e);
    CHECK(diagonal > 0.0 && diagonal < 60.0);
    // Degenerate ellipse = its segment.
    CHECK_NEAR(geometry::distanceToEllipseOutline({50, 5}, Rect{0, 0, 100, 0}), 5.0, 1e-9);
}

RIVET_TEST(annotationGeometryArrowHeadWings) {
    const auto wings = geometry::arrowHeadWings({0, 0}, {100, 0}, 1.0);
    // Length max(6, 3w) = 6 at +-30 degrees back from the tip.
    for (const Point w : wings) CHECK_NEAR(std::hypot(w.x - 100.0, w.y), 6.0, 1e-9);
    CHECK_NEAR(wings[0].y, -wings[1].y, 1e-9);
    CHECK(wings[0].x < 100.0);
    const auto wide = geometry::arrowHeadWings({0, 0}, {0, 100}, 10.0);
    CHECK_NEAR(std::hypot(wide[0].x, wide[0].y - 100.0), 30.0, 1e-9);
}

RIVET_TEST(annotationGeometryReduceStroke) {
    // Collinear points collapse to the end points.
    std::vector<Point> line;
    for (int i = 0; i <= 100; ++i) line.push_back({static_cast<double>(i), 2.0 * i});
    const auto reduced = geometry::reduceStroke(line, 0.01);
    CHECK_EQ(reduced.size(), std::size_t{2});
    CHECK_NEAR(reduced.front().x, 0.0, 0.0);
    CHECK_NEAR(reduced.back().x, 100.0, 0.0);

    // A corner is kept.
    std::vector<Point> corner{{0, 0}, {5, 0}, {10, 0}, {10, 5}, {10, 10}};
    const auto kept = geometry::reduceStroke(corner, 0.5);
    CHECK_EQ(kept.size(), std::size_t{3});
    CHECK_NEAR(kept[1].x, 10.0, 0.0);
    CHECK_NEAR(kept[1].y, 0.0, 0.0);

    // Error bound: every dropped point is within the tolerance of the result.
    std::vector<Point> wave;
    for (int i = 0; i < 400; ++i) wave.push_back({static_cast<double>(i), 10.0 * std::sin(i / 15.0)});
    const double tolerance = 0.5;
    const auto simple = geometry::reduceStroke(wave, tolerance);
    CHECK(simple.size() < wave.size() / 2);
    CHECK_NEAR(simple.front().x, 0.0, 0.0);
    CHECK_NEAR(simple.back().x, 399.0, 0.0);
    for (const Point& p : wave) {
        double best = 1e9;
        for (std::size_t i = 1; i < simple.size(); ++i) {
            best = std::min(best, geometry::distanceToSegment(p, simple[i - 1], simple[i]));
        }
        CHECK(best <= tolerance + 1e-9);
    }

    // Tiny inputs are returned as is; a very long stroke does not recurse.
    CHECK_EQ(geometry::reduceStroke(std::vector<Point>{{1, 1}}, 1.0).size(), std::size_t{1});
    CHECK_EQ(geometry::reduceStroke(std::vector<Point>{{1, 1}, {2, 2}}, 1.0).size(), std::size_t{2});
    std::vector<Point> huge;
    for (int i = 0; i < 50000; ++i) huge.push_back({static_cast<double>(i), (i % 2 == 0) ? 0.0 : 3.0});
    CHECK(geometry::reduceStroke(huge, 1.0).size() >= 2);
}

RIVET_TEST(annotationGeometryUserDisplayRoundTrip) {
    const pdf::PdfBox crops[] = {{0.0, 0.0, 600.0, 800.0}, {50.0, 100.0, 450.0, 700.0}};
    for (const pdf::PdfBox& crop : crops) {
        for (const PageRotation rotation : kRotations) {
            const pdf::PdfPageView view = viewOf(rotation, crop);
            const Point display{123.5, 77.25};
            const pdf::PdfPoint user = geometry::toUser(view, display);
            const Point back = geometry::toDisplay(view, user);
            CHECK_NEAR(back.x, display.x, 1e-6);
            CHECK_NEAR(back.y, display.y, 1e-6);

            const Rect rect{20.0, 30.0, 80.0, 40.0};
            const pdf::PdfBox box = geometry::rectToUserBox(view, rect);
            const Rect again = geometry::boxToDisplayRect(view, box);
            CHECK_NEAR(again.minX(), rect.minX(), 1e-6);
            CHECK_NEAR(again.minY(), rect.minY(), 1e-6);
            CHECK_NEAR(again.size.width, rect.size.width, 1e-6);
            CHECK_NEAR(again.size.height, rect.size.height, 1e-6);
        }
    }
}

RIVET_TEST(annotationGeometryViewMapsEveryKind) {
    for (const PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation, {10.0, 20.0, 410.0, 520.0});
        pdf::PdfAnnotationData ink;
        ink.kind = pdf::PdfAnnotationKind::Ink;
        ink.borderWidth = 2.0F;
        ink.inkStrokes = {{{100, 100}, {150, 140}, {200, 100}}};
        pdf::normalizeAnnotation(ink);
        const AnnotationView v = geometry::makeAnnotationView(core::AnnotationId{7}, ink, view, true);
        CHECK(v.id == core::AnnotationId{7});
        CHECK(v.drawnByOverlay);
        CHECK(v.appearance.has_value());
        CHECK_EQ(v.strokes.size(), std::size_t{1});
        CHECK_EQ(v.strokes[0].size(), std::size_t{3});
        // Display points are the user points through the view...
        for (std::size_t i = 0; i < 3; ++i) {
            const Point expect = geometry::toDisplay(view, ink.inkStrokes[0][i]);
            CHECK_NEAR(v.strokes[0][i].x, expect.x, 1e-9);
            CHECK_NEAR(v.strokes[0][i].y, expect.y, 1e-9);
        }
        // ...and they lie inside the bounds.
        for (const Point& p : v.strokes[0]) CHECK(v.bounds.contains(p));
        CHECK_EQ(v.caps, annotationCaps(pdf::PdfAnnotationKind::Ink));

        const AnnotationView plain = geometry::makeAnnotationView(core::AnnotationId{8}, ink, view, false);
        CHECK(!plain.drawnByOverlay);
        CHECK(!plain.appearance.has_value());
    }
}

RIVET_TEST(annotationGeometryStampAppearanceRotationFollowsView) {
    for (const PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation);
        pdf::PdfAnnotationData stamp;
        stamp.kind = pdf::PdfAnnotationKind::Stamp;
        stamp.rect = pdf::PdfBox{100, 100, 220, 160};
        stamp.stampName = pdf::PdfStampName::Approved;
        stamp.rotation = (360 - core::rotationDegrees(rotation)) % 360; // reads upright
        const AnnotationView v = geometry::makeAnnotationView(core::AnnotationId{1}, stamp, view, true);
        CHECK(v.appearance.has_value());
        CHECK(!v.appearance->texts.empty());
        // On screen the text is upright: (text rotation + view rotation) % 360 == 0.
        for (const DisplayText& text : v.appearance->texts) CHECK_EQ(text.rotation, 0);
    }
}

RIVET_TEST(annotationCapabilities) {
    using K = pdf::PdfAnnotationKind;
    const unsigned markup = kCapRestyle | kCapEditContents | kCapDelete;
    CHECK_EQ(annotationCaps(K::Highlight), markup);
    CHECK_EQ(annotationCaps(K::Underline), markup);
    CHECK_EQ(annotationCaps(K::StrikeOut), markup);
    CHECK_EQ(annotationCaps(K::Note), kCapMove | kCapRestyle | kCapEditContents | kCapDelete);
    const unsigned shape = kCapMove | kCapResize | kCapRestyle | kCapDelete;
    for (const K kind : {K::Ink, K::Square, K::Circle, K::Line, K::Arrow, K::Stamp}) {
        CHECK_EQ(annotationCaps(kind), shape);
    }
    CHECK_EQ(annotationCaps(K::Other), 0U);
}

RIVET_TEST(annotationHitTestMarkup) {
    for (const PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation, {20.0, 30.0, 520.0, 730.0});
        pdf::PdfAnnotationData d;
        d.kind = pdf::PdfAnnotationKind::Highlight;
        d.quads = {pdf::PdfQuad{{100, 500}, {200, 500}, {100, 485}, {200, 485}}};
        pdf::normalizeAnnotation(d);
        const AnnotationView v = geometry::makeAnnotationView(core::AnnotationId{1}, d, view, false);
        const Point inside = geometry::toDisplay(view, pdf::PdfPoint{150, 492});
        const Point near = geometry::toDisplay(view, pdf::PdfPoint{150, 480});
        const Point far = geometry::toDisplay(view, pdf::PdfPoint{150, 400});
        CHECK(geometry::hitsAnnotation(v, inside, 0.0));
        CHECK(!geometry::hitsAnnotation(v, near, 0.0));
        CHECK(geometry::hitsAnnotation(v, near, 6.0)); // 5pt below the quad
        CHECK(!geometry::hitsAnnotation(v, far, 6.0));
    }
}

RIVET_TEST(annotationHitTestSquareCircleOutlineAndFill) {
    for (const PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation, {0.0, 0.0, 600.0, 800.0});
        pdf::PdfAnnotationData square = squareData(); // 120 x 60, width 4
        const AnnotationView hollow = geometry::makeAnnotationView(core::AnnotationId{1}, square, view, false);
        const Rect b = hollow.bounds;
        const Point onEdge{b.minX() + 2.0, b.center().y}; // on the stroke
        const Point centre = b.center();
        CHECK(geometry::hitsAnnotation(hollow, onEdge, 0.0));
        CHECK(!geometry::hitsAnnotation(hollow, centre, 0.0)); // hollow: the interior is not hit
        CHECK(geometry::hitsAnnotation(hollow, Point{b.minX() - 3.0, b.center().y}, 4.0));
        CHECK(!geometry::hitsAnnotation(hollow, Point{b.minX() - 30.0, b.center().y}, 4.0));

        square.interiorColor = pdf::PdfColor{1, 0, 0};
        const AnnotationView filled = geometry::makeAnnotationView(core::AnnotationId{2}, square, view, false);
        CHECK(geometry::hitsAnnotation(filled, filled.bounds.center(), 0.0));

        pdf::PdfAnnotationData circle = squareData();
        circle.kind = pdf::PdfAnnotationKind::Circle;
        const AnnotationView ring = geometry::makeAnnotationView(core::AnnotationId{3}, circle, view, false);
        CHECK(!geometry::hitsAnnotation(ring, ring.bounds.center(), 0.0));
        // The bounds corner is OUTSIDE an ellipse: not a hit for a circle, a hit for the square.
        const Point corner{ring.bounds.minX() + 1.0, ring.bounds.minY() + 1.0};
        CHECK(!geometry::hitsAnnotation(ring, corner, 0.0));
        CHECK(geometry::hitsAnnotation(hollow, corner, 0.0));
        const Point top{ring.bounds.center().x, ring.bounds.minY() + 2.0};
        CHECK(geometry::hitsAnnotation(ring, top, 0.0));
    }
}

RIVET_TEST(annotationHitTestInkLineArrowNoteStamp) {
    const pdf::PdfPageView view = viewOf(PageRotation::Clockwise90, {0.0, 0.0, 600.0, 800.0});
    pdf::PdfAnnotationData ink;
    ink.kind = pdf::PdfAnnotationKind::Ink;
    ink.borderWidth = 4.0F;
    ink.inkStrokes = {{{100, 100}, {200, 100}, {200, 200}}};
    pdf::normalizeAnnotation(ink);
    const AnnotationView inkView = geometry::makeAnnotationView(core::AnnotationId{1}, ink, view, false);
    CHECK(geometry::hitsAnnotation(inkView, geometry::toDisplay(view, {150, 101}), 0.0));
    CHECK(!geometry::hitsAnnotation(inkView, geometry::toDisplay(view, {150, 110}), 0.0));
    CHECK(geometry::hitsAnnotation(inkView, geometry::toDisplay(view, {150, 110}), 8.5));
    CHECK(geometry::hitsAnnotation(inkView, geometry::toDisplay(view, {200, 150}), 0.0));
    CHECK(!geometry::hitsAnnotation(inkView, geometry::toDisplay(view, {120, 180}), 5.0));

    pdf::PdfAnnotationData line;
    line.kind = pdf::PdfAnnotationKind::Line;
    line.borderWidth = 2.0F;
    line.lineStart = {100, 400};
    line.lineEnd = {300, 400};
    pdf::normalizeAnnotation(line);
    const AnnotationView lineView = geometry::makeAnnotationView(core::AnnotationId{2}, line, view, false);
    CHECK(geometry::hitsAnnotation(lineView, geometry::toDisplay(view, {200, 400.5}), 0.0));
    CHECK(!geometry::hitsAnnotation(lineView, geometry::toDisplay(view, {200, 405}), 0.0));
    CHECK(geometry::hitsAnnotation(lineView, geometry::toDisplay(view, {200, 405}), 4.5));
    CHECK(!geometry::hitsAnnotation(lineView, geometry::toDisplay(view, {320, 400}), 4.0));

    pdf::PdfAnnotationData arrow = line;
    arrow.kind = pdf::PdfAnnotationKind::Arrow;
    const AnnotationView arrowView = geometry::makeAnnotationView(core::AnnotationId{3}, arrow, view, false);
    // A point on a wing of the head (user: back from the tip, to one side).
    const auto wings = geometry::arrowHeadWings(arrowView.lineStart, arrowView.lineEnd, 2.0);
    const Point wingMid{(wings[0].x + arrowView.lineEnd.x) / 2.0, (wings[0].y + arrowView.lineEnd.y) / 2.0};
    CHECK(geometry::hitsAnnotation(arrowView, wingMid, 0.0));
    CHECK(!geometry::hitsAnnotation(lineView, wingMid, 0.0));

    pdf::PdfAnnotationData note;
    note.kind = pdf::PdfAnnotationKind::Note;
    note.rect = pdf::PdfBox{50, 600, 74, 624};
    const AnnotationView noteView = geometry::makeAnnotationView(core::AnnotationId{4}, note, view, false);
    CHECK(geometry::hitsAnnotation(noteView, noteView.bounds.center(), 0.0));
    CHECK(!geometry::hitsAnnotation(noteView, Point{noteView.bounds.maxX() + 5.0, noteView.bounds.center().y}, 0.0));
    CHECK(geometry::hitsAnnotation(noteView, Point{noteView.bounds.maxX() + 5.0, noteView.bounds.center().y}, 6.0));

    pdf::PdfAnnotationData stamp = note;
    stamp.kind = pdf::PdfAnnotationKind::Stamp;
    stamp.rect = pdf::PdfBox{50, 600, 170, 660};
    const AnnotationView stampView = geometry::makeAnnotationView(core::AnnotationId{5}, stamp, view, true);
    CHECK(geometry::hitsAnnotation(stampView, stampView.bounds.center(), 0.0));

    AnnotationView other;
    other.kind = pdf::PdfAnnotationKind::Other;
    other.bounds = Rect{0, 0, 100, 100};
    CHECK(!geometry::hitsAnnotation(other, Point{50, 50}, 5.0));
}
