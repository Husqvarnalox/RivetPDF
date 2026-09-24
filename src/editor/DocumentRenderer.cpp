#include "editor/DocumentRenderer.hpp"

#include <chrono>
#include <cstring>
#include <exception>
#include <iterator>
#include <thread>
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
    // Wait until nothing is queued or running on it, so no worker can touch
    // this object once the destructor returns. A render blocked inside the
    // PDF backend blocks here, mirroring SerialExecutor's own in-flight wait.
    while (executor_.hasPendingWork()) {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void DocumentRenderer::requestRender(const render::RenderRequest& request,
                                     render::RenderPriority /*priority*/,
                                     Callback onDone) {
    if (!onDone) {
        return;
    }

    // A TileKey for another document is a wiring bug; fail loudly instead of
    // polluting the cache under the wrong identity.
    if (request.key.documentId != documentId_) {
        deliverCallbacks(std::vector<Callback>{std::move(onDone)}, nullptr,
                         core::Error{core::ErrorCode::InvalidArgument,
                                     "render request does not belong to this document", "editor"},
                         mainDispatcher_);
        return;
    }

    std::uint64_t revision;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        revision = currentRevision_;
    }

    // Fast path: already rendered for this revision -> synchronous delivery on
    // the calling thread, no job scheduled.
    if (auto cached = cache_.get(request.key, revision)) {
        deliverCallbacks(std::vector<Callback>{std::move(onDone)}, std::move(cached), core::Error{},
                         mainDispatcher_);
        return;
    }

    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const PendingKey pendingKey{request.key, revision};
        auto it = pending_.find(pendingKey);
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
            if (it->second.inFlight) {
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
}

void DocumentRenderer::postJob(const render::TileKey& key, const render::RasterParams& params,
                               std::uint64_t revision) {
    executor_.post([this, key, params, revision] {
        // Claim the entry, or bail out if the request was cancelled before the
        // job started (its callbacks already received Cancelled).
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_.find(PendingKey{key, revision});
            if (it == pending_.end()) {
                return;
            }
            it->second.inFlight = true;
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
        auto it = pending_.find(pendingKey);
        if (it != pending_.end()) {
            callbacks = std::move(it->second.callbacks);
            pending_.erase(it);
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
                auto payload = cloneBitmap(*bitmap);
                if (payload.has_value()) {
                    callback(std::move(payload));
                } else {
                    callback(std::unexpected<core::Error>(payload.error()));
                }
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

core::Result<core::Bitmap> DocumentRenderer::cloneBitmap(const core::Bitmap& source) {
    if (!source.isValid()) {
        return std::unexpected(
            core::Error{core::ErrorCode::InvalidArgument, "cannot clone an invalid bitmap", "editor"});
    }
    auto copy = core::Bitmap::create(source.width(), source.height(), source.stride());
    if (!copy.has_value()) {
        return std::unexpected(copy.error());
    }
    if (source.sizeBytes() > 0) {
        std::memcpy(copy->data(), source.data(), source.sizeBytes());
    }
    return copy;
}

} // namespace rivet::editor
