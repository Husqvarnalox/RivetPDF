// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "CommandStack.hpp"
#include "DocumentRenderer.hpp"
#include "LinkService.hpp"
#include "PageModel.hpp"
#include "TextService.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfTypes.hpp"
#include "render/PageLayout.hpp"
#include "render/TileCache.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::editor {

// One open document: the base PDF handle, the editable PAGE MODEL over it
// (order, identities, per-page views - see PageModel), the page layout, the
// render pipeline (DocumentRenderer over a dedicated SerialExecutor +
// TileCache), the text/link services and the undo stack with dirty
// tracking. Exactly one session exists per open document; the owning app
// layer lives on the main thread, so session state other than what the
// renderer/services document is main-thread only. Worker jobs never read
// session state: they capture an immutable PageModelSnapshot (or a copy of
// one of its entries) on the main thread.
//
// Page-model reactions (synchronous, main thread, after every mutation
// including undo/redo): the layout is rebuilt from the new snapshot; text
// and links of deleted pages are evicted (render tiles of deleted pages are
// left to the LRU - an undo makes them valid again); then the app's
// page-model observer runs. Policies the APP applies from that callback
// (PageModelChange): PageSelection::applyChange, clear the text selection
// when textSelectionInvalidatedBy() says so, TextSearchController::
// handlePageModelChanged (restart).
//
// Member destruction order is load-bearing: the services and the renderer
// are declared LAST and are therefore destroyed FIRST - they cancel queued
// work and wait for in-flight jobs while the executor, cache, page model,
// documents and the caller's TaskScheduler are all still alive. The command
// stack is declared after the page model (commands reference it). (The
// scheduler itself is owned by the app and must outlive the session, per the
// SerialExecutor contract.)
class DocumentSession {
public:
    // Opens the document and builds the page model and layout. Returns
    // NotAvailable when the engine has no working backend, and the backend's
    // own error (InvalidDocument / Io / ...) for files that cannot be opened
    // or whose page metadata cannot be read.
    static core::Result<std::unique_ptr<DocumentSession>> create(
        pdf::PdfEngine& engine,
        core::TaskScheduler& scheduler,
        core::IMainThreadDispatcher* mainDispatcher,
        const std::filesystem::path& path,
        std::string_view password = {});

    // Cancels pending work (services and renderer first) and then tears the
    // rest down.
    ~DocumentSession();

    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    core::DocumentId id() const { return id_; }
    const std::filesystem::path& path() const { return path_; }
    // Info of the BASE document as opened (info().pageCount is the base's
    // page count, not the model's - use pageCount()).
    const pdf::PdfDocumentInfo& info() const { return info_; }

    // --- Page model (all delegate to the current snapshot) ---------------

    std::size_t pageCount() const { return model_->size(); }

    // Sentinel returned by pageIndexFor() for an unknown PageId.
    static constexpr std::size_t kInvalidPage = PageModelSnapshot::kInvalidIndex;

    // Position of a page in the CURRENT model order (kInvalidPage when not
    // in the document). This is a model index, not a backend page index.
    std::size_t pageIndexFor(core::PageId pageId) const;

    // Asserts index < pageCount.
    core::PageId pageId(std::size_t index) const;

    // The PageIds in reading order (a copy: safe to hand to worker tasks).
    std::vector<core::PageId> pageOrder() const;

    // Display size of the page's view (post-rotation, crop box size).
    // Asserts index < pageCount.
    core::Size pageSizePoints(std::size_t index) const;

    // Page label from the base document's page-label tree ("i", "A-1", ...)
    // while the model order is the identity over the base pages; otherwise
    // (and when the document has no labels) an empty string: fall back to
    // 1-based positional numbers. Asserts index < pageCount.
    std::string pageLabel(std::size_t index) const;

    // All labels (parallel to pageCount; empty entries = no label), with the
    // same identity rule.
    std::vector<std::string> pageLabels() const;

    // Immutable snapshot of the current page model: THE way to hand page
    // state to worker jobs (render, text, search, links, copy, save).
    const PageSnapshotPtr& pageSnapshot() const { return model_->snapshot(); }

    // The mutable model, for constructing page commands (PageCommands.hpp).
    // Mutate it only through commands run by execute()/commands().
    PageModel& pageModel() { return *model_; }
    const PageModel& pageModel() const { return *model_; }

    // Monotonically increasing on every page-model mutation, including
    // undo/redo (never goes backwards).
    std::uint64_t documentRevision() const { return model_->documentRevision(); }
    std::uint64_t orderRevision() const { return model_->orderRevision(); }

    // Observer of page-model changes (see class comment). Main thread,
    // synchronous, never under a lock.
    void setOnPageModelChanged(std::function<void(const PageModelChange&)> onChanged);

    // --- Destinations --------------------------------------------------------

    // Outline destinations point into the BASE document; link destinations
    // into the document the link's page comes from. Both resolve to the
    // first model entry presenting that source page (nullopt when it was
    // deleted), with the point re-mapped through that entry's view.
    std::optional<PageDestination> resolveOutlineDestination(const pdf::PdfDestination& destination) const;
    std::optional<PageDestination> resolveLinkDestination(core::PageId fromPage,
                                                          const pdf::PdfDestination& destination) const;

    // --- Commands, undo and dirty state -------------------------------------

    CommandStack& commands() { return commands_; }

    // Runs a command through the stack. The error is the command's reported
    // failure (or a generic InvalidArgument).
    core::Status execute(std::unique_ptr<Command> command);
    bool undo() { return commands_.undo(); }
    bool redo() { return commands_.redo(); }

    // Dirty = the current history state differs from the state last marked
    // saved (initially: the state after open). Undoing back to the saved
    // state makes the document clean again; a new command after an undo
    // never matches an old state (CommandStack::stateId).
    bool isDirty() const { return commands_.stateId() != savedStateId_; }
    void markSaved();
    // Fired (main thread, synchronously) whenever isDirty() flips.
    void setOnDirtyChanged(std::function<void(bool dirty)> onDirtyChanged);

    // --- Rendering, text, links ---------------------------------------------

    const render::PageLayout& layout() const { return layout_; }

    // Dependencies the text pipeline needs (scheduler must outlive the
    // session; the dispatcher may be null in tests).
    core::TaskScheduler& scheduler() { return executor_.scheduler(); }
    core::IMainThreadDispatcher* mainDispatcher() { return mainDispatcher_; }

    // The BASE document handle (as opened). Pages of the model may come
    // from other documents too; render/text work must go through the
    // snapshot entries (entry.source + entry.sourcePageIndex + entry.view).
    pdf::PdfDocument& document() { return *document_; }
    const pdf::PdfDocument& document() const { return *document_; }
    const std::shared_ptr<pdf::PdfDocument>& documentPtr() const { return document_; }

    // The DocumentRenderer behind the IRenderSource interface. Tile keys
    // must carry the page's current contentRevision
    // (pageSnapshot()->find(id)->contentRevision).
    render::IRenderSource& renderSource() { return renderer_; }

    // The text pipeline (cache + dedicated extraction stream).
    TextService& textService() { return textService_; }
    const TextService& textService() const { return textService_; }

    // Per-page links (dedicated stream + count-bounded LRU).
    LinkService& linkService() { return linkService_; }

    // RENDER revision (tile-cache epoch), bumped only by markModified().
    // Page-model edits do NOT bump it: tiles are keyed by (PageId,
    // contentRevision) so edits invalidate only the affected pages. Starts
    // at 1.
    std::uint64_t revision() const { return revision_; }

    // Marks the document content as changed OUTSIDE the page model (e.g. a
    // future content-editing backend): bumps the render revision so every
    // cached tile stops matching, and propagates it to the renderer.
    void markModified();

    // NOTE (performance): create() loads every page's metadata synchronously
    // on the calling (main) thread. Measured against local fixtures, one
    // pageInfo() round-trip costs a few microseconds (Debug build), so even a
    // thousands-page document costs single-digit milliseconds of synchronous
    // work - acceptable for now, but page metadata loading on network or slow
    // storage will need an incremental/lazy design behind the same PageLayout
    // interface instead of this upfront loop.

private:
    DocumentSession(core::DocumentId id,
                    std::filesystem::path path,
                    pdf::PdfDocumentInfo info,
                    std::vector<std::string> pageLabels,
                    std::shared_ptr<pdf::PdfDocument> document,
                    std::unique_ptr<PageModel> model,
                    core::TaskScheduler& scheduler,
                    core::IMainThreadDispatcher* mainDispatcher);

    void rebuildLayout();
    void handleModelChanged(const PageModelChange& change);
    void handleCommandsChanged();
    std::optional<RenderPageTarget> resolveRenderTarget(core::PageId pageId) const;

    core::DocumentId id_;
    std::filesystem::path path_;
    pdf::PdfDocumentInfo info_;
    core::IMainThreadDispatcher* mainDispatcher_ = nullptr;
    std::vector<std::string> baseLabels_; // parallel to the BASE pages
    render::PageLayout layout_;
    std::uint64_t revision_ = 1;

    // Declaration order = reverse destruction order (see class comment).
    std::shared_ptr<pdf::PdfDocument> document_;
    std::unique_ptr<PageModel> model_;
    CommandStack commands_;
    std::uint64_t savedStateId_ = 0;
    bool lastDirty_ = false;
    std::function<void(bool)> onDirtyChanged_;
    std::function<void(const PageModelChange&)> onPageModelChanged_;
    render::TileCache cache_;
    core::SerialExecutor executor_;
    DocumentRenderer renderer_;
    TextService textService_;
    LinkService linkService_;
};

} // namespace rivet::editor
