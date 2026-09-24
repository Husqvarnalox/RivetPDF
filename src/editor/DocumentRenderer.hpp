#pragma once

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfEngine.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "render/RenderSource.hpp"
#include "render/TileCache.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rivet::editor {

// Sentinel returned by the pageIndexForId mapping when a PageId is unknown.
// A request whose page cannot be mapped completes with ErrorCode::NotFound
// instead of reaching the PDF backend.
inline constexpr std::size_t kInvalidPageIndex = static_cast<std::size_t>(-1);

// render::IRenderSource over an open PDF document: rasterizes tiles on a
// SerialExecutor (one job at a time per document, never on the main thread),
// stores results in the shared TileCache and delivers them to callers.
//
// Callback contract (every callback is invoked EXACTLY once):
//   - Cache hit at request time: the callback fires synchronously, on the
//     calling thread, with a copy of the cached bitmap. No job is scheduled.
//   - Otherwise the request joins (or creates) a pending entry keyed by
//     (TileKey, revision); duplicate requests are coalesced onto that entry
//     and every registered callback receives the same outcome: the bitmap on
//     success, the backend error on failure, ErrorCode::NotFound for an
//     unmapped PageId, ErrorCode::InvalidArgument for a TileKey belonging to
//     another document. Success results are also put() into the TileCache.
//   - Threading: with a mainDispatcher, callbacks fire posted on the main
//     thread; with a null dispatcher (tests) they fire inline on the worker
//     thread that produced the result. Cache hits and synchronous rejections
//     always fire inline on the calling thread.
//
// Cancellation contract (cancelAll):
//   - Queued, not-yet-started requests are dropped and their callbacks receive
//     ErrorCode::Cancelled (main dispatcher, or inline if null).
//   - The in-flight job is not interrupted: it runs to completion, still
//     populates the cache, and delivers its result to its callbacks normally.
//
// Revisions: results are stamped with the renderer's current revision (see
// setRevision). A tile rendered under revision X is invisible to cachedTile()
// and to later requestRender() calls once the revision moves past X.
//
// Lifetime: the destructor cancelAll()s and then WAITS until the dedicated
// executor stream has no queued or in-flight job left (a render blocked inside
// the PDF backend blocks destruction, mirroring SerialExecutor's own wait for
// its in-flight task). The executor must be dedicated to this renderer, as it
// is in DocumentSession. After the destructor returns, no callback is pending
// and no worker touches this object. The document, cache, executor and
// scheduler references must outlive the renderer; DocumentSession declares its
// members so the renderer is destroyed first.
class DocumentRenderer final : public render::IRenderSource {
public:
    // pageIndexForId maps PageId -> zero-based PDF page index (the session
    // owns the mapping); kInvalidPageIndex marks an unknown id. mainDispatcher
    // may be null (tests): callbacks then fire inline on the worker thread.
    DocumentRenderer(core::DocumentId documentId,
                     pdf::PdfDocument& document,
                     std::function<std::size_t(core::PageId)> pageIndexForId,
                     render::TileCache& cache,
                     core::TaskScheduler& scheduler,
                     core::SerialExecutor& executor,
                     core::IMainThreadDispatcher* mainDispatcher);

    // cancelAll() + wait until the executor stream is idle (see class comment).
    ~DocumentRenderer() override;

    DocumentRenderer(const DocumentRenderer&) = delete;
    DocumentRenderer& operator=(const DocumentRenderer&) = delete;

    void requestRender(const render::RenderRequest& request,
                       render::RenderPriority priority,
                       std::function<void(core::Result<core::Bitmap>)> onDone) override;

    std::shared_ptr<const core::Bitmap> cachedTile(const render::TileKey& key,
                                                   std::uint64_t revision) const override;

    void cancelAll() override;

    // Current document revision used to stamp cache entries and to key pending
    // requests. Starts at 1.
    std::uint64_t revision() const;

    // Called by the owning session when the document content changes; new
    // requests render and cache under the new revision. Main-thread use.
    void setRevision(std::uint64_t revision);

private:
    using Callback = std::function<void(core::Result<core::Bitmap>)>;

    struct PendingKey {
        render::TileKey key;
        std::uint64_t revision = 0;

        bool operator==(const PendingKey&) const = default;
    };

    struct PendingKeyHash {
        std::size_t operator()(const PendingKey& key) const noexcept;
    };

    struct PendingEntry {
        std::vector<Callback> callbacks;
        bool inFlight = false; // true once its job started executing
    };

    // Schedules the rasterization job for one pending entry. pdf::PdfDocument
    // is touched ONLY here, inside the SerialExecutor job - never on the
    // caller's thread.
    void postJob(const render::TileKey& key, const render::RasterParams& params, std::uint64_t revision);

    // Worker-side completion: takes the pending entry's callbacks (erasing the
    // entry) and delivers `bitmap` (success) or `error` to all of them. Safe to
    // call when the entry is already gone (cancelled before start): it then
    // delivers nothing.
    void completePending(const PendingKey& pendingKey,
                         const std::shared_ptr<const core::Bitmap>& bitmap,
                         const core::Error& error);

    // Runs `callbacks` with the outcome, on the main dispatcher when set,
    // otherwise inline on the calling (worker) thread. Static and independent
    // of `this`: posted deliveries stay safe during teardown.
    static void deliverCallbacks(std::vector<Callback> callbacks,
                                 const std::shared_ptr<const core::Bitmap>& bitmap,
                                 const core::Error& error,
                                 core::IMainThreadDispatcher* mainDispatcher);

    // Deep-copies a cached/stored bitmap so it can be handed out as an owned
    // Result<core::Bitmap> (the interface moves bitmaps, the cache shares them).
    static core::Result<core::Bitmap> cloneBitmap(const core::Bitmap& source);

    core::DocumentId documentId_;
    pdf::PdfDocument& document_;
    std::function<std::size_t(core::PageId)> pageIndexForId_;
    render::TileCache& cache_;
    core::SerialExecutor& executor_;
    core::IMainThreadDispatcher* mainDispatcher_;

    // Guards pending_ and currentRevision_.
    mutable std::mutex mutex_;
    std::unordered_map<PendingKey, PendingEntry, PendingKeyHash> pending_;
    std::uint64_t currentRevision_ = 1;
};

} // namespace rivet::editor
