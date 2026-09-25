// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "SelectionText.hpp"
#include "TextPageCache.hpp"

#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"

// Forward declaration: TextService holds only a reference; the full
// definition is needed by the .cpp (including DocumentSession.hpp here would
// cycle - the session owns a TextService member).
namespace rivet::editor {
class DocumentSession;
}

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rivet::editor {

// Editor-side text pipeline for one document: byte-bounded TextPageCache +
// serialized extraction on a DEDICATED SerialExecutor (never the UI thread,
// never the renderer's stream - cancelling text work must not touch render
// work). PDFium itself stays serialized by the adapter's global gate.
//
// Entry points:
//   - cachedTextPage(): synchronous cache probe (main thread).
//   - ensureTextPage(): fire-and-forget extraction; used to warm the pages
//     under the caret/selection so mouse interaction hits a warm cache.
//   - requestTextPage(): extraction + exactly-once callback delivery on the
//     main thread (inline on the worker in tests without a dispatcher).
//     Duplicate requests for the same page are coalesced.
//   - textPageNow(): worker-thread path for the search walker: cache probe
//     or a synchronous extraction on the CALLING thread (the global PDFium
//     gate makes this safe alongside render jobs). Never call from the main
//     thread.
//
//   - requestRangesText(): the exact UTF-8 text of an ordered list of page
//     ranges (a text selection). Every page is taken from the cache or
//     extracted on the worker - pages are NEVER skipped; the first page that
//     cannot be extracted fails the whole request with an error naming it.
//     Memory: only the assembled string plus one page at a time.
//
// Lifetime: owned by DocumentSession (declared so it dies before the
// document handle). The destructor flags cancellation (a long range job
// stops between pages), cancels queued jobs and waits for the in-flight
// one. Pending callbacks are dropped WITHOUT firing: deliveries already
// posted to the dispatcher check a liveness flag first, so no callback runs
// after the service (and therefore its owner's session) is gone.
class TextService {
public:
    // Delivered exactly once per accepted request: the page on success.
    // (Failures deliver a null pointer; per spec, extraction failure is a
    // non-fatal feature error, not a document error.)
    using TextCallback = std::function<void(std::shared_ptr<const pdf::PdfTextPage>)>;

    explicit TextService(DocumentSession& session);
    ~TextService();

    TextService(const TextService&) = delete;
    TextService& operator=(const TextService&) = delete;

    std::shared_ptr<const pdf::PdfTextPage> cachedTextPage(core::PageId pageId) const;

    // Cache hit -> no work. Otherwise schedules extraction (coalesced with
    // any pending request for the same page).
    void ensureTextPage(core::PageId pageId);

    void requestTextPage(core::PageId pageId, TextCallback onDone);

    // See the class comment. onDone is delivered exactly once on the main
    // thread (inline on the worker without a dispatcher), unless the service
    // is destroyed first.
    using RangesTextCallback = std::function<void(core::Result<std::string>)>;
    void requestRangesText(std::vector<TextRange> ranges, RangesTextCallback onDone);

    // Worker-thread extraction (search path). Cache probe first; on a miss
    // extracts synchronously on the calling thread and caches the result.
    // Returns null on failure.
    std::shared_ptr<const pdf::PdfTextPage> textPageNow(core::PageId pageId);

    // The byte budget of the underlying cache (diagnostics/tests).
    std::size_t cacheMaxBytes() const { return cache_.maxBytes(); }
    TextPageCache& cache() { return cache_; }

private:
    void scheduleExtraction(core::PageId pageId);

    DocumentSession& session_;
    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    TextPageCache cache_;
    core::SerialExecutor executor_;

    // Runs `deliver` on the main thread (dispatcher) unless the service died
    // meanwhile; inline on the calling worker without a dispatcher.
    void deliver(std::function<void()> deliver);

    std::mutex mutex_;
    std::unordered_map<core::PageId, std::vector<TextCallback>> pending_;

    // False once destruction started; shared with posted deliveries.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace rivet::editor
