#include "RivetTest.h"

#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "render/PageTransform.hpp"

#include <optional>
#include <vector>

using rivet::core::Matrix;
using rivet::core::PageRotation;
using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::render::PageTransform;

namespace {

constexpr double kPageW = 200.0; // unrotated page width in points
constexpr double kPageH = 100.0; // unrotated page height in points
constexpr double kFrameX = 50.0;
constexpr double kFrameY = 100.0;

Size displaySize(PageRotation rotation) {
    switch (rotation) {
        case PageRotation::None:
        case PageRotation::Clockwise180:
            return {kPageW, kPageH};
        case PageRotation::Clockwise90:
        case PageRotation::Clockwise270:
            return {kPageH, kPageW};
    }
    return {kPageW, kPageH};
}

std::vector<Point> samplePagePoints() {
    return {
        {0.0, 0.0},
        {kPageW, 0.0},
        {0.0, kPageH},
        {kPageW, kPageH},
        {kPageW / 3.0, kPageH / 4.0},
        {kPageW / 2.0, kPageH / 2.0},
        {17.3, 91.7},
    };
}

std::vector<PageRotation> allRotations() {
    return {PageRotation::None, PageRotation::Clockwise90, PageRotation::Clockwise180,
            PageRotation::Clockwise270};
}

} // namespace

RIVET_TEST(pageTransformPointRoundtrip) {
    for (const double zoom : {1.0, 2.5}) {
        for (const PageRotation rotation : allRotations()) {
            const Size display = displaySize(rotation);
            const PageTransform t =
                PageTransform::make(Rect{kFrameX, kFrameY, display.width, display.height},
                                    Size{kPageW, kPageH}, rotation, zoom, 2.0);
            for (const Point& p : samplePagePoints()) {
                const Point logical = t.pageToLogical(p);
                const Point page = t.logicalToPage(logical);
                CHECK_NEAR(page.x, p.x, 1e-9);
                CHECK_NEAR(page.y, p.y, 1e-9);
            }
        }
    }
}

RIVET_TEST(pageTransformCorners) {
    // Page point -> logical point at zoom 1.0, frame origin {50, 100}.
    // Page (0, 0) is the bottom-left corner, (0, kPageH) the top-left.
    struct Expectation {
        PageRotation rotation;
        Point pagePoint;
        Point logical;
    };
    const Expectation expectations[] = {
        {PageRotation::None, {0.0, 0.0}, {50.0, 200.0}},
        {PageRotation::None, {0.0, kPageH}, {50.0, 100.0}},
        {PageRotation::None, {kPageW, 0.0}, {250.0, 200.0}},
        {PageRotation::None, {kPageW, kPageH}, {250.0, 100.0}},

        // Clockwise90: page top edge lands on the display right edge.
        {PageRotation::Clockwise90, {0.0, 0.0}, {50.0, 100.0}},
        {PageRotation::Clockwise90, {0.0, kPageH}, {150.0, 100.0}},
        {PageRotation::Clockwise90, {kPageW, 0.0}, {50.0, 300.0}},
        {PageRotation::Clockwise90, {kPageW, kPageH}, {150.0, 300.0}},

        {PageRotation::Clockwise180, {0.0, 0.0}, {250.0, 100.0}},
        {PageRotation::Clockwise180, {0.0, kPageH}, {250.0, 200.0}},
        {PageRotation::Clockwise180, {kPageW, 0.0}, {50.0, 100.0}},
        {PageRotation::Clockwise180, {kPageW, kPageH}, {50.0, 200.0}},

        // Clockwise270: page top edge lands on the display left edge.
        {PageRotation::Clockwise270, {0.0, 0.0}, {150.0, 300.0}},
        {PageRotation::Clockwise270, {0.0, kPageH}, {50.0, 300.0}},
        {PageRotation::Clockwise270, {kPageW, 0.0}, {150.0, 100.0}},
        {PageRotation::Clockwise270, {kPageW, kPageH}, {50.0, 100.0}},
    };
    for (const Expectation& e : expectations) {
        const Size display = displaySize(e.rotation);
        const PageTransform t =
            PageTransform::make(Rect{kFrameX, kFrameY, display.width, display.height},
                                Size{kPageW, kPageH}, e.rotation, 1.0, 1.0);
        const Point mapped = t.pageToLogical(e.pagePoint);
        CHECK_NEAR(mapped.x, e.logical.x, 1e-9);
        CHECK_NEAR(mapped.y, e.logical.y, 1e-9);
        const Point back = t.logicalToPage(e.logical);
        CHECK_NEAR(back.x, e.pagePoint.x, 1e-9);
        CHECK_NEAR(back.y, e.pagePoint.y, 1e-9);
    }
}

RIVET_TEST(pageTransformZoomScalesLinearly) {
    for (const PageRotation rotation : allRotations()) {
        const Size display = displaySize(rotation);
        const PageTransform unit =
            PageTransform::make(Rect{kFrameX, kFrameY, display.width, display.height},
                                Size{kPageW, kPageH}, rotation, 1.0, 1.0);
        const PageTransform scaled =
            PageTransform::make(Rect{kFrameX, kFrameY, display.width, display.height},
                                Size{kPageW, kPageH}, rotation, 2.5, 1.0);
        CHECK_NEAR(scaled.zoom(), 2.5, 1e-12);
        for (const Point& p : samplePagePoints()) {
            const Point atUnit = unit.pageToLogical(p);
            const Point atScaled = scaled.pageToLogical(p);
            CHECK_NEAR(atScaled.x, kFrameX + 2.5 * (atUnit.x - kFrameX), 1e-9);
            CHECK_NEAR(atScaled.y, kFrameY + 2.5 * (atUnit.y - kFrameY), 1e-9);
        }
    }
}

RIVET_TEST(pageTransformRectRoundtrip) {
    for (const PageRotation rotation : allRotations()) {
        const Size display = displaySize(rotation);
        const PageTransform t =
            PageTransform::make(Rect{kFrameX, kFrameY, display.width, display.height},
                                Size{kPageW, kPageH}, rotation, 2.0, 1.0);
        const Rect pageRect{Point{10.0, 20.0}, Size{100.0, 50.0}};
        const Rect logical = t.pageToLogical(pageRect);
        // Rotation + uniform scale preserves area.
        CHECK_NEAR(logical.area(), pageRect.area() * 4.0, 1e-9);
        const Rect page = t.logicalToPage(logical);
        CHECK_NEAR(page.origin.x, pageRect.origin.x, 1e-9);
        CHECK_NEAR(page.origin.y, pageRect.origin.y, 1e-9);
        CHECK_NEAR(page.size.width, pageRect.size.width, 1e-9);
        CHECK_NEAR(page.size.height, pageRect.size.height, 1e-9);
        // The display frame (at zoom 2.0) maps back onto the whole page.
        CHECK(Rect::nearlyEqual(
            t.logicalToPage(Rect{kFrameX, kFrameY, display.width * 2.0, display.height * 2.0}),
            Rect{0.0, 0.0, kPageW, kPageH}, 1e-9));
    }
}

RIVET_TEST(pageTransformPhysicalMapping) {
    const PageTransform t = PageTransform::make(Rect{50.0, 100.0, 200.0, 100.0}, Size{kPageW, kPageH},
                                                PageRotation::None, 1.0, 2.0);
    CHECK_NEAR(t.backingScale(), 2.0, 1e-12);
    CHECK_NEAR(t.devicePixelsPerPoint(), 2.0, 1e-12);

    const Point physical = t.logicalToPhysical(Point{60.0, 120.0});
    CHECK_NEAR(physical.x, 120.0, 1e-12);
    CHECK_NEAR(physical.y, 240.0, 1e-12);
    const Point logical = t.physicalToLogical(physical);
    CHECK_NEAR(logical.x, 60.0, 1e-12);
    CHECK_NEAR(logical.y, 120.0, 1e-12);

    const Rect physicalRect = t.logicalToPhysical(Rect{50.0, 100.0, 200.0, 100.0});
    CHECK_NEAR(physicalRect.origin.x, 100.0, 1e-12);
    CHECK_NEAR(physicalRect.origin.y, 200.0, 1e-12);
    CHECK_NEAR(physicalRect.size.width, 400.0, 1e-12);
    CHECK_NEAR(physicalRect.size.height, 200.0, 1e-12);

    const PageTransform hiDpi = PageTransform::make(Rect{0.0, 0.0, 100.0, 100.0}, Size{kPageW, kPageH},
                                                    PageRotation::None, 2.5, 2.0);
    CHECK_NEAR(hiDpi.devicePixelsPerPoint(), 5.0, 1e-12);
}

RIVET_TEST(pageTransformMatrixMatchesPointMapping) {
    for (const double zoom : {1.0, 2.5}) {
        for (const PageRotation rotation : allRotations()) {
            const Size display = displaySize(rotation);
            const PageTransform t =
                PageTransform::make(Rect{kFrameX, kFrameY, display.width, display.height},
                                    Size{kPageW, kPageH}, rotation, zoom, 2.0);
            const Matrix matrix = t.pageToLogicalMatrix();
            for (const Point& p : samplePagePoints()) {
                const Point viaMatrix = matrix.map(p);
                const Point direct = t.pageToLogical(p);
                CHECK_NEAR(viaMatrix.x, direct.x, 1e-9);
                CHECK_NEAR(viaMatrix.y, direct.y, 1e-9);
            }
            // The matrix inverse agrees with logicalToPage.
            const std::optional<Matrix> inverse = matrix.inverted();
            CHECK(inverse.has_value());
            for (const Point& p : samplePagePoints()) {
                const Point logical = t.pageToLogical(p);
                const Point viaInverse = inverse->map(logical);
                CHECK_NEAR(viaInverse.x, p.x, 1e-9);
                CHECK_NEAR(viaInverse.y, p.y, 1e-9);
            }
        }
    }
}
