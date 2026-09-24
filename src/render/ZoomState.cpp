#include "render/ZoomState.hpp"

namespace rivet::render {

ZoomState::ZoomState(double initialZoom) {
    if (std::isnan(initialZoom)) initialZoom = 1.0;
    zoom_ = std::clamp(initialZoom, kMinZoom, kMaxZoom);
}

bool ZoomState::setZoom(double zoom) {
    if (std::isnan(zoom)) zoom = 1.0;
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    if (zoom == zoom_) return false;
    zoom_ = zoom;
    if (onZoomChanged_) onZoomChanged_(zoom_);
    return true;
}

bool ZoomState::zoomIn() {
    for (const double stop : kStops) {
        if (stop > zoom_) return setZoom(stop);
    }
    return false;
}

bool ZoomState::zoomOut() {
    for (auto it = kStops.rbegin(); it != kStops.rend(); ++it) {
        if (*it < zoom_) return setZoom(*it);
    }
    return false;
}

void ZoomState::actualSize() {
    setZoom(1.0);
}

void ZoomState::setCallback(std::function<void(double)> onZoomChanged) {
    onZoomChanged_ = std::move(onZoomChanged);
}

double ZoomState::fitWidthZoom(double viewportLogicalWidth, double pageWidthPoints) const {
    if (pageWidthPoints <= 0.0) return 1.0;
    double raw = viewportLogicalWidth / pageWidthPoints;
    if (std::isnan(raw)) raw = 1.0;
    return std::clamp(raw, kMinZoom, kMaxZoom);
}

double ZoomState::fitPageZoom(double viewportLogicalWidth, double viewportLogicalHeight,
                              const core::Size& pagePoints) const {
    if (pagePoints.width <= 0.0 || pagePoints.height <= 0.0) return 1.0;
    double raw = std::min(viewportLogicalWidth / pagePoints.width, viewportLogicalHeight / pagePoints.height);
    if (std::isnan(raw)) raw = 1.0;
    return std::clamp(raw, kMinZoom, kMaxZoom);
}

} // namespace rivet::render
