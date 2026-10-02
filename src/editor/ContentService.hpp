// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/ContentObjects.hpp"
#include "editor/PageModel.hpp"

#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "pdf/PdfContent.hpp"

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
class DocumentSession;
}

namespace rivet::editor {

// The page content of a document as the editor sees it (ADR-0014), the
// content twin of AnnotationService:
//
//   - EXTRACTION: PdfDocument::pageContent(sourcePageIndex, entry.contentEdits)
//     runs lazily per (source document, source page, edits pointer) on a
//     DEDICATED SerialExecutor stream (never the UI thread), into a bounded
//     LRU (kMaxCachedPages). Errors / backends without content support
//     cache an EMPTY read-only content. Only extraction results are cached:
//     the edits live in the page model (PageEntry::contentEdits), so
//     eviction never loses an edit.
//   - IDENTITY REGISTRY: (PageId, source object index) -> ObjectId minted
//     through PageModel::mintObjectId on first sight, never evicted nor
//     reissued; created objects are keyed by their edit tag (the tag IS the
//     ObjectId value of the created block, see ADR-0014). Re-keyed by
//     DocumentSession after a save (rebased()).
//   - RESOLVED VIEW per page: display-space object views + reconstructed
//     text blocks (TextBlocks.hpp) + capabilities, recomputed lazily when
//     the entry's edits pointer, view or loaded status changed.
//
// Threading: everything but the extraction jobs is main-thread only; the
// completion is posted to the main dispatcher (without one - tests - it
// runs inline on the worker and touches only the mutex-guarded cache).
// Pending work is dropped without firing when the service dies (liveness
// flag); results of a previous base (rebased()) or of a document nobody
// references any more are dropped (per-request tokens + weak_ptr).
class ContentService {
public:
    static constexpr std::size_t kMaxCachedPages = 64;

    explicit ContentService(DocumentSession& session);
    ~ContentService();
    ContentService(const ContentService&) = delete;
    ContentService& operator=(const ContentService&) = delete;

    // Fired (main thread) when a page's resolved content changed because an
    // extraction finished. Model edits are reported through the page-model
    // change instead.
    void setOnChanged(std::function<void(core::PageId)> onChanged);

    // Main thread. Requests extraction when not loaded (`loaded` = false
    // until then). Empty view for an unknown page.
    PageContentViewPtr content(core::PageId pageId) const;
    std::optional<ContentObjectView> findObject(core::PageId pageId, core::ObjectId id) const;
    std::optional<TextBlockView> findBlock(core::PageId pageId, core::ObjectId blockId) const;

    // Topmost (highest z) object whose quad contains `display` within
    // `tolerancePoints` (display points; the caller derives it from the
    // zoom). Text objects hit as their block when they belong to one
    // (ContentHit::isBlock). ReadOnly objects are still hit (so the UI can
    // explain why they cannot be edited) unless `editableOnly`.
    std::optional<ContentHit> hitTest(core::PageId pageId, core::Point display, double tolerancePoints,
                                      bool editableOnly = false) const;

    // Drops resolved views of removed pages (main thread). Registry and
    // extraction cache are kept: an undo brings the pages back.
    void evictPages(std::span<const core::PageId> pageIds);

    // How one page's registry changes when the session rebases onto a
    // freshly saved file (ADR-0017 "Rebase"): `origins[i]` is the origin of
    // the saved page's object i (from PdfAssembledPageContent); empty =
    // the page had no edits (object i stays object i). Entries keyed by a
    // Source index are re-keyed to their new index; Created(tag) entries
    // are keyed to the new index of the first object carrying that tag.
    struct PageRekey {
        core::PageId page;
        std::vector<pdf::PdfContentOrigin> origins;
        std::vector<std::uint64_t> blockTags;
    };
    void rebased(std::span<const PageRekey> pages);

    // Diagnostics / tests.
    std::size_t cachedPages() const;
    std::size_t registrySize() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rivet::editor
