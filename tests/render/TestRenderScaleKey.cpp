#include "RivetTest.h"

#include "render/RenderScaleKey.hpp"

#include <cmath>
#include <cstddef>
#include <limits>
#include <unordered_set>
#include <vector>

using rivet::render::RenderScaleKey;

namespace {

// Ascending, all within [kMinZoom, kMaxZoom].
std::vector<double> sampleZooms() {
    return {0.10, 0.125, 0.2,  0.31,  0.5,   0.64,  0.75, 0.99,
            1.0,  1.01,  1.24, 1.26,  2.0,   2.5,   3.33, 7.77,
            31.9, 63.9,  64.0 - 1e-9, 64.0};
}

} // namespace

RIVET_TEST(scaleKeyQuantizationCeils) {
    CHECK_EQ(RenderScaleKey::fromZoom(1.0).value, 64u); // exact multiple stays exact
    CHECK_EQ(RenderScaleKey::fromZoom(0.10).value, 7u); // ceil(6.4)
    CHECK_EQ(RenderScaleKey::fromZoom(1.5).value, 96u);
    CHECK_EQ(RenderScaleKey::fromZoom(2.0).value, 128u);
    CHECK_EQ(RenderScaleKey::fromZoom(64.0).value, 4096u);
}

RIVET_TEST(scaleKeyClampsToZoomBounds) {
    const RenderScaleKey minKey = RenderScaleKey::fromZoom(RenderScaleKey::kMinZoom);
    const RenderScaleKey maxKey = RenderScaleKey::fromZoom(RenderScaleKey::kMaxZoom);
    CHECK_EQ(RenderScaleKey::fromZoom(0.0).value, minKey.value);
    CHECK_EQ(RenderScaleKey::fromZoom(-3.0).value, minKey.value);
    CHECK_EQ(RenderScaleKey::fromZoom(1e9).value, maxKey.value);
    CHECK(RenderScaleKey::fromZoom(0.0) == minKey);
    CHECK(RenderScaleKey::fromZoom(1e9) == maxKey);
}

RIVET_TEST(scaleKeyNonFiniteMapsToZoomOne) {
    // Non-finite input is replaced by zoom 1.0 before clamping.
    const double inf = std::numeric_limits<double>::infinity();
    CHECK_EQ(RenderScaleKey::fromZoom(std::nan("")).value, 64u);
    CHECK_EQ(RenderScaleKey::fromZoom(inf).value, 64u);
    CHECK_EQ(RenderScaleKey::fromZoom(-inf).value, 64u);
}

RIVET_TEST(scaleKeyNeverBelowRequestedScale) {
    for (const double zoom : sampleZooms()) {
        const RenderScaleKey key = RenderScaleKey::fromZoom(zoom);
        CHECK_GE(key.scale(), zoom - 1e-12);
    }
}

RIVET_TEST(scaleKeyRoundtripWithinOneStep) {
    const double step = 1.0 / RenderScaleKey::kDenominator;
    for (const double zoom : sampleZooms()) {
        const RenderScaleKey key = RenderScaleKey::fromZoom(zoom);
        CHECK_NEAR(key.scale(), zoom, step + 1e-9);
    }
}

RIVET_TEST(scaleKeyMonotonic) {
    const std::vector<double> zooms = sampleZooms();
    RenderScaleKey previous = RenderScaleKey::fromZoom(zooms.front());
    for (std::size_t i = 1; i < zooms.size(); ++i) {
        const RenderScaleKey current = RenderScaleKey::fromZoom(zooms[i]);
        CHECK(previous <= current);
        previous = current;
    }
}

RIVET_TEST(scaleKeyOrderingAndHashing) {
    CHECK(RenderScaleKey::fromZoom(0.10) < RenderScaleKey::fromZoom(1.0));
    CHECK(RenderScaleKey::fromZoom(1.0) < RenderScaleKey::fromZoom(64.0));
    CHECK(RenderScaleKey::fromZoom(2.0) == RenderScaleKey::fromZoom(2.0));

    std::unordered_set<RenderScaleKey> keys;
    keys.insert(RenderScaleKey::fromZoom(0.10));
    keys.insert(RenderScaleKey::fromZoom(0.10)); // duplicate
    keys.insert(RenderScaleKey::fromZoom(1.0));
    keys.insert(RenderScaleKey::fromZoom(1.0)); // duplicate
    keys.insert(RenderScaleKey::fromZoom(2.0));
    CHECK_EQ(keys.size(), std::size_t{3});
    CHECK_EQ(keys.count(RenderScaleKey::fromZoom(1.0)), std::size_t{1});
    CHECK_EQ(keys.count(RenderScaleKey::fromZoom(4.0)), std::size_t{0});
}
