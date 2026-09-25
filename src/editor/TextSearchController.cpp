// SPDX-License-Identifier: MPL-2.0
#include "editor/TextSearchController.hpp"

#include <utility>

namespace rivet::editor {

TextSearchController::TextSearchController(DocumentSession& session, TextService& text)
    : session_(session), text_(text), walkExecutor_(session.scheduler()) {}

TextSearchController::~TextSearchController() {
    // Invalidate the walk, then wait for the walker to observe it and exit:
    // the walker dereferences session_ and text_ between per-page checks, so
    // destroying them first would be use-after-free. The wait is bounded by
    // one page's extraction work.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        generation_.store(0, std::memory_order_release);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    walkerDone_.wait(lock, [this] { return !walkerActive_.load(std::memory_order_acquire); });
}

void TextSearchController::start(std::string query, pdf::TextSearchOptions options) {
    std::vector<Match> cleared;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        query_ = std::move(query);
        options_ = options;
        matches_.swap(cleared);
        currentIndex_.reset();
        if (query_.empty()) {
            // Empty query: cancel + clear, no walk.
            generation_.store(0, std::memory_order_release);
            fireChanged();
            return;
        }
        generation_.store(generation_.load(std::memory_order_relaxed) + 1,
                          std::memory_order_release);
    }
    const std::uint64_t generation = generation_.load(std::memory_order_acquire);
    walkExecutor_.post([this, generation, query = query_, options] {
        runWalk(generation, query, options);
    });
    fireChanged();
}

void TextSearchController::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_.store(0, std::memory_order_release);
}

void TextSearchController::runWalk(std::uint64_t generation, std::string query,
                                   pdf::TextSearchOptions options) {
    walkerActive_.store(true, std::memory_order_release);
    // All exits funnel through the end of this scope (see the walkerExit
    // lambda) so the destructor's wait cannot miss the heartbeat.
    auto walkerExit = [&] {
        walkerActive_.store(false, std::memory_order_release);
        walkerDone_.notify_all();
    };
    const std::size_t pageCount = session_.pageCount();
    std::vector<Match> fresh;
    for (std::size_t index = 0; index < pageCount; ++index) {
        // Cheap per-page cancellation check (also covers destruction, which
        // resets the generation to 0 while the walker is between pages).
        if (generation_.load(std::memory_order_acquire) != generation) {
            walkerExit();
            return;
        }

        const core::PageId pageId = session_.pageId(index);
        const std::shared_ptr<const pdf::PdfTextPage> page = text_.textPageNow(pageId);
        if (generation_.load(std::memory_order_acquire) != generation) {
            walkerExit();
            return;
        }
        if (page == nullptr) continue; // extraction failed: skip, not fatal

        const std::vector<pdf::TextSearchResult> found =
            pdf::searchTextPage(*page, query, options);
        if (generation_.load(std::memory_order_acquire) != generation) {
            walkerExit();
            return;
        }

        for (const pdf::TextSearchResult& result : found) {
            fresh.push_back(Match{pageId, result.startIndex, result.count});
        }

        // Publish incrementally. The callback is copied under the mutex and
        // fired outside it: after a generation check passes under the mutex,
        // the destructor cannot proceed until the walker releases it, so the
        // copy is safe to call.
        std::function<void()> notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation_.load(std::memory_order_acquire) != generation) {
                walkerExit();
                return;
            }
            matches_.insert(matches_.end(), fresh.begin(), fresh.end());
            fresh.clear();
            notify = onResultsChanged_;
        }
        if (notify) notify();
    }
    // Walk finished.
    std::function<void()> notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (generation_.load(std::memory_order_acquire) == generation) {
            generation_.store(0, std::memory_order_release);
            notify = onResultsChanged_;
        }
    }
    walkerExit();
    if (notify) notify();
}

std::vector<TextSearchController::Match> TextSearchController::matches() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return matches_;
}

void TextSearchController::next() { stepActive(+1); }
void TextSearchController::previous() { stepActive(-1); }

void TextSearchController::stepActive(int delta) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (matches_.empty()) {
        currentIndex_.reset();
        return;
    }
    const std::size_t count = matches_.size();
    if (!currentIndex_.has_value()) {
        currentIndex_ = delta > 0 ? std::optional<std::size_t>{0} : std::optional<std::size_t>{count - 1};
    } else {
        const long long next =
            (static_cast<long long>(*currentIndex_) + delta + static_cast<long long>(count)) %
            static_cast<long long>(count);
        currentIndex_ = static_cast<std::size_t>(next);
    }
    if (onResultsChanged_) onResultsChanged_();
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
    if (onResultsChanged_) onResultsChanged_();
}

void TextSearchController::fireChanged() {
    if (onResultsChanged_) onResultsChanged_();
}

void TextSearchController::setOnResultsChanged(std::function<void()> onResultsChanged) {
    std::lock_guard<std::mutex> lock(mutex_);
    onResultsChanged_ = std::move(onResultsChanged);
}

} // namespace rivet::editor
