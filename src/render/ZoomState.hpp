#pragma once

#include "core/geometry/Size.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>

namespace rivet::render {

// User-facing zoom level: clamping, preset stops for stepped zooming and fit
// intent. Not thread-safe; owned by the UI (main) thread.
class ZoomState {
public:
    static constexpr double kMinZoom = 0.10;
    static constexpr double kMaxZoom = 64.0;

    explicit ZoomState(double initialZoom = 1.0); // clamped like setZoom

    double zoom() const { return zoom_; }

    // Clamps to [kMinZoom, kMaxZoom] (NaN maps to 1.0; infinities land on the
    // bounds). Returns true if the value actually changed.
    bool setZoom(double zoom);

    // Next/previous preset stop strictly above/below the current zoom.
    // Returns false (no change) when already at or beyond the end stop.
    bool zoomIn();
    bool zoomOut();

    void actualSize();

    enum class FitMode : std::uint8_t { None, Width, Page };

    FitMode fitMode() const { return fitMode_; }

    // Stored intent only; geometry owners resolve it via fitWidthZoom /
    // fitPageZoom and feed the result back through setZoom.
    void setFitMode(FitMode mode) { fitMode_ = mode; }

    // viewportLogicalWidth / pageWidthPoints, clamped. pageWidthPoints <= 0
    // yields 1.0.
    double fitWidthZoom(double viewportLogicalWidth, double pageWidthPoints) const;

    // Min of the per-axis fits, clamped. An empty page yields 1.0.
    double fitPageZoom(double viewportLogicalWidth, double viewportLogicalHeight,
                       const core::Size& pagePoints) const;

    // Invoked on the calling thread whenever the zoom actually changes.
    void setCallback(std::function<void(double)> onZoomChanged);

private:
    static constexpr std::array<double, 20> kStops{
        0.10, 0.25, 0.33, 0.50, 0.67, 0.75, 1.0, 1.25, 1.5, 2.0,
        3.0,  4.0,  6.0,  8.0,  12.0, 16.0, 24.0, 32.0, 48.0, 64.0};

    double zoom_ = 1.0;
    FitMode fitMode_ = FitMode::None;
    std::function<void(double)> onZoomChanged_;
};

} // namespace rivet::render
