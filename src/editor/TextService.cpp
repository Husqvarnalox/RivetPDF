// SPDX-License-Identifier: MPL-2.0
#include "editor/TextService.hpp"

#include "editor/DocumentSession.hpp"

#include <utility>

namespace rivet::editor {

TextService::TextService(DocumentSession& session)
    : session_(session), executor_(session.scheduler()) {}

TextService::~TextService() {
    // Queued extraction jobs are cancelled (their callbacks dropped without
    // firing); the in-flight job runs to completion against the document,
    // which is still alive in the session's destruction order.
    executor_.cancelPending();
    // executor_ (member) destruction waits for the in-flight job.
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
    executor_.post([this, pageId] {
        std::vector<TextCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find(pageId);
            if (it == pending_.end()) return; // nothing waiting (already delivered)
            callbacks = std::move(it->second);
            pending_.erase(it);
        }

        // Cache probe again: another job may have produced the page.
        auto page = cachedTextPage(pageId);
        if (page == nullptr) {
            const std::size_t pageIndex = session_.pageIndexFor(pageId);
            if (pageIndex != DocumentSession::kInvalidPage) {
                page = session_.document().textPage(pageIndex).value_or(nullptr);
            }
            if (page != nullptr) {
                cache_.put(pageId, session_.revision(), page);
            }
        }

        if (callbacks.empty()) return;
        core::IMainThreadDispatcher* dispatcher = session_.mainDispatcher();
        if (dispatcher != nullptr) {
            dispatcher->post([callbacks = std::move(callbacks), page]() mutable {
                for (auto& callback : callbacks) callback(page);
            });
        } else {
            for (auto& callback : callbacks) callback(page);
        }
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
