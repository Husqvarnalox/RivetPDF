// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "TextService.hpp"
#include "editor/DocumentSession.hpp"

#include "core/StrongId.hpp"
#include "pdf/PdfText.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace rivet::editor {

// Asynchronous, cancellable document text search over a TextService.
//
// Execution model: start() posts ONE walker task (on the session scheduler,
// outside any SerialExecutor stream) that walks the pages in order. For each
// page it takes the text via TextService::textPageNow() (cache probe or a
// synchronous extraction on the walker thread; the global PDFium gate keeps
// this safe alongside render jobs), runs searchTextPage (Unicode-aware,
// Rivet-owned case folding - no ICU) and appends the matches. The walker
// checks its generation after every page: a new start() or cancel()
// invalidates the previous walk, so an obsolete search can never overwrite
// newer results and cancellation is cheap (bounded by one page's work).
//
// Results: the guarded match vector is updated incrementally and a
// notification fires (main thread, coalesced by the UI repaint). The UI
// reads matches()/currentIndex() directly - no snapshot copying per page.
//
// Ordering note (documented limitation): the walk is sequential from page 0.
// Prioritizing the currently visible pages first would complicate ordering
// for no measured benefit and is deferred.
//
// Main-thread API; the walker is the only worker-side code.
class TextSearchController {
public:
    struct Match {
        core::PageId page;
        std::uint32_t startIndex = 0;
        std::uint32_t count = 0;
    };

    // pageIndexForId resolves reading order for next/previous navigation.
    TextSearchController(DocumentSession& session, TextService& text);
    ~TextSearchController();

    TextSearchController(const TextSearchController&) = delete;
    TextSearchController& operator=(const TextSearchController&) = delete;

    // Starts (replacing any running search). Empty query cancels + clears.
    void start(std::string query, pdf::TextSearchOptions options = {});
    void cancel();

    // True while a walk is in flight.
    bool searching() const { return generation_.load(std::memory_order_acquire) != 0; }
    const std::string& query() const { return query_; }
    pdf::TextSearchOptions options() const { return options_; }

    // Snapshot of the current matches (grows while the walk progresses;
    // complete after searching() is false). Returns a copy under the mutex:
    // the walker mutates the vector concurrently.
    std::vector<Match> matches() const;

    // Active match navigation (wraps around). nullopt when no matches.
    std::optional<std::size_t> currentIndex() const { return currentIndex_; }
    void next();
    void previous();
    void setCurrentIndex(std::optional<std::size_t> index);

    // Fired (main thread) whenever matches or the active index changed; the
    // shell re-reads matches() and updates the UI.
    void setOnResultsChanged(std::function<void()> onResultsChanged);

private:
    void runWalk(std::uint64_t generation, std::string query, pdf::TextSearchOptions options);
    void fireChanged();
    void stepActive(int delta);

    DocumentSession& session_;
    TextService& text_;

    mutable std::mutex mutex_;
    std::vector<Match> matches_;
    std::optional<std::size_t> currentIndex_;
    std::string query_;
    pdf::TextSearchOptions options_;

    // 0 = idle; otherwise the in-flight walk's generation (post-increment
    // per start()). Atomic for the walker's cheap per-page check; writes
    // happen under mutex_.
    std::atomic<std::uint64_t> generation_{0};
    // Walker heartbeat: true while the walker task is running. The
    // destructor waits for it to drop (bounded by one page's work) because
    // the walker references session_ and text_ between generation checks.
    std::atomic<bool> walkerActive_{false};
    std::condition_variable walkerDone_;
    std::function<void()> onResultsChanged_;
};

} // namespace rivet::editor
