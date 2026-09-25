// SPDX-License-Identifier: MPL-2.0
#include "editor/LinkService.hpp"

#include "editor/DocumentSession.hpp"

#include <utility>

namespace rivet::editor {

LinkService::LinkService(DocumentSession& session)
    : session_(session), dispatcher_(session.mainDispatcher()), executor_(session.scheduler()) {}

LinkService::~LinkService() {
    alive_->store(false, std::memory_order_release);
    executor_.cancelPending();
    executor_.waitUntilIdle();
}

std::vector<pdf::PdfPageLink> LinkService::cachedLinks(core::PageId pageId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(pageId);
    if (it == cache_.end()) return {};
    // Promote to MRU.
    lru_.splice(lru_.begin(), lru_, it->second.first);
    return it->second.second;
}

void LinkService::put(core::PageId pageId, std::vector<pdf::PdfPageLink> links) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(pageId);
    if (it != cache_.end()) {
        lru_.erase(it->second.first);
        cache_.erase(it);
    }
    while (cache_.size() >= kMaxCachedPages && !lru_.empty()) {
        cache_.erase(lru_.back());
        lru_.pop_back();
    }
    lru_.push_front(pageId);
    cache_[pageId] = {lru_.begin(), std::move(links)};
}

void LinkService::ensurePageLinks(core::PageId pageId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cache_.count(pageId) != 0) return;
        pending_[pageId]; // coalescing entry
    }
    scheduleLoad(pageId);
}

void LinkService::requestPageLinks(core::PageId pageId, LinksCallback onDone) {
    if (!onDone) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[pageId].push_back(std::move(onDone));
    }
    scheduleLoad(pageId);
}

void LinkService::scheduleLoad(core::PageId pageId) {
    // Capture by value (see TextService::scheduleExtraction for the rationale).
    pdf::PdfDocument& document = session_.document();
    const std::size_t pageIndex = session_.pageIndexFor(pageId);
    executor_.post([this, pageId, pageIndex, &document] {
        std::vector<LinksCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find(pageId);
            if (it == pending_.end()) return;
            callbacks = std::move(it->second);
            pending_.erase(it);
        }

        std::vector<pdf::PdfPageLink> links = cachedLinks(pageId);
        if (links.empty() && pageIndex != DocumentSession::kInvalidPage) {
            links = document.pageLinks(pageIndex).value_or(std::vector<pdf::PdfPageLink>{});
            put(pageId, links);
        }

        if (callbacks.empty()) return;
        if (dispatcher_ != nullptr) {
            dispatcher_->post([alive = alive_, callbacks = std::move(callbacks), links]() mutable {
                if (!alive->load(std::memory_order_acquire)) return;
                for (auto& callback : callbacks) callback(links);
            });
        } else {
            for (auto& callback : callbacks) callback(links);
        }
    });
}

std::vector<pdf::PdfPageLink> LinkService::linksNow(core::PageId pageId) {
    if (auto cached = cachedLinks(pageId); !cached.empty()) return cached;
    const std::size_t pageIndex = session_.pageIndexFor(pageId);
    if (pageIndex == DocumentSession::kInvalidPage) return {};
    auto links = session_.document().pageLinks(pageIndex).value_or(std::vector<pdf::PdfPageLink>{});
    put(pageId, links);
    return links;
}

LinkService::Outline LinkService::cachedOutline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outline_;
}

void LinkService::requestOutline(std::function<void(Outline)> onDone) {
    if (!onDone) return;
    pdf::PdfDocument& document = session_.document();
    executor_.post([this, &document, onDone = std::move(onDone)]() mutable {
        Outline outline = cachedOutline();
        if (outline == nullptr) {
            auto loaded = document.outline();
            outline = std::make_shared<const std::optional<pdf::PdfOutlineNode>>(
                loaded.has_value() ? std::move(*loaded) : std::nullopt);
            std::lock_guard<std::mutex> lock(mutex_);
            outline_ = outline;
        }
        if (dispatcher_ == nullptr) {
            onDone(std::move(outline));
            return;
        }
        dispatcher_->post([alive = alive_, onDone = std::move(onDone), outline]() mutable {
            if (alive->load(std::memory_order_acquire)) onDone(std::move(outline));
        });
    });
}

} // namespace rivet::editor
