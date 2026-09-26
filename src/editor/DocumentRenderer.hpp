#pragma once

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "render/RenderSource.hpp"
#include "render/TileCache.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace rivet::editor {

// Where a page's pixels come from: a page of some opened document presented
// through a view (see PdfPageGeometry.hpp), identified for caching by the
// page model's contentRevision. The shared_ptr keeps the source document
// alive for the queued job.
struct RenderPageTarget {
    std::shared_ptr<pdf::PdfDocument> document;
    std::size_t pageIndex = 0;
    pdf::PdfPageView view;
    std::uint64_t contentRevision = 0;
};

// Resolves a PageId to its current render target (nullopt = not in the
// document). Called on the thread that calls requestRender() (the main
// thread): the job only uses the captured target and never reads live
// page-model state on the worker.
using RenderPageResolver = std::function<std::optional<RenderPageTarget>(core::PageId)>;

// render::IRenderSource over an open PDF document: rasterizes tiles on a
// SerialExecutor (one job at a time per document, never on the main thread),
// stores results in the shared TileCache and delivers them to callers.
//
// Callback contract (every callback is invoked EXACTLY once):
//   - Validation failures fire inline on the calling thread; no job is
//     scheduled and nothing is recorded: InvalidArgument for a TileKey
//     belonging to another document or RasterParams not derived from the key
//     (params.devicePixelsPerPoint must equal key.scale.scale()); NotFound
//     for a PageId the resolver does not know (page deleted) or a key whose
//     contentRevision is not the page's current one (stale view).
//   - A tile that already FAILED for the current revision fires inline on the
//     calling thread with the recorded error, WITHOUT scheduling. Failed tiles
//     never re-enter the render pipeline (see failed-tile contract below).
//   - Cache hit at request time: the callback fires synchronously with the
//     cache-shared bitmap. No job is scheduled.
//   - Otherwise the request joins (or creates) a pending entry keyed by
//     (TileKey, revision); duplicate requests are coalesced onto that entry
//     and every registered callback receives the same outcome: the cache-shared
//     bitmap on success, the backend error on failure. Success results are
//     also put() into the TileCache.
//   - Threading: with a mainDispatcher, ALL callbacks fire posted on the main
//     thread; with a null dispatcher (tests) they fire inline on the worker
//     thread that produced the result (synchronous deliveries: inline on the
//     calling thread).
//
// Payload ownership: on success every callback receives the SAME
// shared_ptr<const core::Bitmap> the TileCache stores. Ownership is shared;
// the pixel data is never copied and must be treated as immutable by receivers.
//
// Failed-tile contract:
//   - A job that completes with an error (backend failure, OOM while
//     retaining the tile) records the error for its (TileKey,
//     revision) BEFORE delivering it. Later requestRender() calls for the same
//     key + revision replay that error inline instead of scheduling work - a
//     missing tile is re-requested on every repaint, and without the record
//     each of those would re-schedule a doomed job forever.
//   - Cancellations (cancelAll) are NOT failures and leave no record.
//   - A success erases any record for the key, so a retry that succeeds hands
//     subsequent requests over to the cache's hit path.
//   - setRevision() clears all records (a new revision keys new entries
//     anyway; clearing also bounds memory). retryFailedTiles() clears the
//     CURRENT revision's records so an explicitly requested retry schedules
//     fresh work.
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
// Lifetime: the destructor cancelAll()s and then waits until the dedicated
// executor stream has no queued or in-flight job left (SerialExecutor::
// waitUntilIdle). A render blocked inside the PDF backend blocks destruction,
// mirroring SerialExecutor's own wait for its in-flight task. The executor
// must be dedicated to this renderer, as it is in DocumentSession. After the
// destructor returns, no callback is pending and no worker touches this
// object. Source documents are kept alive by the captured RenderPageTarget;
// the cache, executor and scheduler references must outlive
// the renderer; DocumentSession declares its members so the renderer is
// destroyed first.
class DocumentRenderer final : public render::IRenderSource {
public:
    // resolvePage maps PageId -> render target (the session resolves through
    // its page model). mainDispatcher may be null (tests): callbacks then
    // fire inline on the worker thread.
    DocumentRenderer(core::DocumentId documentId,
                     RenderPageResolver resolvePage,
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
                       render::RenderCallback onDone) override;

    std::shared_ptr<const core::Bitmap> cachedTile(const render::TileKey& key,
                                                   std::uint64_t revision) const override;

    void cancelAll() override;

    // Current document revision used to stamp cache entries and to key pending
    // requests. Starts at 1.
    std::uint64_t revision() const;

    // Called by the owning session when the document content changes; new
    // requests render and cache under the new revision, and failure records
    // are cleared. Main-thread use.
    void setRevision(std::uint64_t revision);

    // Clears the failure records of the CURRENT revision so the next
    // requestRender for those tiles schedules fresh work. Call after fixing
    // whatever made tiles fail; repaints alone never retry a failed tile.
    // Main-thread use.
    void retryFailedTiles();

private:
    using Callback = render::RenderCallback;

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
        render::RasterParams params;
        RenderPageTarget target;
        render::RenderPriority priority = render::RenderPriority::Prefetch;
        std::uint64_t sequence = 0; // FIFO tie-break within one priority lane

        enum class State : std::uint8_t { Queued, InFlight };
        State state = State::Queued;
    };

    // Iterator to the queued entry to claim next: lowest priority value,
    // FIFO by sequence within a lane. Returns pending_.end() when nothing is
    // queued. Called with mutex_ held.
    std::unordered_map<PendingKey, PendingEntry, PendingKeyHash>::iterator pickNextQueued();

    // Posts exactly one drain task to the executor when none is scheduled or
    // running. The drain task claims queued entries highest-priority-first
    // (FIFO within a lane) and rasterizes them one at a time, so Visible
    // requests are never stuck behind a burst of Prefetch thumbnails.
    void scheduleDrain();

    // Executor task: claims and runs queued entries until none are left.
    void drainLoop();

    // Rasterizes one claimed entry (worker side). pdf::PdfDocument is touched
    // ONLY here - never on the caller's thread.
    void runJob(const render::TileKey& key, const render::RasterParams& params,
                const RenderPageTarget& target, std::uint64_t revision);

    // Worker-side completion: takes the pending entry's callbacks (erasing the
    // entry), records `error` in failed_ when the outcome is a failure (see
    // the failed-tile contract; cancellations never reach this path) and
    // delivers `bitmap` (success) or `error` to all callbacks. Safe to call
    // when the entry is already gone (cancelled before start): it then
    // delivers nothing.
    void completePending(const PendingKey& pendingKey,
                         const std::shared_ptr<const core::Bitmap>& bitmap,
                         const core::Error& error);

    // Runs `callbacks` with the outcome, on the main dispatcher when set,
    // otherwise inline on the calling (worker) thread. Static and independent
    // of `this`: posted deliveries stay safe during teardown. The bitmap is
    // shared, never copied.
    static void deliverCallbacks(std::vector<Callback> callbacks,
                                 const std::shared_ptr<const core::Bitmap>& bitmap,
                                 const core::Error& error,
                                 core::IMainThreadDispatcher* mainDispatcher);

    core::DocumentId documentId_;
    RenderPageResolver resolvePage_;
    render::TileCache& cache_;
    core::SerialExecutor& executor_;
    core::IMainThreadDispatcher* mainDispatcher_;

    // Guards pending_, failed_, currentRevision_, nextSequence_ and
    // drainScheduled_.
    mutable std::mutex mutex_;
    std::unordered_map<PendingKey, PendingEntry, PendingKeyHash> pending_;
    // Recorded job failures per (TileKey, revision): see the failed-tile
    // contract in the class comment.
    std::unordered_map<PendingKey, core::Error, PendingKeyHash> failed_;
    std::uint64_t currentRevision_ = 1;
    // Monotonic request sequence for FIFO tie-breaks within a priority lane.
    std::uint64_t nextSequence_ = 0;
    // True while a drain task is scheduled on (or running on) the executor
    // and has not yet consumed its scheduling slot. Prevents stacking drain
    // tasks; the executor serializes them anyway, the flag only avoids
    // redundant tasks.
    bool drainScheduled_ = false;
};

} // namespace rivet::editor
