// SPDX-License-Identifier: MPL-2.0
#include "editor/TextService.hpp"

#include "editor/DocumentSession.hpp"

#include <string>
#include <utility>

namespace rivet::editor {

TextService::TextService(DocumentSession& session)
    : session_(session), dispatcher_(session.mainDispatcher()), executor_(session.scheduler()) {}

TextService::~TextService() {
    // Deliveries already posted become no-ops; a running range job notices
    // the flag between pages. Queued jobs are cancelled (their callbacks
    // dropped without firing); the in-flight job finishes against the
    // document, which is still alive in the session's destruction order.
    alive_->store(false, std::memory_order_release);
    executor_.cancelPending();
    executor_.waitUntilIdle();
}

void TextService::deliver(std::function<void()> task) {
    if (dispatcher_ == nullptr) {
        task();
        return;
    }
    dispatcher_->post([alive = alive_, task = std::move(task)] {
        if (alive->load(std::memory_order_acquire)) task();
    });
}

std::shared_ptr<const pdf::PdfTextPage> TextService::cachedTextPage(core::PageId pageId) const {
    return cache_.get(pageId, session_.revision());
}

void TextService::ensureTextPage(core::PageId pageId) {
    if (cachedTextPage(pageId) != nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& callbacks = pending_[pageId]; // creates the coalescing entry
        (void)callbacks;
    }
    scheduleExtraction(pageId);
}

void TextService::requestTextPage(core::PageId pageId, TextCallback onDone) {
    if (!onDone) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[pageId].push_back(std::move(onDone));
    }
    scheduleExtraction(pageId);
}

// Extraction job (dedicated executor stream). Coalescing: the pending entry
// accumulates every callback registered so far; the job takes them all and
// delivers the single outcome. A job posted twice for the same page (two
// requests racing) delivers once per entry - the second job finds no pending
// entry and becomes a no-op.
void TextService::scheduleExtraction(core::PageId pageId) {
    // The job captures everything it needs BY VALUE (document reference,
    // page index, revision, dispatcher): it must not dereference the session
    // while running, because it can outlive the calling stack and overlap
    // session/service teardown (the executor's idle-wait provides the final
    // ordering edge).
    pdf::PdfDocument& document = session_.document();
    const std::size_t pageIndex = session_.pageIndexFor(pageId);
    const std::uint64_t revision = session_.revision();
    executor_.post([this, pageId, pageIndex, revision, &document] {
        std::vector<TextCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find(pageId);
            if (it == pending_.end()) return; // nothing waiting (already delivered)
            callbacks = std::move(it->second);
            pending_.erase(it);
        }

        // Cache probe again: another job may have produced the page.
        auto page = cache_.get(pageId, revision);
        if (page == nullptr && pageIndex != DocumentSession::kInvalidPage) {
            page = document.textPage(pageIndex).value_or(nullptr);
            if (page != nullptr) {
                cache_.put(pageId, revision, page);
            }
        }

        if (callbacks.empty()) return;
        deliver([callbacks = std::move(callbacks), page]() mutable {
            for (auto& callback : callbacks) callback(page);
        });
    });
}

void TextService::requestRangesText(std::vector<TextRange> ranges, RangesTextCallback onDone) {
    if (!onDone) return;
    // Resolve everything the job needs on the calling (main) thread; the job
    // never reads session state.
    struct Resolved {
        TextRange range;
        std::size_t pageIndex = 0;
        std::size_t readingPosition = 0;
    };
    std::vector<Resolved> resolved;
    resolved.reserve(ranges.size());
    for (const TextRange& range : ranges) {
        const std::size_t index = session_.pageIndexFor(range.page);
        resolved.push_back(Resolved{range, index, index});
    }
    pdf::PdfDocument& document = session_.document();
    const std::uint64_t revision = session_.revision();
    executor_.post([this, resolved = std::move(resolved), &document, revision,
                    onDone = std::move(onDone), alive = alive_]() mutable {
        std::string out;
        core::Result<std::string> result = std::string{};
        for (std::size_t i = 0; i < resolved.size(); ++i) {
            if (!alive->load(std::memory_order_acquire)) return; // owner is going away
            const Resolved& item = resolved[i];
            std::shared_ptr<const pdf::PdfTextPage> page = cache_.get(item.range.page, revision);
            if (page == nullptr && item.pageIndex != DocumentSession::kInvalidPage) {
                auto extracted = document.textPage(item.pageIndex);
                if (extracted.has_value() && *extracted != nullptr) {
                    page = std::move(*extracted);
                    cache_.put(item.range.page, revision, page);
                }
            }
            if (page == nullptr) {
                const std::string where = item.readingPosition == DocumentSession::kInvalidPage
                                              ? std::string("a page that is no longer in the document")
                                              : "page " + std::to_string(item.readingPosition + 1);
                result = std::unexpected(core::Error{core::ErrorCode::InvalidDocument,
                                                     "could not read the text of " + where, "editor"});
                break;
            }
            appendRangeText(*page, item.range.begin, item.range.end, out);
            if (i + 1 < resolved.size()) out.push_back('\n'); // page break
        }
        if (result.has_value()) result = std::move(out);
        deliver([onDone = std::move(onDone), result = std::move(result)]() mutable {
            onDone(std::move(result));
        });
    });
}

std::shared_ptr<const pdf::PdfTextPage> TextService::textPageNow(core::PageId pageId) {
    if (auto cached = cachedTextPage(pageId)) return cached;
    const std::size_t pageIndex = session_.pageIndexFor(pageId);
    if (pageIndex == DocumentSession::kInvalidPage) return nullptr;
    auto page = session_.document().textPage(pageIndex).value_or(nullptr);
    if (page != nullptr) {
        cache_.put(pageId, session_.revision(), page);
    }
    return page;
}

} // namespace rivet::editor
