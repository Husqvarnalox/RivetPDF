// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rivet::editor {

// A page of some opened document, as it is inserted into a page model: the
// source page plus its NATIVE presentation (media box + native view). Built
// with PageModel::describePages (which reads the backend's page metadata).
struct PageSource {
    std::shared_ptr<pdf::PdfDocument> document;
    std::size_t pageIndex = 0;
    pdf::PdfBox mediaBox;
    pdf::PdfPageView nativeView;
};

// One page of the editor's page model.
//
// Identity: `id` is minted by the model's IdGenerator and never reused (a
// moved page keeps its id; duplicated/imported pages get new ids; deleted
// ids are never reissued - an undo/redo restores the very ids it removed).
//
// Presentation: `view` is what the page shows (absolute rotation + crop box
// in the source page's user space). mediaBox/nativeView are the source
// page's own metadata, kept so crops can be validated and reset without a
// backend call.
//
// contentRevision: identifies the page's CONTENT as presented (its view).
// 0 = the view the entry was created with; rotate/crop assign a fresh value
// from a model-wide monotonic counter, and undo/redo restore the exact
// (view, contentRevision) pair they replaced. Because values are minted
// model-wide and never reassigned to a different view, a (PageId,
// contentRevision) pair always denotes exactly one view - render tiles, text
// pages and links are cached by it, so reorder/delete/duplicate never
// invalidate another page's caches and an undone rotation hits the cache
// again.
struct PageEntry {
    core::PageId id;
    std::shared_ptr<pdf::PdfDocument> source;
    std::size_t sourcePageIndex = 0;
    pdf::PdfPageView view;
    std::uint64_t contentRevision = 0;
    pdf::PdfBox mediaBox;
    pdf::PdfPageView nativeView;
};

// A destination (link or outline) resolved against the page model.
struct PageDestination {
    core::PageId page;
    std::size_t index = 0; // position in the snapshot's order
    bool hasPoint = false;
    core::Point point; // display space of the target entry's CURRENT view
};

// Immutable, shareable state of a page model after one mutation. Worker jobs
// (render, text extraction, search walks, link loading, copy) capture a
// snapshot on the main thread instead of reading live session state; the
// snapshot keeps every source document alive through its entries.
class PageModelSnapshot {
public:
    static constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(-1);

    PageModelSnapshot(std::shared_ptr<pdf::PdfDocument> base, std::vector<PageEntry> entries,
                      std::uint64_t orderRevision, std::uint64_t documentRevision);

    const std::vector<PageEntry>& entries() const { return entries_; }
    std::size_t size() const { return entries_.size(); }
    const PageEntry& at(std::size_t index) const { return entries_.at(index); }

    std::uint64_t orderRevision() const { return orderRevision_; }
    std::uint64_t documentRevision() const { return documentRevision_; }
    const std::shared_ptr<pdf::PdfDocument>& base() const { return base_; }

    // Position of a page (kInvalidIndex when not in the model). O(1).
    std::size_t indexOf(core::PageId id) const;
    // nullptr when not in the model. O(1).
    const PageEntry* find(core::PageId id) const;
    bool contains(core::PageId id) const { return find(id) != nullptr; }

    std::vector<core::PageId> order() const;

    // True while the model is exactly the base document's pages in their
    // original order (views may differ). Page labels are only meaningful
    // then.
    bool isIdentityOrder() const { return identityOrder_; }

    // Maps a destination that points into `source`'s page indexing to the
    // FIRST entry presenting that source page (a duplicate never steals the
    // original's destinations). nullopt when that page is no longer in the
    // model (deleted) or `source` is unknown. The target point is re-mapped
    // through the entry's current view (via the destination's user-space
    // point; a display-only point survives only while the entry shows its
    // native view).
    std::optional<PageDestination> resolveDestination(const pdf::PdfDocument* source,
                                                      const pdf::PdfDestination& destination) const;

    // Save: PreserveBase over every page, base = the model's base document.
    // Extract: Fresh over `subset` (in MODEL order, duplicates in `subset`
    // ignored); InvalidArgument for an empty subset or an unknown id. The
    // request holds raw document pointers: keep this snapshot alive until
    // the assembly finished.
    enum class AssemblyMode : std::uint8_t { Save, Extract };
    core::Result<pdf::PdfAssemblyRequest> toAssemblyRequest(
        AssemblyMode mode, std::span<const core::PageId> subset = {}) const;

private:
    struct SourceKey {
        const pdf::PdfDocument* document = nullptr;
        std::size_t pageIndex = 0;
        bool operator==(const SourceKey&) const = default;
    };
    struct SourceKeyHash {
        std::size_t operator()(const SourceKey& key) const noexcept;
    };

    std::shared_ptr<pdf::PdfDocument> base_;
    std::vector<PageEntry> entries_;
    std::uint64_t orderRevision_ = 0;
    std::uint64_t documentRevision_ = 0;
    bool identityOrder_ = false;
    std::unordered_map<core::PageId, std::size_t> indexById_;
    std::unordered_map<SourceKey, std::size_t, SourceKeyHash> firstBySource_;
};

using PageSnapshotPtr = std::shared_ptr<const PageModelSnapshot>;

// What one model mutation changed, computed by diffing the snapshots before
// and after (so execute, undo and redo report uniformly).
struct PageModelChange {
    PageSnapshotPtr previous;
    PageSnapshotPtr current;
    bool orderChanged = false;              // move/insert/delete/duplicate
    std::vector<core::PageId> removed;      // in `previous`, not in `current`
    std::vector<core::PageId> added;        // in `current`, not in `previous`
    std::vector<core::PageId> contentChanged; // in both, view/contentRevision differ
};

// The editor-level mutable page model of one document. Main-thread owned
// and NOT thread-safe; workers only ever see immutable snapshots.
//
// Invariant: never empty. Every mutation is transactional (validated first,
// applied all-or-nothing; a failure leaves the model untouched and reports
// an error) and O(N) in the page count (index maps, single-pass rebuilds).
// After each successful mutation a new snapshot is published and the change
// observer (if any) is invoked synchronously, never under a lock.
class PageModel {
public:
    // Reads the native metadata of pages of `document` (backend call on the
    // calling thread; cheap metadata only).
    static core::Result<std::vector<PageSource>> describePages(
        const std::shared_ptr<pdf::PdfDocument>& document, std::span<const std::size_t> pageIndices);
    // All pages of `document` in order.
    static core::Result<std::vector<PageSource>> describeAllPages(
        const std::shared_ptr<pdf::PdfDocument>& document);

    // Identity model over `base`: one entry per page, native views, fresh
    // ids. InvalidDocument when the document has no pages.
    static core::Result<std::unique_ptr<PageModel>> createIdentity(std::shared_ptr<pdf::PdfDocument> base);
    // Same, from pre-read metadata (pages must all belong to `base`).
    static core::Result<std::unique_ptr<PageModel>> create(std::shared_ptr<pdf::PdfDocument> base,
                                                           std::vector<PageSource> pages);

    PageModel(const PageModel&) = delete;
    PageModel& operator=(const PageModel&) = delete;

    const PageSnapshotPtr& snapshot() const { return snapshot_; }
    std::size_t size() const { return snapshot_->size(); }
    std::uint64_t orderRevision() const { return orderRevision_; }
    std::uint64_t documentRevision() const { return documentRevision_; }
    const std::shared_ptr<pdf::PdfDocument>& base() const { return base_; }

    core::Result<pdf::PdfAssemblyRequest> toAssemblyRequest(
        PageModelSnapshot::AssemblyMode mode, std::span<const core::PageId> subset = {}) const {
        return snapshot_->toAssemblyRequest(mode, subset);
    }

    // Fresh, never-before-issued page id.
    core::PageId mintPageId() { return ids_.next(); }
    // Fresh content revision (see PageEntry::contentRevision).
    std::uint64_t mintContentRevision() { return ++lastContentRevision_; }

    // --- Primitive, transactional mutations (used by the commands) -------
    //
    // Removes the pages `ids` (no duplicates, all present) and returns them
    // with the index each had, ascending. Refuses (InvalidArgument) to leave
    // the model empty.
    core::Result<std::vector<std::pair<std::size_t, PageEntry>>> extract(std::span<const core::PageId> ids);

    // Inserts entries at their FINAL positions (strictly ascending, each <
    // size() + count); their ids must not be in the model.
    core::Status insertAt(std::vector<std::pair<std::size_t, PageEntry>> placed);

    // Removes `ids` and re-inserts them (keeping their relative model
    // order) as a block whose first page lands at final index
    // `destination` (0 <= destination <= size() - ids.size()). One publish.
    // Returns the ids' previous positions (ascending, with their entries)
    // for undo via restorePositions().
    core::Result<std::vector<std::pair<std::size_t, PageEntry>>> moveBlock(
        std::span<const core::PageId> ids, std::size_t destination);

    // Moves the given (present) pages back to the recorded positions in one
    // publish: the inverse of moveBlock.
    core::Status restorePositions(const std::vector<std::pair<std::size_t, PageEntry>>& positions);

    // Replaces view + contentRevision of existing pages (all ids present, no
    // duplicates, every view validated against the entry's media box).
    struct ViewUpdate {
        core::PageId id;
        pdf::PdfPageView view;
        std::uint64_t contentRevision = 0;
    };
    core::Status updateViews(std::span<const ViewUpdate> updates);

    // Change observer (the owning session). Invoked synchronously after each
    // successful mutation on the calling (main) thread.
    void setOnChanged(std::function<void(const PageModelChange&)> onChanged);

private:
    PageModel(std::shared_ptr<pdf::PdfDocument> base, std::vector<PageEntry> entries,
              core::IdGenerator<core::PageIdTag> ids);

    // Validates `ids` against the current snapshot: non-empty, all present,
    // no duplicates. Returns their positions, ascending.
    core::Result<std::vector<std::size_t>> positionsOf(std::span<const core::PageId> ids) const;

    void publish(std::vector<PageEntry> entries, bool orderChanged);

    std::shared_ptr<pdf::PdfDocument> base_;
    core::IdGenerator<core::PageIdTag> ids_;
    std::uint64_t lastContentRevision_ = 0;
    std::uint64_t orderRevision_ = 1;
    std::uint64_t documentRevision_ = 1;
    PageSnapshotPtr snapshot_;
    std::function<void(const PageModelChange&)> onChanged_;
};

// Whether a view is presentable for a page with `mediaBox`: valid crop box
// inside the media box (0.01pt slack for float round trips).
bool isValidViewFor(const pdf::PdfPageView& view, const pdf::PdfBox& mediaBox);

} // namespace rivet::editor
