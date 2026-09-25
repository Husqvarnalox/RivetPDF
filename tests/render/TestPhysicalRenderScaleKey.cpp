#include "RivetTest.h"

#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderScaleKey.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_set>
#include <vector>

using rivet::render::PhysicalRenderScaleKey;
using rivet::render::RenderScaleKey;

namespace {

// Zoom x backing products, ascending, that all land inside the sane range.
std::vector<std::pair<double, double>> sampleDensities() {
    return {
        {0.10, 1.0}, {0.125, 1.0}, {0.5, 1.5}, {1.0, 1.0},  {1.0, 1.01},
        {1.25, 1.0}, {2.0, 1.0},   {2.0, 2.0}, {3.33, 1.5}, {8.0, 2.0},
        {31.9, 1.0}, {64.0, 2.0},
    };
}

} // namespace

RIVET_TEST(physicalScaleKeyQuantizesUp) {
    // Exact multiples of 1/64 stay exact: 1.0 zoom x 1.0 backing.
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(1.0, 1.0).value, 64u);
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(2.0, 1.0).value, 128u);
    // ceil(1.5 * 64) = 96.
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(1.5, 1.0).value, 96u);
    // In-between products round UP to the next 1/64 step: ceil(97/64 * 64).
    const double justAboveOne = 1.0 + 1.0 / (4.0 * PhysicalRenderScaleKey::kDenominator);
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(justAboveOne, 1.0).value, 65u);
    CHECK_GT(PhysicalRenderScaleKey::fromDensities(1.0 + 1e-12, 1.0).value, 64u);
}

RIVET_TEST(physicalScaleKeySeparatesBackingScales) {
    // Same zoom, different display backing: different pixel dimensions, so
    // different cache identities.
    const PhysicalRenderScaleKey oneX = PhysicalRenderScaleKey::fromDensities(1.0, 1.0);
    const PhysicalRenderScaleKey twoX = PhysicalRenderScaleKey::fromDensities(1.0, 2.0);
    CHECK(oneX != twoX);
    CHECK_LT(oneX, twoX);
    CHECK_EQ(oneX.value, 64u);
    CHECK_EQ(twoX.value, 128u);

    // The distinction survives quantization on both sides.
    const PhysicalRenderScaleKey quantized =
        PhysicalRenderScaleKey::fromDensities(RenderScaleKey::fromZoom(1.0).scale(), 2.0);
    CHECK_EQ(quantized, twoX);
}

RIVET_TEST(physicalScaleKeySharesBuckets) {
    // Products that land in the same 1/64 bucket share one key.
    const double a = 1.0 + 1.0 / (8.0 * PhysicalRenderScaleKey::kDenominator);  // just above 1
    const double b = 1.0 + 1.0 / (16.0 * PhysicalRenderScaleKey::kDenominator); // a bit less above
    CHECK(a != b);
    const PhysicalRenderScaleKey keyA = PhysicalRenderScaleKey::fromDensities(a, 1.0);
    const PhysicalRenderScaleKey keyB = PhysicalRenderScaleKey::fromDensities(b, 1.0);
    CHECK_EQ(keyA, keyB);

    // The split of the product across factors does not matter: 0.5 x 2.0 and
    // 2.0 x 0.5 are the same density.
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(0.5, 2.0),
             PhysicalRenderScaleKey::fromDensities(2.0, 0.5));
}

RIVET_TEST(physicalScaleKeyClampsToSaneRange) {
    const PhysicalRenderScaleKey minKey = PhysicalRenderScaleKey::fromDensities(0.0, 1.0);
    const PhysicalRenderScaleKey maxKey =
        PhysicalRenderScaleKey::fromDensities(PhysicalRenderScaleKey::kMaxDensity, 1.0);

    // Below/above the documented range clamps: [0.1, 512] device px per point.
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(-3.0, 0.5).value, minKey.value);
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(1e9, 1e9).value, maxKey.value);
    CHECK_GE(minKey.scale(), PhysicalRenderScaleKey::kMinDensity);
    CHECK_EQ(maxKey.scale(), PhysicalRenderScaleKey::kMaxDensity);

    // A non-finite product maps to density 1.0, mirroring RenderScaleKey.
    const double inf = std::numeric_limits<double>::infinity();
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(std::nan(""), 1.0).value, 64u);
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(inf, inf).value, 64u);
    CHECK_EQ(PhysicalRenderScaleKey::fromDensities(-inf, 1.0).value, 64u);
}

RIVET_TEST(physicalScaleKeyIsDeterministic) {
    // Repeated calls with identical inputs give identical keys, and equal
    // inputs hash and compare equal in unordered containers.
    for (const auto& [zoom, backing] : sampleDensities()) {
        const PhysicalRenderScaleKey first = PhysicalRenderScaleKey::fromDensities(zoom, backing);
        const PhysicalRenderScaleKey second = PhysicalRenderScaleKey::fromDensities(zoom, backing);
        CHECK_EQ(first.value, second.value);
        CHECK(first == second);
    }

    std::unordered_set<PhysicalRenderScaleKey> keys;
    keys.insert(PhysicalRenderScaleKey::fromDensities(1.0, 1.0));
    keys.insert(PhysicalRenderScaleKey::fromDensities(1.0, 1.0)); // duplicate
    keys.insert(PhysicalRenderScaleKey::fromDensities(1.0, 2.0));
    keys.insert(PhysicalRenderScaleKey::fromDensities(2.0, 1.0)); // same density as 1x2
    CHECK_EQ(keys.size(), std::size_t{2});
}

RIVET_TEST(physicalScaleKeyRoundTripsWithinOneStep) {
    const double step = 1.0 / PhysicalRenderScaleKey::kDenominator;
    for (const auto& [zoom, backing] : sampleDensities()) {
        const PhysicalRenderScaleKey key = PhysicalRenderScaleKey::fromDensities(zoom, backing);
        // Never below the requested density, at most one step above.
        CHECK_GE(key.scale(), zoom * backing - step * 1e-6);
        CHECK_LE(key.scale(), zoom * backing + step);
        // scale() is the exact rational value / kDenominator.
        CHECK_NEAR(key.scale() * PhysicalRenderScaleKey::kDenominator,
                   static_cast<double>(key.value), 1e-9);
    }
}

RIVET_TEST(physicalScaleKeyIsMonotonicInProduct) {
    const auto densities = sampleDensities();
    PhysicalRenderScaleKey previous = PhysicalRenderScaleKey::fromDensities(densities.front().first,
                                                                            densities.front().second);
    for (std::size_t i = 1; i < densities.size(); ++i) {
        const PhysicalRenderScaleKey current =
            PhysicalRenderScaleKey::fromDensities(densities[i].first, densities[i].second);
        CHECK(previous <= current);
        previous = current;
    }
}
