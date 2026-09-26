// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/PageModel.hpp"

#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "pdf/PdfNavigation.hpp"

// Forward declaration: only a reference is held here (DocumentSession owns a
// LinkService member - including it would cycle). The .cpp includes it.
namespace rivet::editor {
class DocumentSession;
}

#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <optional>
#include <span>
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
//     calling thread under the global PDFium gate) over a snapshot entry.
//
// Page model: links are loaded through each entry's view (rects in the
// entry's display space) and cached by (PageId, contentRevision), so
// reorders never invalidate them and rotate/crop only affect that page.
// Destinations stay in the SOURCE document's page indexing: resolve them
// with DocumentSession::resolveLinkDestination (deleted target = none).
//
// Lifetime: owned by DocumentSession, declared so it dies before the
// document handle; the destructor cancels queued work and waits for the
// in-flight job. Pending callbacks are dropped without firing, including
// deliveries already posted to the dispatcher (liveness flag).
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
    std::vector<pdf::PdfPageLink> linksNow(const PageEntry& entry);

    // Drops cached links of the given pages (deleted pages; main thread).
    void evictPages(std::span<const core::PageId> pageIds);

    // Document outline, loaded ONCE on this service's worker stream (the
    // outline walk is a PDFium call and must never run on the main thread).
    // cachedOutline() is null until loaded; requestOutline() delivers the
    // loaded tree (nullopt inside = the document has no outline, or it
    // could not be read) on the main thread, exactly once per call.
    using Outline = std::shared_ptr<const std::optional<pdf::PdfOutlineNode>>;
    Outline cachedOutline() const;
    void requestOutline(std::function<void(Outline)> onDone);

private:
    void scheduleLoad(const PageEntry& entry);
    void put(core::PageId pageId, std::uint64_t contentRevision, std::vector<pdf::PdfPageLink> links);
    // Cache probe for an exact (page, revision); nullopt on miss.
    std::optional<std::vector<pdf::PdfPageLink>> cached(core::PageId pageId, std::uint64_t contentRevision) const;
    static constexpr std::size_t kMaxCachedPages = 64;

    DocumentSession& session_;
    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    // False once destruction started; shared with posted deliveries.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    core::SerialExecutor executor_;

    mutable std::mutex mutex_;
    std::map<std::pair<core::PageId, std::uint64_t>, std::vector<LinksCallback>> pending_;
    Outline outline_; // guarded by mutex_; null until loaded
    // LRU: front = most recently used.
    mutable std::list<core::PageId> lru_;
    struct CachedLinks {
        std::list<core::PageId>::iterator lru;
        std::uint64_t contentRevision = 0;
        std::vector<pdf::PdfPageLink> links;
    };
    mutable std::unordered_map<core::PageId, CachedLinks> cache_;
};

} // namespace rivet::editor
