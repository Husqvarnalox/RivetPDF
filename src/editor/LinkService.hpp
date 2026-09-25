// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"
#include "core/async/SerialExecutor.hpp"
#include "pdf/PdfNavigation.hpp"

// Forward declaration: only a reference is held here (DocumentSession owns a
// LinkService member - including it would cycle). The .cpp includes it.
namespace rivet::editor {
class DocumentSession;
}

#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::editor {

// Per-page link store for one document: async loading on a DEDICATED
// SerialExecutor stream (never the UI thread, never the renderer's or text
// stream), count-bounded LRU (links are tiny; the cache holds the 64 most
// recently requested pages), deduped requests, exactly-once delivery.
//
// Entry points mirror TextService:
//   - cachedLinks(): synchronous probe (main thread).
//   - ensurePageLinks(): fire-and-forget warm-up (used for the tracked page).
//   - requestPageLinks(): extraction + exactly-once callback (main thread or
//     inline in tests without a dispatcher).
//   - linksNow(): worker-thread path (cache probe or synchronous load on the
//     calling thread under the global PDFium gate).
//
// Lifetime: owned by DocumentSession, declared so it dies before the
// document handle; the destructor cancels queued work. Pending callbacks are
// dropped without firing (the owning view is dying).
class LinkService {
public:
    using LinksCallback = std::function<void(std::vector<pdf::PdfPageLink>)>;

    explicit LinkService(DocumentSession& session);
    ~LinkService();

    LinkService(const LinkService&) = delete;
    LinkService& operator=(const LinkService&) = delete;

    std::vector<pdf::PdfPageLink> cachedLinks(core::PageId pageId) const;

    void ensurePageLinks(core::PageId pageId);
    void requestPageLinks(core::PageId pageId, LinksCallback onDone);

    // Worker-thread path (mirrors TextService::textPageNow). Never main thread.
    std::vector<pdf::PdfPageLink> linksNow(core::PageId pageId);

private:
    void scheduleLoad(core::PageId pageId);
    void put(core::PageId pageId, std::vector<pdf::PdfPageLink> links);
    static constexpr std::size_t kMaxCachedPages = 64;

    DocumentSession& session_;
    core::SerialExecutor executor_;

    mutable std::mutex mutex_;
    std::unordered_map<core::PageId, std::vector<LinksCallback>> pending_;
    // LRU: front = most recently used.
    mutable std::list<core::PageId> lru_;
    mutable std::unordered_map<core::PageId, std::pair<std::list<core::PageId>::iterator,
                                                       std::vector<pdf::PdfPageLink>>>
        cache_;
};

} // namespace rivet::editor
