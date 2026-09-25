#pragma once

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"

#include <functional>
#include <memory>

namespace rivet::render {

// Delivery payload of requestRender: on success the CACHE-SHARED tile bitmap
// (shared ownership with the tile cache; treat it as immutable and read-only),
// on failure the reason. No copies of the pixel data are made on delivery.
using RenderResult = core::Result<std::shared_ptr<const core::Bitmap>>;
using RenderCallback = std::function<void(RenderResult)>;

// Abstract render service consumed by UI viewports. Implementations (editor
// layer) schedule rasterization through the shared TaskScheduler, serialize
// access per document, and own the tile cache.
class IRenderSource {
public:
    virtual ~IRenderSource() = default;

    // Requests rasterization. The callback is invoked exactly once with either
    // the cache-shared bitmap or an error, on the main thread when a
    // main-thread dispatcher is configured (production), otherwise inline on
    // the calling/worker thread (tests). Duplicate requests for the same
    // TileKey + revision may be coalesced by the implementation.
    //
    // Synchronous rejections (never scheduled, delivered inline on the calling
    // thread): a request that fails validation, or one for a tile that already
    // FAILED for the current revision - implementations replay the recorded
    // error instead of re-scheduling, so a failing tile cannot turn into a
    // repaint->request->fail loop.
    virtual void requestRender(const RenderRequest& request,
                               RenderPriority priority,
                               RenderCallback onDone) = 0;

    // Synchronous cache probe for the paint path. Returns the cached tile
    // bitmap, or nullptr on miss/stale revision. Never schedules work and
    // never blocks; main-thread use expected.
    virtual std::shared_ptr<const core::Bitmap> cachedTile(const TileKey& key,
                                                           std::uint64_t revision) const = 0;

    // Drops all queued requests (results will report Cancelled or simply not
    // fire for already-completed work).
    virtual void cancelAll() = 0;
};

} // namespace rivet::render
