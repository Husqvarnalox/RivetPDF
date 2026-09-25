// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "TextService.hpp"
#include "editor/DocumentSession.hpp"

#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "pdf/PdfText.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rivet::editor {

// Asynchronous, cancellable document text search over a TextService.
//
// Execution model: start() snapshots the reading order (the session's
// PageIds, main thread) and posts ONE walker task on a dedicated
// SerialExecutor. The walker takes each page's text via
// TextService::textPageNow() (cache probe or a synchronous extraction on the
// walker thread; the global PDFium gate keeps this safe alongside render
// jobs), runs searchTextPage (Unicode-aware, Rivet-owned case folding - no
// ICU) and appends the matches.
//
// Request identity: every start() and cancel() mints a NEW token from a
// monotonic counter that is never reset or reused (A = 1, cancel = 2,
// B = 3, ...). A walker only publishes while the active token still equals
// the token it was started with, so an obsolete walker can never mistake a
// later request for its own (no ABA on cancel/restart). "Is a walk running"
// is a separate flag (searching()), not encoded in the token.
//
// Notifications (the thread contract):
//   - The walker NEVER invokes the results callback. It publishes under the
//     mutex and requests a notification, which is marshaled to the main
//     thread through the session's IMainThreadDispatcher and coalesced (at
//     most one notification is queued at a time). Without a dispatcher
//     (tests) worker-side progress is observable by polling only.
//   - Main-thread mutations (start/cancel/next/previous/setCurrentIndex)
//     notify synchronously on the calling (main) thread.
//   - The callback is NEVER invoked while the internal mutex is held, so it
//     may freely call back into matches()/currentIndex()/query()/
//     searching()/next()/... (re-entrancy is supported).
//
// Lifetime: the destructor (main thread) invalidates the active token,
// drops queued walkers, and waits until the executor stream is idle, so no
// walker can touch this object afterwards; notifications already posted to
// the dispatcher hold only a shared liveness flag and become no-ops.
//
// Ordering note (documented limitation): the walk is sequential from the
// first page in reading order.
//
// Main-thread API; the walker is the only worker-side code.
class TextSearchController {
public:
    struct Match {
        core::PageId page;
        std::uint32_t startIndex = 0;
        std::uint32_t count = 0;
    };

    TextSearchController(DocumentSession& session, TextService& text);
    ~TextSearchController();

    TextSearchController(const TextSearchController&) = delete;
    TextSearchController& operator=(const TextSearchController&) = delete;

    // Starts (replacing any running search). Empty query cancels + clears.
    void start(std::string query, pdf::TextSearchOptions options = {});
    // Stops the running walk (matches found so far are kept). No-op when idle.
    void cancel();
    // Cancels and clears matches, query and the active match.
    void reset();

    // True while a walk is in flight for the current request.
    bool searching() const;
    const std::string& query() const { return query_; }
    pdf::TextSearchOptions options() const { return options_; }

    // Snapshot of the current matches (grows while the walk progresses;
    // complete after searching() is false). Returns a copy under the mutex:
    // the walker mutates the vector concurrently.
    std::vector<Match> matches() const;
    std::size_t matchCount() const;

    // Active match navigation (wraps around). nullopt when no matches.
    std::optional<std::size_t> currentIndex() const;
    void next();
    void previous();
    void setCurrentIndex(std::optional<std::size_t> index);

    // Fired on the main thread whenever matches, the searching state or the
    // active index changed; the shell re-reads the state and updates the UI.
    void setOnResultsChanged(std::function<void()> onResultsChanged);

    // Test/diagnostic hook: the token of the active request. Monotonic.
    std::uint64_t activeRequestForTesting() const {
        return activeRequest_.load(std::memory_order_acquire);
    }

private:
    void runWalk(std::uint64_t request, std::vector<core::PageId> order, std::string query,
                 pdf::TextSearchOptions options);
    // Main thread: invokes the callback (copied under the mutex, called
    // outside it).
    void notifyNow();
    // Worker: queues one coalesced main-thread notification.
    void requestNotify();
    void stepActive(int delta);
    // Mints a fresh token and makes it active (invalidating any walker).
    std::uint64_t mintRequest();

    DocumentSession& session_;
    TextService& text_;
    core::IMainThreadDispatcher* dispatcher_ = nullptr;

    // Main-thread only.
    std::string query_;
    pdf::TextSearchOptions options_;

    mutable std::mutex mutex_;
    // Guarded by mutex_.
    std::vector<Match> matches_;
    std::optional<std::size_t> currentIndex_;
    bool searching_ = false;
    bool notifyQueued_ = false;
    std::function<void()> onResultsChanged_;

    // Monotonic token source (main thread) and the active token (read by
    // the walker for its cheap per-page check; written under mutex_).
    std::uint64_t lastRequest_ = 0;
    std::atomic<std::uint64_t> activeRequest_{0};

    // Shared with posted notifications: false once destruction started.
    std::shared_ptr<std::atomic<bool>> alive_;

    // Declared LAST so it is destroyed FIRST (the destructor body already
    // drains it; this ordering is the second line of defense): walkers
    // reference every member above.
    core::SerialExecutor walkExecutor_;
};

} // namespace rivet::editor
