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
    // Physical device pixels per point. MUST be derived from the request's
    // TileKey: params.devicePixelsPerPoint == key.scale.scale() exactly (both
    // are the same quantized PhysicalRenderScaleKey value divided by the
    // denominator). DocumentRenderer rejects requests where they disagree, so
    // a cache entry's identity and its pixel dimensions can never diverge.
    double devicePixelsPerPoint = 1.0;
};

// A fully specified render job: cache identity + raster parameters.
struct RenderRequest {
    TileKey key;
    RasterParams params;
};

} // namespace rivet::render
