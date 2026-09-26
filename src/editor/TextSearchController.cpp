// SPDX-License-Identifier: MPL-2.0
#include "editor/TextSearchController.hpp"

#include <utility>

namespace rivet::editor {

TextSearchController::TextSearchController(DocumentSession& session, TextService& text)
    : session_(session),
      text_(text),
      dispatcher_(session.mainDispatcher()),
      alive_(std::make_shared<std::atomic<bool>>(true)),
      walkExecutor_(session.scheduler()) {}

TextSearchController::~TextSearchController() {
    // Posted notifications become no-ops from here on (they run on the main
    // thread, like this destructor, so the flag is race-free).
    alive_->store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        activeRequest_.store(++lastRequest_, std::memory_order_release);
    }
    // Drop walkers that have not started and wait for the in-flight one to
    // observe the invalidated token (bounded by one page's work). Waiting on
    // the EXECUTOR - not on a "walker running" flag - is what guarantees no
    // queued walker can start after this point and touch a dead object.
    walkExecutor_.cancelPending();
    walkExecutor_.waitUntilIdle();
}

std::uint64_t TextSearchController::mintRequest() {
    // Main thread only; the store happens under mutex_ so publication checks
    // inside the walker (also under mutex_) see a consistent token.
    const std::uint64_t request = ++lastRequest_;
    activeRequest_.store(request, std::memory_order_release);
    return request;
}

void TextSearchController::start(std::string query, pdf::TextSearchOptions options) {
    query_ = std::move(query);
    options_ = options;
    std::uint64_t request = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request = mintRequest();
        matches_.clear();
        currentIndex_.reset();
        searching_ = !query_.empty();
    }
    // Queued walkers of earlier requests are obsolete: drop them instead of
    // letting each one start just to notice its token is stale.
    walkExecutor_.cancelPending();
    if (!query_.empty()) {
        walkExecutor_.post([this, request, snapshot = session_.pageSnapshot(), query = query_,
                            options]() mutable {
            runWalk(request, std::move(snapshot), std::move(query), options);
        });
    }
    notifyNow();
}

void TextSearchController::cancel() {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mintRequest();
        changed = searching_;
        searching_ = false;
    }
    walkExecutor_.cancelPending();
    if (changed) notifyNow();
}

void TextSearchController::reset() {
    query_.clear();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mintRequest();
        searching_ = false;
        matches_.clear();
        currentIndex_.reset();
    }
    walkExecutor_.cancelPending();
    notifyNow();
}

bool TextSearchController::searching() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return searching_;
}

void TextSearchController::handlePageModelChanged(const PageModelChange& change) {
    (void)change; // every kind of change restarts (see header)
    if (query_.empty()) return;
    start(query_, options_);
}

void TextSearchController::runWalk(std::uint64_t request, PageSnapshotPtr snapshot,
                                   std::string query, pdf::TextSearchOptions options) {
    const auto stale = [&] { return activeRequest_.load(std::memory_order_acquire) != request; };
    std::vector<Match> fresh;
    for (const PageEntry& entry : snapshot->entries()) {
        const core::PageId pageId = entry.id;
        // Cheap per-page cancellation check (also covers destruction, which
        // invalidates the token before draining the executor).
        if (stale()) return;

        // The captured entry keeps its source document alive; edits made
        // meanwhile restart the search (handlePageModelChanged).
        const std::shared_ptr<const pdf::PdfTextPage> page = text_.textPageNow(entry);
        if (stale()) return;
        if (page == nullptr) continue; // extraction failed: skip, not fatal

        const std::vector<pdf::TextSearchResult> found = pdf::searchTextPage(*page, query, options);
        for (const pdf::TextSearchResult& result : found) {
            fresh.push_back(Match{pageId, result.startIndex, result.count});
        }
        if (fresh.empty()) continue;

        // Publish incrementally, re-checking the token under the mutex so a
        // start()/cancel() that raced this page can never see stale matches.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stale()) return;
            matches_.insert(matches_.end(), fresh.begin(), fresh.end());
        }
        fresh.clear();
        requestNotify();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stale()) return;
        searching_ = false;
    }
    requestNotify();
}

void TextSearchController::requestNotify() {
    if (dispatcher_ == nullptr) return; // tests without a main thread: poll
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (notifyQueued_) return; // coalesced into the pending notification
        notifyQueued_ = true;
    }
    // The posted task holds the liveness flag, never the object: after
    // destruction it is a no-op. It runs on the main thread, where the
    // destructor also runs, so the check cannot race the teardown.
    dispatcher_->post([this, alive = alive_] {
        if (!alive->load(std::memory_order_acquire)) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            notifyQueued_ = false;
        }
        notifyNow();
    });
}

void TextSearchController::notifyNow() {
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = onResultsChanged_;
    }
    if (callback) callback(); // outside the lock: re-entrancy is allowed
}

std::vector<TextSearchController::Match> TextSearchController::matches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return matches_;
}

std::size_t TextSearchController::matchCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return matches_.size();
}

std::optional<std::size_t> TextSearchController::currentIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentIndex_;
}

void TextSearchController::next() { stepActive(+1); }
void TextSearchController::previous() { stepActive(-1); }

void TextSearchController::stepActive(int delta) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (matches_.empty()) {
            currentIndex_.reset();
        } else {
            const std::size_t count = matches_.size();
            if (!currentIndex_.has_value() || *currentIndex_ >= count) {
                currentIndex_ = delta > 0 ? std::size_t{0} : count - 1;
            } else {
                const long long next = (static_cast<long long>(*currentIndex_) + delta +
                                        static_cast<long long>(count)) %
                                       static_cast<long long>(count);
                currentIndex_ = static_cast<std::size_t>(next);
            }
        }
    }
    notifyNow();
}

void TextSearchController::setCurrentIndex(std::optional<std::size_t> index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index && *index < matches_.size()) {
            currentIndex_ = index;
        } else {
            currentIndex_.reset();
        }
    }
    notifyNow();
}

void TextSearchController::setOnResultsChanged(std::function<void()> onResultsChanged) {
    std::lock_guard<std::mutex> lock(mutex_);
    onResultsChanged_ = std::move(onResultsChanged);
}

} // namespace rivet::editor
