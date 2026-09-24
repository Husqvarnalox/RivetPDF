#include "RivetTest.h"

#include "core/geometry/Size.hpp"
#include "render/ZoomState.hpp"

#include <cmath>

using rivet::core::Size;
using rivet::render::ZoomState;

RIVET_TEST(zoomInitialState) {
    const ZoomState defaults;
    CHECK_NEAR(defaults.zoom(), 1.0, 1e-12);
    CHECK(defaults.fitMode() == ZoomState::FitMode::None);

    const ZoomState initial(2.5);
    CHECK_NEAR(initial.zoom(), 2.5, 1e-12);

    // Initial values clamp like setZoom.
    const ZoomState clampedLow(0.01);
    CHECK_NEAR(clampedLow.zoom(), ZoomState::kMinZoom, 1e-12);
    const ZoomState clampedHigh(1000.0);
    CHECK_NEAR(clampedHigh.zoom(), ZoomState::kMaxZoom, 1e-12);
}

RIVET_TEST(zoomSetClampsAndReportsChange) {
    ZoomState zoom;
    CHECK(zoom.setZoom(-1.0));
    CHECK_NEAR(zoom.zoom(), ZoomState::kMinZoom, 1e-12);
    CHECK(!zoom.setZoom(0.02)); // clamps to the same minimum, no change

    CHECK(zoom.setZoom(100.0));
    CHECK_NEAR(zoom.zoom(), ZoomState::kMaxZoom, 1e-12);
    CHECK(!zoom.setZoom(1e9)); // clamps to the same maximum, no change

    CHECK(zoom.setZoom(1.5));
    CHECK_NEAR(zoom.zoom(), 1.5, 1e-12);
    CHECK(!zoom.setZoom(1.5));
}

RIVET_TEST(zoomSteppedWalk) {
    ZoomState zoom; // starts at 1.0
    CHECK(zoom.zoomIn());
    CHECK_NEAR(zoom.zoom(), 1.25, 1e-12);
    CHECK(zoom.zoomIn());
    CHECK_NEAR(zoom.zoom(), 1.5, 1e-12);
    CHECK(zoom.zoomOut());
    CHECK_NEAR(zoom.zoom(), 1.25, 1e-12);
    CHECK(zoom.zoomOut());
    CHECK_NEAR(zoom.zoom(), 1.0, 1e-12);
}

RIVET_TEST(zoomStopsAtEnds) {
    ZoomState low(ZoomState::kMinZoom);
    CHECK(!low.zoomOut());
    CHECK_NEAR(low.zoom(), ZoomState::kMinZoom, 1e-12);
    CHECK(low.zoomIn());
    CHECK_NEAR(low.zoom(), 0.25, 1e-12);

    ZoomState high(ZoomState::kMaxZoom);
    CHECK(!high.zoomIn());
    CHECK_NEAR(high.zoom(), ZoomState::kMaxZoom, 1e-12);
    CHECK(high.zoomOut());
    CHECK_NEAR(high.zoom(), 48.0, 1e-12);
}

RIVET_TEST(zoomSteppedFromBetweenStops) {
    ZoomState below(0.9);
    CHECK(below.zoomIn());
    CHECK_NEAR(below.zoom(), 1.0, 1e-12);

    ZoomState above(1.1);
    CHECK(above.zoomIn());
    CHECK_NEAR(above.zoom(), 1.25, 1e-12);

    ZoomState between(1.1);
    CHECK(between.zoomOut());
    CHECK_NEAR(between.zoom(), 1.0, 1e-12);

    // Exactly on a stop: steps to the next stop strictly above/below.
    ZoomState onStop(1.25);
    CHECK(onStop.zoomIn());
    CHECK_NEAR(onStop.zoom(), 1.5, 1e-12);
    CHECK(onStop.zoomOut());
    CHECK_NEAR(onStop.zoom(), 1.25, 1e-12);
}

RIVET_TEST(zoomActualSize) {
    ZoomState zoom(3.0);
    zoom.actualSize();
    CHECK_NEAR(zoom.zoom(), 1.0, 1e-12);
}

RIVET_TEST(zoomFitWidth) {
    const ZoomState zoom;
    CHECK_NEAR(zoom.fitWidthZoom(600.0, 612.0), 600.0 / 612.0, 1e-12);
    CHECK_NEAR(zoom.fitWidthZoom(10000.0, 612.0), 10000.0 / 612.0, 1e-12); // within range
    CHECK_NEAR(zoom.fitWidthZoom(100.0, 1.0), ZoomState::kMaxZoom, 1e-12); // clamped high
    CHECK_NEAR(zoom.fitWidthZoom(10.0, 10000.0), ZoomState::kMinZoom, 1e-12); // clamped low
    CHECK_NEAR(zoom.fitWidthZoom(600.0, 0.0), 1.0, 1e-12);
    CHECK_NEAR(zoom.fitWidthZoom(600.0, -5.0), 1.0, 1e-12);
}

RIVET_TEST(zoomFitPage) {
    const ZoomState zoom;
    // min(600/612, 800/792) = 600/612.
    CHECK_NEAR(zoom.fitPageZoom(600.0, 800.0, Size{612.0, 792.0}), 600.0 / 612.0, 1e-12);
    // min(500/612, 400/792) = 400/792.
    CHECK_NEAR(zoom.fitPageZoom(500.0, 400.0, Size{612.0, 792.0}), 400.0 / 792.0, 1e-12);
    CHECK_NEAR(zoom.fitPageZoom(600.0, 800.0, Size{0.0, 792.0}), 1.0, 1e-12);
    CHECK_NEAR(zoom.fitPageZoom(600.0, 800.0, Size{612.0, 0.0}), 1.0, 1e-12);
}

RIVET_TEST(zoomCallbackFiresOnChangeOnly) {
    int calls = 0;
    double lastValue = 0.0;

    ZoomState zoom;
    zoom.setCallback([&calls, &lastValue](double value) {
        ++calls;
        lastValue = value;
    });

    CHECK(zoom.setZoom(2.0));
    CHECK_EQ(calls, 1);
    CHECK_NEAR(lastValue, 2.0, 1e-12);

    CHECK(!zoom.setZoom(2.0)); // no change, no callback
    CHECK_EQ(calls, 1);

    CHECK(zoom.zoomIn()); // 2.0 -> 3.0
    CHECK_EQ(calls, 2);
    CHECK_NEAR(lastValue, 3.0, 1e-12);

    CHECK(zoom.zoomOut()); // 3.0 -> 2.0
    CHECK_EQ(calls, 3);

    zoom.actualSize(); // 2.0 -> 1.0
    CHECK_EQ(calls, 4);
    CHECK_NEAR(lastValue, 1.0, 1e-12);

    zoom.actualSize(); // already 1.0, no callback
    CHECK_EQ(calls, 4);

    zoom.setFitMode(ZoomState::FitMode::Width);
    CHECK(zoom.fitMode() == ZoomState::FitMode::Width);
    CHECK(zoom.setZoom(2.0)); // fit mode changes do not fire; setZoom does
    CHECK_EQ(calls, 5);
}
