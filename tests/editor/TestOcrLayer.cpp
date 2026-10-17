// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/geometry/Matrix.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "editor/OcrLayer.hpp"
#include "pdf/BundledFontMetrics.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <array>
#include <cmath>
#include <string>
#include <vector>

// Pure OCR planning (ADR-0027): bitmap pixels -> display space -> user space
// through every rotation and a crop offset, the horizontal scale, and the
// planner limits. No PDF engine.

using namespace rivet;
using namespace rivet::editor;
using core::PageRotation;
using core::Point;

namespace {

const std::array<PageRotation, 4> kRotations{PageRotation::None, PageRotation::Clockwise90,
                                             PageRotation::Clockwise180, PageRotation::Clockwise270};

// A 200 x 300 media box cropped to an off-origin box, presented at `rotation`.
pdf::PdfPageView viewOf(PageRotation rotation) {
    pdf::PdfPageView view;
    view.rotation = rotation;
    view.cropBox = pdf::PdfBox{10.0, 20.0, 190.0, 280.0}; // 180 x 260
    return view;
}

OcrWord wordAt(std::string text, double x, double y, double w, double h) {
    OcrWord word;
    word.text = std::move(text);
    word.quad = {Point{x, y}, Point{x + w, y}, Point{x + w, y + h}, Point{x, y + h}};
    return word;
}

} // namespace

RIVET_TEST(ocrBitmapPixelsScaleToDisplayPoints) {
    // A 300 dpi bitmap of a 200 x 100 pt page is 833.33 x 416.67 px; use exact numbers.
    const std::array<Point, 4> quad{Point{100, 50}, Point{300, 50}, Point{300, 90}, Point{100, 90}};
    const OcrWord word = ocrWordFromBitmap("Hi", quad, 1000, 500, core::Size{200.0, 100.0});
    CHECK_EQ(word.text, std::string("Hi"));
    CHECK_NEAR(word.quad[0].x, 20.0, 1e-9);
    CHECK_NEAR(word.quad[0].y, 10.0, 1e-9);
    CHECK_NEAR(word.quad[2].x, 60.0, 1e-9);
    CHECK_NEAR(word.quad[2].y, 18.0, 1e-9);
}

RIVET_TEST(ocrPlanMapsDisplayBoxesToUserSpaceThroughEveryRotation) {
    const pdf::BundledFontMetrics& metrics = pdf::bundledFontMetrics(pdf::PdfBundledFont::SansRegular);
    for (const PageRotation rotation : kRotations) {
        const pdf::PdfPageView view = viewOf(rotation);
        const OcrWord word = wordAt("Hello", 30.0, 40.0, 60.0, 12.0);
        const InvisibleTextPlan plan = planInvisibleText(view, std::span<const OcrWord>(&word, 1));
        CHECK_EQ(plan.runs.size(), std::size_t{1});
        CHECK_EQ(plan.skipped, std::size_t{0});
        if (plan.runs.size() != 1) continue;
        const pdf::PdfInvisibleTextRun& run = plan.runs[0];
        CHECK_EQ(run.text, std::string("Hello"));
        CHECK_NEAR(run.fontSize, 12.0, 1e-9);
        // The natural advance times the horizontal scale is the box width.
        CHECK_NEAR(metrics.textWidth("Hello", run.fontSize) * run.horizontalScale, 60.0, 1e-6);

        // The run's frame, taken back to display space, is the upright
        // baseline frame: origin a fifth of the height above the bottom edge,
        // x along the line, y up (display y is down).
        const core::Matrix toDisplay = pdf::userToDisplayMatrix(view) * run.placement;
        const Point origin = toDisplay.map(Point{0.0, 0.0});
        const Point alongX = toDisplay.map(Point{1.0, 0.0});
        const Point alongY = toDisplay.map(Point{0.0, 1.0});
        CHECK_NEAR(origin.x, 30.0, 1e-6);
        CHECK_NEAR(origin.y, 52.0 - 0.2 * 12.0, 1e-6);
        CHECK_NEAR(alongX.x - origin.x, 1.0, 1e-6);
        CHECK_NEAR(alongX.y - origin.y, 0.0, 1e-6);
        CHECK_NEAR(alongY.x - origin.x, 0.0, 1e-6);
        CHECK_NEAR(alongY.y - origin.y, -1.0, 1e-6);
        // The placement is rigid (no scale) in user space.
        const Point ux = run.placement.map(Point{1.0, 0.0});
        const Point u0 = run.placement.map(Point{0.0, 0.0});
        CHECK_NEAR(std::hypot(ux.x - u0.x, ux.y - u0.y), 1.0, 1e-9);
    }
}

RIVET_TEST(ocrPlanFollowsTheCropOffsetInUserSpace) {
    // Unrotated: display (30, 52 - 2.4) is crop-relative; user space adds the
    // crop box's left edge and counts y from the crop's top.
    const pdf::PdfPageView view = viewOf(PageRotation::None);
    const OcrWord word = wordAt("Hello", 30.0, 40.0, 60.0, 12.0);
    const InvisibleTextPlan plan = planInvisibleText(view, std::span<const OcrWord>(&word, 1));
    CHECK_EQ(plan.runs.size(), std::size_t{1});
    if (plan.runs.size() != 1) return;
    const Point user = plan.runs[0].placement.map(Point{0.0, 0.0});
    CHECK_NEAR(user.x, 10.0 + 30.0, 1e-6);
    CHECK_NEAR(user.y, 20.0 + (260.0 - 49.6), 1e-6);
}

RIVET_TEST(ocrPlanFollowsATiltedBaseline) {
    // A word tilted by atan(3/4): the baseline is 5 long and rises to the right.
    OcrWord word;
    word.text = "Hi";
    // BL -> BR is (4, -3); the top edge sits above it.
    word.quad = {Point{40.0, 46.0}, Point{44.0, 43.0}, Point{48.0, 49.0}, Point{44.0, 52.0}};
    const pdf::PdfPageView view = viewOf(PageRotation::None);
    const InvisibleTextPlan plan = planInvisibleText(view, std::span<const OcrWord>(&word, 1));
    CHECK_EQ(plan.runs.size(), std::size_t{1});
    if (plan.runs.size() != 1) return;
    const core::Matrix toDisplay = pdf::userToDisplayMatrix(view) * plan.runs[0].placement;
    const Point origin = toDisplay.map(Point{0.0, 0.0});
    const Point alongX = toDisplay.map(Point{1.0, 0.0});
    CHECK_NEAR(alongX.x - origin.x, 4.0 / 5.0, 1e-6);
    CHECK_NEAR(alongX.y - origin.y, -3.0 / 5.0, 1e-6);
}

RIVET_TEST(ocrPlanDropsUnwritableCharactersAndDegenerateWords) {
    const pdf::PdfPageView view = viewOf(PageRotation::None);
    std::vector<OcrWord> words;
    words.push_back(wordAt("A\x01" "B", 10, 10, 30, 10));             // control character dropped
    words.push_back(wordAt("\xE4\xB8\xAD", 10, 30, 30, 10));          // CJK: not in the bundled face
    words.push_back(wordAt("Zero", 10, 50, 0.0, 10));                 // empty box
    words.push_back(wordAt("Flat", 10, 70, 30, 0.1));                 // below the minimum side
    words.push_back(wordAt("", 10, 90, 30, 10));                      // empty text
    words.push_back(wordAt("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", 10, 110, 60, 10)); // Cyrillic
    const InvisibleTextPlan plan = planInvisibleText(view, words);
    CHECK_EQ(plan.runs.size(), std::size_t{2});
    CHECK_EQ(plan.skipped, std::size_t{4});
    if (plan.runs.size() == 2) {
        CHECK_EQ(plan.runs[0].text, std::string("AB"));
        CHECK_EQ(plan.runs[1].text, std::string("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));
    }
}

RIVET_TEST(ocrPlanIsBoundedByTheRunLimit) {
    const pdf::PdfPageView view = viewOf(PageRotation::None);
    std::vector<OcrWord> words(pdf::kMaxInvisibleRunsPerPage + 5, wordAt("a", 10, 10, 8, 8));
    const InvisibleTextPlan plan = planInvisibleText(view, words);
    CHECK_EQ(plan.runs.size(), pdf::kMaxInvisibleRunsPerPage);
    CHECK_EQ(plan.skipped, std::size_t{5});
    CHECK(pdf::validate([&] {
              pdf::PdfPageContentEdits edits;
              edits.invisibleRuns = std::vector(plan.runs); // copy, then move-assign (avoids GCC 13 -Wnull-dereference in vector::operator=)
              edits.invisibleTag = 1;
              return edits;
          }(),
          0)
              .has_value());
}
