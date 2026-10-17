// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "pdf/ImageReduce.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// Portable parts of Save Reduced Copy (ADR-0028): preset table, the dpi
// decision and the box-filter downsampler. Run in both build modes.

namespace {

namespace pdf = rivet::pdf;

RIVET_TEST(reducePresetTableMatchesTheAdr) {
    CHECK_EQ(pdf::reducePreset(pdf::ReduceQuality::High).dpi, 200);
    CHECK_EQ(pdf::reducePreset(pdf::ReduceQuality::High).jpegQuality, 85);
    CHECK_EQ(pdf::reducePreset(pdf::ReduceQuality::Medium).dpi, 150);
    CHECK_EQ(pdf::reducePreset(pdf::ReduceQuality::Medium).jpegQuality, 75);
    CHECK_EQ(pdf::reducePreset(pdf::ReduceQuality::Small).dpi, 100);
    CHECK_EQ(pdf::reducePreset(pdf::ReduceQuality::Small).jpegQuality, 60);
}

RIVET_TEST(planReductionLimitsEachAxisToTheTargetDpi) {
    // 1000 px placed 144 pt (2 in) wide = 500 dpi; at 150 dpi -> 300 px.
    const auto plan = pdf::planReduction(1000, 500, 144.0, 72.0, 150);
    CHECK(plan.has_value());
    if (!plan.has_value()) return;
    CHECK_EQ(plan->width, std::uint32_t{300});
    CHECK_EQ(plan->height, std::uint32_t{150});
    CHECK(plan->resample);
}

RIVET_TEST(planReductionNeverEnlargesAndKeepsLowResolutionImages) {
    // 200 px over 2 in = 100 dpi, below 150: nothing to resample.
    const auto plan = pdf::planReduction(200, 200, 144.0, 144.0, 150);
    CHECK(plan.has_value());
    if (!plan.has_value()) return;
    CHECK_EQ(plan->width, std::uint32_t{200});
    CHECK_EQ(plan->height, std::uint32_t{200});
    CHECK(!plan->resample);
}

RIVET_TEST(planReductionRefusesTinyAndDegenerateInputs) {
    CHECK(!pdf::planReduction(63, 500, 10.0, 10.0, 150).has_value());
    CHECK(!pdf::planReduction(500, 63, 10.0, 10.0, 150).has_value());
    CHECK(!pdf::planReduction(500, 500, 0.0, 10.0, 150).has_value());
    CHECK(!pdf::planReduction(500, 500, 10.0, -1.0, 150).has_value());
    CHECK(!pdf::planReduction(500, 500, 10.0, 10.0, 0).has_value());
}

RIVET_TEST(boxDownsampleAveragesBlocks) {
    // 4x2 gray: [0 100 200 40 / 100 200 0 40] -> 2x1 averages of 2x2 blocks.
    const std::vector<std::uint8_t> src = {0, 100, 200, 40, 100, 200, 0, 40};
    const auto out = pdf::boxDownsample(src, 4, 2, 1, 4, 2, 1);
    CHECK_EQ(out.size(), std::size_t{2});
    if (out.size() != 2) return;
    CHECK_EQ(int{out[0]}, 100); // (0+100+100+200)/4
    CHECK_EQ(int{out[1]}, 70);  // (200+40+0+40)/4
}

RIVET_TEST(boxDownsampleKeepsConstantImagesConstantWithFractionalRatios) {
    const std::vector<std::uint8_t> src(7 * 5 * 3, 123);
    const auto out = pdf::boxDownsample(src, 7, 5, 3, 7 * 3, 3, 2);
    CHECK_EQ(out.size(), std::size_t{3 * 2 * 3});
    for (const std::uint8_t value : out) CHECK_EQ(int{value}, 123);
}

RIVET_TEST(boxDownsampleHonoursStrideAndChannels) {
    // 2x2 RGB with 2 bytes of row padding (0xFF must never be read).
    const std::vector<std::uint8_t> src = {10, 20, 30, 30, 40, 50, 0xFF, 0xFF,
                                           50, 60, 70, 70, 80, 90, 0xFF, 0xFF};
    const auto out = pdf::boxDownsample(src, 2, 2, 3, 8, 1, 1);
    CHECK_EQ(out.size(), std::size_t{3});
    if (out.size() != 3) return;
    CHECK_EQ(int{out[0]}, 40);
    CHECK_EQ(int{out[1]}, 50);
    CHECK_EQ(int{out[2]}, 60);
}

RIVET_TEST(boxDownsampleRejectsInconsistentArguments) {
    const std::vector<std::uint8_t> src(16, 1);
    CHECK(pdf::boxDownsample(src, 4, 4, 1, 4, 5, 2).empty());  // enlarges
    CHECK(pdf::boxDownsample(src, 4, 4, 1, 4, 0, 2).empty());  // empty target
    CHECK(pdf::boxDownsample(src, 4, 4, 1, 3, 2, 2).empty());  // stride < row
    CHECK(pdf::boxDownsample(src, 8, 8, 1, 8, 2, 2).empty());  // buffer too small
    CHECK(pdf::boxDownsample(src, 4, 4, 5, 20, 2, 2).empty()); // channels
}

} // namespace
