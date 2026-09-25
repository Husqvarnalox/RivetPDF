// SPDX-License-Identifier: MPL-2.0
#include "editor/LinkService.hpp"

#include "editor/DocumentSession.hpp"

#include <utility>

namespace rivet::editor {

LinkService::LinkService(DocumentSession& session)
    : session_(session), executor_(session.scheduler()) {}

LinkService::~LinkService() {
    executor_.cancelPending();
    // executor_ destruction waits for the in-flight job.
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
    executor_.post([this, pageId] {
        std::vector<LinksCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find(pageId);
            if (it == pending_.end()) return;
            callbacks = std::move(it->second);
            pending_.erase(it);
        }

        std::vector<pdf::PdfPageLink> links = cachedLinks(pageId);
        if (links.empty()) {
            const std::size_t pageIndex = session_.pageIndexFor(pageId);
            if (pageIndex != DocumentSession::kInvalidPage) {
                links = session_.document().pageLinks(pageIndex).value_or(std::vector<pdf::PdfPageLink>{});
            }
            put(pageId, links);
        }

        if (callbacks.empty()) return;
        core::IMainThreadDispatcher* dispatcher = session_.mainDispatcher();
        if (dispatcher != nullptr) {
            dispatcher->post([callbacks = std::move(callbacks), links]() mutable {
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

} // namespace rivet::editor
