#pragma once

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"

#include <functional>

namespace rivet::render {

// Abstract render service consumed by UI viewports. Implementations (editor
// layer) schedule rasterization through the shared TaskScheduler, serialize
// access per document, and own the tile cache.
class IRenderSource {
public:
    virtual ~IRenderSource() = default;

    // Requests rasterization. The callback is invoked exactly once with
    // either a bitmap or an error, on the main thread when a main-thread
    // dispatcher is configured (production), otherwise inline on the worker
    // thread (tests). Duplicate requests for the same TileKey + revision may
    // be coalesced by the implementation.
    virtual void requestRender(const RenderRequest& request,
                               RenderPriority priority,
                               std::function<void(core::Result<core::Bitmap>)> onDone) = 0;

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
