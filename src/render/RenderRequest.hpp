#pragma once

#include "core/geometry/Rect.hpp"
#include "render/TileKey.hpp"

namespace rivet::render {

// Pure raster parameters: what a PDF backend must draw. Deliberately free of
// cache identity so the PDF abstraction does not depend on render-cache types.
struct RasterParams {
    // Sub-rectangle of the rotated page in page display coordinates
    // (points, origin at the top-left of the displayed page, y-down).
    core::Rect pageRectPoints;
    // Physical device pixels per point (zoom * display backing scale).
    double devicePixelsPerPoint = 1.0;
};

// A fully specified render job: cache identity + raster parameters.
struct RenderRequest {
    TileKey key;
    RasterParams params;
};

} // namespace rivet::render
