// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/Annotations.hpp"
#include "editor/PageModel.hpp"

#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfPageGeometry.hpp"

// Forward declaration: only a reference is held here (DocumentSession owns an
// AnnotationService member - including it would cycle).
namespace rivet::editor {
class DocumentSession;
}

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::editor {

// The annotations of a document as the editor sees them (ADR-0011):
//
//   - ORIGINALS: the page's /Annots as stored in its file, loaded lazily per
//     (source document, source page) on a DEDICATED SerialExecutor stream
//     (never the UI thread; PDFium calls are gated inside the backend), into
//     a bounded LRU (kMaxCachedPages). A load error or a backend without
//     annotation support caches an EMPTY list (the page then simply has no
//     editable originals). The cache holds originals only: edits live in the
//     page model (PageEntry::annotations), so eviction never loses an edit.
//   - IDENTITY REGISTRY: (PageId, /Annots index of the entry's current
//     source) -> AnnotationId, minted through PageModel::mintAnnotationId on
//     first sight and never evicted nor reissued; overlay items that exist
//     in the file are registered too. Rebased by DocumentSession after a
//     save (rebased()).
//   - RESOLVED VIEW per page: editable originals that are not suppressed (in
//     /Annots order, drawnByOverlay = false), then the overlay items (in
//     z-order, drawnByOverlay = true, with a display-space appearance). All
//     geometry is display space of the entry's CURRENT view. Non-editable
//     originals are not listed: the raster draws them, they are not
//     selectable. Resolved results are cached per page and recomputed lazily
//     when the state, the view or the loaded-originals status changed.
//
// Threading: everything but the load jobs is main-thread only (the
// completion is posted to the main dispatcher; without one - tests - it runs
// inline on the worker and touches only the mutex-guarded cache). Pending
// work is dropped without firing when the service dies (liveness flag), and
// results of loads that belong to a previous base (rebased()) or to a
// document nobody references any more are dropped (per-request tokens; the
// document is verified through a weak_ptr, never by raw address alone).
class AnnotationService {
public:
    // Immutable resolved list of a page (shared: safe to keep while the page
    // is re-resolved).
    using Resolved = std::shared_ptr<const std::vector<AnnotationView>>;

    static constexpr std::size_t kMaxCachedPages = 128;

    explicit AnnotationService(DocumentSession& session);
    ~AnnotationService();

    AnnotationService(const AnnotationService&) = delete;
    AnnotationService& operator=(const AnnotationService&) = delete;

    // Fired (main thread) when a page's resolved annotations changed because
    // its originals finished loading. Model edits are reported through the
    // page-model change instead.
    void setOnChanged(std::function<void(core::PageId)> onChanged);

    // Main thread. Requests the page's originals when not loaded (the list
    // is overlay-only until they are). Empty for an unknown page.
    Resolved annotations(core::PageId pageId) const;

    std::optional<AnnotationView> find(core::PageId pageId, core::AnnotationId id) const;

    // Topmost (last in z-order) annotation hit at `display` (page display
    // space) within `tolerancePoints`. Per kind: markup = inside any quad;
    // Note/Stamp = inside bounds; Square/Circle = near the outline (width/2 +
    // tolerance) or inside when filled; Ink/Line/Arrow = near the strokes.
    std::optional<core::AnnotationId> hitTest(core::PageId pageId, core::Point display,
                                              double tolerancePoints) const;

    // Which page and in which form an annotation lives (user-space data), for
    // the command factories. nullopt = unknown, deleted, not editable, or an
    // original whose page has not finished loading.
    struct Located {
        core::PageId page;
        core::AnnotationId id;
        bool overlay = false;
        std::size_t overlayPosition = 0;       // overlay only
        std::optional<std::uint32_t> index;    // original: /Annots index; overlay: fileIndex
        std::optional<std::uint32_t> popupIndex; // original only
        pdf::PdfAnnotationData data;           // user space
        pdf::PdfPageView view;
        PageAnnotationStatePtr state;
        std::uint64_t rasterRevision = 0;
    };
    std::optional<Located> locate(core::AnnotationId id) const;

    // Drops resolved lists of removed pages (main thread). The registry and
    // the originals cache are kept: an undo brings the pages back.
    void evictPages(std::span<const core::PageId> pageIds);

    // How one page's identity registry changes when the session rebases onto
    // a freshly saved file (ADR-0012 "Rebase").
    struct PageRekey {
        core::PageId page;
        // true: surviving originals shift down by the number of removed
        // indices below them and `overlayIds` register at their new file
        // indices. false: the page's registry entries are dropped (ids of
        // its overlay items are lost; documented fallback).
        bool keep = false;
        std::vector<std::uint32_t> removed; // sorted old suppressed indices
        std::vector<std::pair<core::AnnotationId, std::uint32_t>> overlayIds; // id, NEW index
    };

    // The session switched its base document: re-keys the registry, drops the
    // originals cache, the pending loads and the resolved lists. Main thread.
    void rebased(std::span<const PageRekey> pages);

    // Diagnostics / tests.
    std::size_t cachedOriginalPages() const;
    std::size_t registrySize() const { return registry_.size(); }

private:
    struct Key {
        const pdf::PdfDocument* document = nullptr;
        std::size_t pageIndex = 0;
        bool operator<(const Key& o) const {
            return document != o.document ? std::less<const pdf::PdfDocument*>{}(document, o.document)
                                          : pageIndex < o.pageIndex;
        }
    };
    struct Cached {
        std::list<Key>::iterator lru;
        std::weak_ptr<pdf::PdfDocument> document;
        pdf::PdfPageAnnotationsPtr annotations;
    };
    struct Pending {
        std::uint64_t token = 0;
        std::weak_ptr<pdf::PdfDocument> document;
        std::vector<core::PageId> pages;
    };

    // The loaded originals of the entry's source page; null (and a load is
    // requested when `request`) while not loaded.
    pdf::PdfPageAnnotationsPtr originalsFor(const PageEntry& entry, bool request) const;
    void requestLoad(const PageEntry& entry) const;
    void completeLoad(const Key& key, std::uint64_t token, const std::weak_ptr<pdf::PdfDocument>& document,
                      pdf::PdfPageAnnotationsPtr annotations) const;

    // Registry id of (page, index), minted on first sight.
    core::AnnotationId registryId(core::PageId page, std::uint32_t index) const;
    Resolved resolve(const PageEntry& entry, const pdf::PdfPageAnnotationsPtr& originals) const;

    DocumentSession& session_;
    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    // False once destruction started; shared with posted deliveries.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    core::SerialExecutor executor_;
    std::function<void(core::PageId)> onChanged_;

    // Originals cache + pending loads (shared with the completion).
    mutable std::mutex mutex_;
    mutable std::list<Key> lru_; // front = most recently used
    mutable std::map<Key, Cached> cache_;
    mutable std::map<Key, Pending> pending_;
    mutable std::uint64_t nextToken_ = 0;

    // Main-thread only.
    struct RegistryKey {
        core::PageId page;
        std::uint32_t index = 0;
        bool operator<(const RegistryKey& o) const {
            return page != o.page ? page < o.page : index < o.index;
        }
    };
    mutable std::map<RegistryKey, core::AnnotationId> registry_;
    mutable std::unordered_map<core::AnnotationId, RegistryKey> registryById_;

    struct ResolvedEntry {
        PageAnnotationStatePtr state;
        std::uint64_t contentRevision = 0;
        bool originalsLoaded = false;
        Resolved list;
    };
    mutable std::unordered_map<core::PageId, ResolvedEntry> resolved_;
};

} // namespace rivet::editor
