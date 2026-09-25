// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfTypes.hpp"

#include <cmath>
#include <limits>
#include <utility>

// Engine-independent page-view geometry (pdf/PdfPageGeometry.hpp): runs in
// both build modes. The expected values below are the convention of
// docs/ARCHITECTURE.md section 4 applied by hand to crop-box-relative
// coordinates.

namespace {

namespace core = rivet::core;
using rivet::pdf::PdfBox;
using rivet::pdf::PdfPageView;

constexpr double kEps = 1e-9;

constexpr core::PageRotation kAllRotations[] = {
    core::PageRotation::None,
    core::PageRotation::Clockwise90,
    core::PageRotation::Clockwise180,
    core::PageRotation::Clockwise270,
};

// A crop box with a non-zero origin: [36 36 576 756] (540 x 720).
constexpr PdfBox kCrop{36.0, 36.0, 576.0, 756.0};

bool nearPoint(const core::Point& a, const core::Point& b) {
    return std::fabs(a.x - b.x) <= kEps && std::fabs(a.y - b.y) <= kEps;
}

} // namespace

RIVET_TEST(pdfBoxBasics) {
    const PdfBox box{10.0, 20.0, 110.0, 220.0};
    CHECK_NEAR(box.width(), 100.0, kEps);
    CHECK_NEAR(box.height(), 200.0, kEps);
    CHECK(box.isValid());
    CHECK(box == (PdfBox{10.0, 20.0, 110.0, 220.0}));
    CHECK(!(box == (PdfBox{10.0, 20.0, 110.0, 221.0})));

    CHECK(!(PdfBox{0.0, 0.0, 0.0, 10.0}).isValid());   // zero width
    CHECK(!(PdfBox{0.0, 10.0, 10.0, 0.0}).isValid());  // inverted
    CHECK(!(PdfBox{0.0, 0.0, std::numeric_limits<double>::infinity(), 10.0}).isValid());
    CHECK(!(PdfBox{std::nan(""), 0.0, 10.0, 10.0}).isValid());

    const PdfBox overlap = box.intersection(PdfBox{50.0, 0.0, 500.0, 100.0});
    CHECK(overlap == (PdfBox{50.0, 20.0, 110.0, 100.0}));
    CHECK(!box.intersection(PdfBox{200.0, 0.0, 300.0, 10.0}).isValid()); // disjoint

    CHECK(box.contains(PdfBox{10.0, 20.0, 110.0, 220.0}));
    CHECK(box.contains(PdfBox{20.0, 30.0, 100.0, 200.0}));
    CHECK(!box.contains(PdfBox{9.0, 20.0, 110.0, 220.0}));
    CHECK(box.contains(PdfBox{9.9995, 20.0, 110.0, 220.0}, 1e-3));
}

RIVET_TEST(pdfPageViewDisplaySize) {
    CHECK(rivet::pdf::displaySize(PdfPageView{core::PageRotation::None, kCrop}) == core::Size(540.0, 720.0));
    CHECK(rivet::pdf::displaySize(PdfPageView{core::PageRotation::Clockwise90, kCrop}) ==
          core::Size(720.0, 540.0));
    CHECK(rivet::pdf::displaySize(PdfPageView{core::PageRotation::Clockwise180, kCrop}) ==
          core::Size(540.0, 720.0));
    CHECK(rivet::pdf::displaySize(PdfPageView{core::PageRotation::Clockwise270, kCrop}) ==
          core::Size(720.0, 540.0));
    CHECK(PdfPageView{core::PageRotation::Clockwise90, kCrop} ==
          (PdfPageView{core::PageRotation::Clockwise90, kCrop}));
    CHECK(!(PdfPageView{core::PageRotation::Clockwise90, kCrop} ==
            PdfPageView{core::PageRotation::None, kCrop}));
}

// The crop box's user-space top-left corner (36, 756) and a point 10 right /
// 20 down from it, for every rotation; plus the four crop corners landing on
// the display corners.
RIVET_TEST(pdfPageViewUserToDisplayAllRotations) {
    struct Case {
        core::PageRotation rotation;
        core::Point topLeft;  // display position of user (36, 756)
        core::Point inner;    // display position of user (46, 736)
    };
    // dx0 = x - 36, dy0 = y - 36, w0 = 540, h0 = 720.
    const Case cases[] = {
        {core::PageRotation::None, {0.0, 0.0}, {10.0, 20.0}},
        {core::PageRotation::Clockwise90, {720.0, 0.0}, {700.0, 10.0}},
        {core::PageRotation::Clockwise180, {540.0, 720.0}, {530.0, 700.0}},
        {core::PageRotation::Clockwise270, {0.0, 540.0}, {20.0, 530.0}},
    };
    for (const Case& c : cases) {
        const PdfPageView view{c.rotation, kCrop};
        CHECK(nearPoint(rivet::pdf::userToDisplay(view, 36.0, 756.0), c.topLeft));
        CHECK(nearPoint(rivet::pdf::userToDisplay(view, 46.0, 736.0), c.inner));

        // The four crop corners map onto the four display corners.
        const core::Size size = rivet::pdf::displaySize(view);
        const core::Point corners[] = {
            rivet::pdf::userToDisplay(view, kCrop.left, kCrop.bottom),
            rivet::pdf::userToDisplay(view, kCrop.right, kCrop.bottom),
            rivet::pdf::userToDisplay(view, kCrop.left, kCrop.top),
            rivet::pdf::userToDisplay(view, kCrop.right, kCrop.top),
        };
        for (const core::Point& p : corners) {
            CHECK(std::fabs(p.x) <= kEps || std::fabs(p.x - size.width) <= kEps);
            CHECK(std::fabs(p.y) <= kEps || std::fabs(p.y - size.height) <= kEps);
        }
    }
}

// displayToUser is the exact inverse of userToDisplay, and the affine form
// agrees with the function, for every rotation (non-zero-origin crop box).
RIVET_TEST(pdfPageViewInverseAndMatrixRoundTrip) {
    const std::pair<double, double> samples[] = {
        {36.0, 36.0}, {576.0, 756.0}, {100.25, 400.5}, {300.0, 50.75}, {0.0, 0.0}, {612.0, 792.0},
    };
    for (const core::PageRotation rotation : kAllRotations) {
        const PdfPageView view{rotation, kCrop};
        const core::Matrix matrix = rivet::pdf::userToDisplayMatrix(view);
        for (const auto& [x, y] : samples) {
            const core::Point display = rivet::pdf::userToDisplay(view, x, y);
            const auto [ux, uy] = rivet::pdf::displayToUser(view, display);
            CHECK_NEAR(ux, x, kEps);
            CHECK_NEAR(uy, y, kEps);
            CHECK(nearPoint(matrix.map(core::Point{x, y}), display));
        }
        // Display -> user -> display as well.
        const core::Point displayPoint{123.5, 45.25};
        const auto [ux, uy] = rivet::pdf::displayToUser(view, displayPoint);
        CHECK(nearPoint(rivet::pdf::userToDisplay(view, ux, uy), displayPoint));
    }
}

// Boxes map to display rects (normalized, clamped) and back.
RIVET_TEST(pdfPageViewBoxRectRoundTrip) {
    const PdfBox inner{100.0, 200.0, 250.0, 260.0};
    for (const core::PageRotation rotation : kAllRotations) {
        const PdfPageView view{rotation, kCrop};
        const core::Rect rect = rivet::pdf::userBoxToDisplayRect(view, inner);
        CHECK(rect.size.width > 0.0);
        CHECK(rect.size.height > 0.0);
        const PdfBox back = rivet::pdf::displayRectToUserBox(view, rect);
        CHECK_NEAR(back.left, inner.left, kEps);
        CHECK_NEAR(back.bottom, inner.bottom, kEps);
        CHECK_NEAR(back.right, inner.right, kEps);
        CHECK_NEAR(back.top, inner.top, kEps);

        // The crop box itself covers the whole displayed page.
        const core::Rect whole = rivet::pdf::userBoxToDisplayRect(view, kCrop);
        CHECK(core::Rect::nearlyEqual(
            whole, core::Rect{core::Point{0.0, 0.0}, rivet::pdf::displaySize(view)}, kEps));
    }

    // Unrotated: box [100 200 250 260] -> display x 64..214, y 496..556.
    const core::Rect none =
        rivet::pdf::userBoxToDisplayRect(PdfPageView{core::PageRotation::None, kCrop}, inner);
    CHECK(core::Rect::nearlyEqual(none, core::Rect{64.0, 496.0, 150.0, 60.0}, kEps));
    // 90: display x = y - 36 in 164..224, display y = x - 36 in 64..214.
    const core::Rect turned =
        rivet::pdf::userBoxToDisplayRect(PdfPageView{core::PageRotation::Clockwise90, kCrop}, inner);
    CHECK(core::Rect::nearlyEqual(turned, core::Rect{164.0, 64.0, 60.0, 150.0}, kEps));
}

RIVET_TEST(pdfPageViewClampsAndRejects) {
    const PdfPageView view{core::PageRotation::None, kCrop};
    // A box straddling the crop box's left/top edges is clamped to the page.
    const core::Rect clamped = rivet::pdf::userBoxToDisplayRect(view, PdfBox{0.0, 700.0, 100.0, 792.0});
    CHECK(core::Rect::nearlyEqual(clamped, core::Rect{0.0, 0.0, 64.0, 56.0}, kEps));
    // A box entirely outside collapses to a zero-area rect on the edge.
    const core::Rect outside = rivet::pdf::userBoxToDisplayRect(view, PdfBox{0.0, 0.0, 30.0, 30.0});
    CHECK(outside.isEmpty());
    // Invalid boxes yield an empty rect.
    CHECK(rivet::pdf::userBoxToDisplayRect(view, PdfBox{10.0, 10.0, 5.0, 20.0}).isEmpty());
    CHECK(rivet::pdf::userBoxToDisplayRect(view, PdfBox{std::nan(""), 0.0, 5.0, 20.0}).isEmpty());
    // An empty display rect maps back to a zero-area (invalid) box.
    CHECK(!rivet::pdf::displayRectToUserBox(view, core::Rect{10.0, 10.0, 0.0, 5.0}).isValid());
}

// Fake-backend PdfPageInfo: the size-only constructor derives zero-origin
// boxes consistent with the size and rotation.
RIVET_TEST(pdfPageInfoSizeOnlyConstructor) {
    const rivet::pdf::PdfPageInfo plain(3, core::Size{612.0, 792.0}, core::PageRotation::None);
    CHECK_EQ(plain.index, std::size_t{3});
    CHECK(plain.mediaBox == (PdfBox{0.0, 0.0, 612.0, 792.0}));
    CHECK(plain.view.cropBox == plain.mediaBox);
    CHECK(rivet::pdf::displaySize(plain.view) == plain.sizePoints);

    const rivet::pdf::PdfPageInfo turned(0, core::Size{792.0, 612.0}, core::PageRotation::Clockwise90);
    CHECK(turned.mediaBox == (PdfBox{0.0, 0.0, 612.0, 792.0}));
    CHECK_EQ(turned.view.rotation, core::PageRotation::Clockwise90);
    CHECK(rivet::pdf::displaySize(turned.view) == turned.sizePoints);
}
