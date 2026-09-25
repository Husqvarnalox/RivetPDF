#include "editor/DocumentRenderer.hpp"

#include <exception>
#include <iterator>
#include <optional>
#include <utility>

namespace rivet::editor {

std::size_t DocumentRenderer::PendingKeyHash::operator()(const PendingKey& key) const noexcept {
    std::size_t h = std::hash<render::TileKey>{}(key.key);
    h ^= std::hash<std::uint64_t>{}(key.revision) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

DocumentRenderer::DocumentRenderer(core::DocumentId documentId,
                                   pdf::PdfDocument& document,
                                   std::function<std::size_t(core::PageId)> pageIndexForId,
                                   render::TileCache& cache,
                                   core::TaskScheduler& /*scheduler*/,
                                   core::SerialExecutor& executor,
                                   core::IMainThreadDispatcher* mainDispatcher)
    : documentId_(documentId),
      document_(document),
      pageIndexForId_(std::move(pageIndexForId)),
      cache_(cache),
      executor_(executor),
      mainDispatcher_(mainDispatcher) {}

DocumentRenderer::~DocumentRenderer() {
    cancelAll();
    // The executor stream is dedicated to this renderer (see class comment).
    // Block until nothing is queued or running on it, so no worker can touch
    // this object once the destructor returns. A render blocked inside the
    // PDF backend blocks here, mirroring SerialExecutor's own in-flight wait.
    executor_.waitUntilIdle();
}

void DocumentRenderer::requestRender(const render::RenderRequest& request,
                                     render::RenderPriority /*priority*/,
                                     Callback onDone) {
    if (!onDone) {
        return;
    }

    const auto reject = [this](Callback callback, const core::Error& error) {
        deliverCallbacks(std::vector<Callback>{std::move(callback)}, nullptr, error, mainDispatcher_);
    };

    // A TileKey for another document is a wiring bug; fail loudly instead of
    // polluting the cache under the wrong identity.
    if (request.key.documentId != documentId_) {
        reject(std::move(onDone),
               core::Error{core::ErrorCode::InvalidArgument,
                           "render request does not belong to this document", "editor"});
        return;
    }

    // Cache identity derives the raster parameters: the key's physical scale
    // IS the density the tile is rasterized at. Both sides hold the same
    // quantized PhysicalRenderScaleKey value, so the exact comparison is
    // well-defined (same numerator over the same constant denominator -> the
    // same double). A mismatch would let a cache entry's pixel dimensions
    // diverge from its identity; reject instead.
    if (request.params.devicePixelsPerPoint != request.key.scale.scale()) {
        reject(std::move(onDone),
               core::Error{core::ErrorCode::InvalidArgument,
                           "devicePixelsPerPoint does not match the tile key's physical scale",
                           "editor"});
        return;
    }

    std::uint64_t revision = 0;
    std::optional<core::Error> recordedFailure;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        revision = currentRevision_;
        if (const auto it = failed_.find(PendingKey{request.key, revision}); it != failed_.end()) {
            recordedFailure = it->second;
        }
    }
    if (recordedFailure.has_value()) {
        // Failed tiles never re-enter the render pipeline: replay the recorded
        // error inline, without scheduling (see the failed-tile contract). This
        // is what breaks the repaint -> request -> fail loop.
        deliverCallbacks(std::vector<Callback>{std::move(onDone)}, nullptr, *recordedFailure,
                         mainDispatcher_);
        return;
    }

    // Fast path: already rendered for this revision -> synchronous delivery on
    // the calling thread, sharing the cached bitmap, no job scheduled.
    if (auto cached = cache_.get(request.key, revision)) {
        deliverCallbacks(std::vector<Callback>{std::move(onDone)}, std::move(cached), core::Error{},
                         mainDispatcher_);
        return;
    }

    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const PendingKey pendingKey{request.key, revision};
        const auto it = pending_.find(pendingKey);
        if (it != pending_.end()) {
            // Same tile is already being produced: coalesce onto that entry.
            it->second.callbacks.push_back(std::move(onDone));
        } else {
            PendingEntry entry;
            entry.callbacks.push_back(std::move(onDone));
            pending_.emplace(pendingKey, std::move(entry));
            schedule = true;
        }
    }
    if (schedule) {
        postJob(request.key, request.params, revision);
    }
}

std::shared_ptr<const core::Bitmap> DocumentRenderer::cachedTile(const render::TileKey& key,
                                                                 std::uint64_t revision) const {
    return cache_.get(key, revision);
}

void DocumentRenderer::cancelAll() {
    // Drops queued-not-started tasks; the in-flight task keeps running.
    executor_.cancelPending();

    std::vector<Callback> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->second.state == PendingEntry::State::InFlight) {
                // Its job is running and delivers its own result normally.
                ++it;
                continue;
            }
            dropped.insert(dropped.end(),
                           std::make_move_iterator(it->second.callbacks.begin()),
                           std::make_move_iterator(it->second.callbacks.end()));
            it = pending_.erase(it);
        }
    }

    if (!dropped.empty()) {
        deliverCallbacks(std::move(dropped), nullptr,
                         core::Error{core::ErrorCode::Cancelled, "render request cancelled", "editor"},
                         mainDispatcher_);
    }
}

std::uint64_t DocumentRenderer::revision() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentRevision_;
}

void DocumentRenderer::setRevision(std::uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentRevision_ = revision;
    // Failure records are keyed by revision, so stale ones can never match new
    // requests; clearing them here bounds their memory instead.
    failed_.clear();
}

void DocumentRenderer::retryFailedTiles() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = failed_.begin(); it != failed_.end();) {
        if (it->first.revision == currentRevision_) {
            it = failed_.erase(it);
        } else {
            ++it;
        }
    }
}

void DocumentRenderer::postJob(const render::TileKey& key, const render::RasterParams& params,
                               std::uint64_t revision) {
    executor_.post([this, key, params, revision] {
        // Claim the entry, or bail out if the request was cancelled before the
        // job started (its callbacks already received Cancelled).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find(PendingKey{key, revision});
            if (it == pending_.end()) {
                return;
            }
            it->second.state = PendingEntry::State::InFlight;
        }

        // Another path may have produced the tile after this entry was created
        // (e.g. a request that raced a completing job); deliver from the cache
        // instead of rasterizing twice.
        if (auto cached = cache_.get(key, revision)) {
            completePending(PendingKey{key, revision}, std::move(cached), core::Error{});
            return;
        }

        const std::size_t pageIndex = pageIndexForId_(key.pageId);
        if (pageIndex == kInvalidPageIndex) {
            completePending(PendingKey{key, revision}, nullptr,
                            core::Error{core::ErrorCode::NotFound,
                                        "no PDF page matches the requested page id", "editor"});
            return;
        }

        // pdf::PdfDocument is only ever touched here, on the executor stream.
        core::Result<core::Bitmap> rendered = std::unexpected(
            core::Error{core::ErrorCode::Internal, "render job did not produce a result", "editor"});
        try {
            rendered = document_.renderPage(pageIndex, params.pageRectPoints, params.devicePixelsPerPoint);
        } catch (const std::exception& exception) {
            rendered = std::unexpected(
                core::Error{core::ErrorCode::Internal, exception.what(), "editor"});
        } catch (...) {
            rendered = std::unexpected(
                core::Error{core::ErrorCode::Internal, "unknown rasterization failure", "editor"});
        }

        std::shared_ptr<const core::Bitmap> stored;
        if (rendered.has_value()) {
            try {
                auto owned = std::make_shared<core::Bitmap>(std::move(*rendered));
                // May refuse (invalid or oversize bitmap); delivery proceeds
                // regardless - the caller still gets its tile.
                cache_.put(key, revision, owned);
                stored = std::move(owned);
            } catch (const std::exception& exception) {
                rendered = std::unexpected(
                    core::Error{core::ErrorCode::OutOfMemory, exception.what(), "editor"});
            } catch (...) {
                rendered = std::unexpected(
                    core::Error{core::ErrorCode::OutOfMemory, "failed to retain the rendered tile", "editor"});
            }
        }

        if (stored) {
            completePending(PendingKey{key, revision}, std::move(stored), core::Error{});
        } else {
            const core::Error error = rendered.has_value()
                ? core::Error{core::ErrorCode::OutOfMemory, "failed to retain the rendered tile", "editor"}
                : rendered.error();
            completePending(PendingKey{key, revision}, nullptr, error);
        }
    });
}

void DocumentRenderer::completePending(const PendingKey& pendingKey,
                                       const std::shared_ptr<const core::Bitmap>& bitmap,
                                       const core::Error& error) {
    std::vector<Callback> callbacks;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(pendingKey);
        if (it == pending_.end()) {
            // The entry is already gone (cancelled before its job started);
            // its callbacks already received Cancelled, deliver nothing.
            return;
        }
        callbacks = std::move(it->second.callbacks);
        pending_.erase(it);
        // Record failures BEFORE delivery so later requests replay the error
        // instead of scheduling (see the failed-tile contract). Cancellations
        // never reach this function. A success erases any record so
        // subsequent requests go through the cache's hit path.
        if (bitmap) {
            failed_.erase(pendingKey);
        } else {
            failed_.insert_or_assign(pendingKey, error);
        }
    }
    if (!callbacks.empty()) {
        deliverCallbacks(std::move(callbacks), bitmap, error, mainDispatcher_);
    }
}

void DocumentRenderer::deliverCallbacks(std::vector<Callback> callbacks,
                                        const std::shared_ptr<const core::Bitmap>& bitmap,
                                        const core::Error& error,
                                        core::IMainThreadDispatcher* mainDispatcher) {
    auto invokeAll = [callbacks = std::move(callbacks), bitmap, error]() mutable {
        for (auto& callback : callbacks) {
            if (!callback) {
                continue;
            }
            if (bitmap) {
                // Shared ownership, no pixel copy: receivers get the very
                // bitmap the cache stores, and must treat it as immutable.
                callback(bitmap);
            } else {
                callback(std::unexpected<core::Error>(error));
            }
        }
    };

    if (mainDispatcher != nullptr) {
        mainDispatcher->post(std::move(invokeAll));
    } else {
        invokeAll();
    }
}

} // namespace rivet::editor
