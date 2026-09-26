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

std::optional<std::vector<pdf::PdfPageLink>> LinkService::cached(core::PageId pageId,
                                                                 std::uint64_t contentRevision) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(pageId);
    if (it == cache_.end() || it->second.contentRevision != contentRevision) return std::nullopt;
    lru_.splice(lru_.begin(), lru_, it->second.lru); // promote to MRU
    return it->second.links;
}

std::vector<pdf::PdfPageLink> LinkService::cachedLinks(core::PageId pageId) const {
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr) return {};
    return cached(pageId, entry->contentRevision).value_or(std::vector<pdf::PdfPageLink>{});
}

void LinkService::put(core::PageId pageId, std::uint64_t contentRevision, std::vector<pdf::PdfPageLink> links) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = cache_.find(pageId);
    if (it != cache_.end()) {
        lru_.erase(it->second.lru);
        cache_.erase(it);
    }
    while (cache_.size() >= kMaxCachedPages && !lru_.empty()) {
        cache_.erase(lru_.back());
        lru_.pop_back();
    }
    lru_.push_front(pageId);
    cache_[pageId] = CachedLinks{lru_.begin(), contentRevision, std::move(links)};
}

void LinkService::evictPages(std::span<const core::PageId> pageIds) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const core::PageId id : pageIds) {
        const auto it = cache_.find(id);
        if (it == cache_.end()) continue;
        lru_.erase(it->second.lru);
        cache_.erase(it);
    }
}

void LinkService::ensurePageLinks(core::PageId pageId) {
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr) return;
    if (cached(pageId, entry->contentRevision).has_value()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[{pageId, entry->contentRevision}]; // coalescing entry
    }
    scheduleLoad(*entry);
}

void LinkService::requestPageLinks(core::PageId pageId, LinksCallback onDone) {
    if (!onDone) return;
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr) {
        // Deleted page: no links (delivered like any other result).
        if (dispatcher_ == nullptr) {
            onDone({});
        } else {
            dispatcher_->post([alive = alive_, onDone = std::move(onDone)]() mutable {
                if (alive->load(std::memory_order_acquire)) onDone({});
            });
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[{pageId, entry->contentRevision}].push_back(std::move(onDone));
    }
    scheduleLoad(*entry);
}

void LinkService::scheduleLoad(const PageEntry& entry) {
    // Captures a copy of the snapshot entry (see TextService::scheduleExtraction).
    executor_.post([this, entry] {
        std::vector<LinksCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find({entry.id, entry.contentRevision});
            if (it == pending_.end()) return;
            callbacks = std::move(it->second);
            pending_.erase(it);
        }

        std::vector<pdf::PdfPageLink> links = linksNow(entry);

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

std::vector<pdf::PdfPageLink> LinkService::linksNow(const PageEntry& entry) {
    if (auto hit = cached(entry.id, entry.contentRevision)) return std::move(*hit);
    if (entry.source == nullptr) return {};
    auto links = entry.source->pageLinks(entry.sourcePageIndex, entry.view)
                     .value_or(std::vector<pdf::PdfPageLink>{});
    put(entry.id, entry.contentRevision, links);
    return links;
}

LinkService::Outline LinkService::cachedOutline() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return outline_;
}

void LinkService::requestOutline(std::function<void(Outline)> onDone) {
    if (!onDone) return;
    // The outline always belongs to the BASE document (resolve its
    // destinations with DocumentSession::resolveOutlineDestination).
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
