// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "TextPageCache.hpp"

#include "core/async/SerialExecutor.hpp"

// Forward declaration: TextService holds only a reference; the full
// definition is needed by the .cpp (including DocumentSession.hpp here would
// cycle - the session owns a TextService member).
namespace rivet::editor {
class DocumentSession;
}

#include <cstdint>
#include <functional>
#include <mutex>
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
// Lifetime: owned by DocumentSession (declared so it dies before the
// document handle). The destructor cancels queued extraction jobs and waits
// for the in-flight one; pending callbacks are dropped WITHOUT firing (the
// owning view is being destroyed; its callbacks are alive-flag-guarded
// anyway). No use-after-free callbacks.
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
    TextPageCache cache_;
    core::SerialExecutor executor_;

    std::mutex mutex_;
    std::unordered_map<core::PageId, std::vector<TextCallback>> pending_;
};

} // namespace rivet::editor
