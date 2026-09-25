#pragma once

#include "CommandStack.hpp"
#include "DocumentRenderer.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfTypes.hpp"
#include "render/PageLayout.hpp"
#include "render/TileCache.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace rivet::editor {

// One open document: the PDF handle, its page metadata and layout, the render
// pipeline (DocumentRenderer over a dedicated SerialExecutor + TileCache) and
// the undo stack. Exactly one session exists per open document; the owning
// app layer lives on the main thread, so session state other than what the
// renderer documents is main-thread only.
//
// Member destruction order is load-bearing: the renderer is declared LAST and
// is therefore destroyed FIRST - it cancels queued render work and waits for
// the in-flight job while the executor, cache, document and the caller's
// TaskScheduler are all still alive. (The scheduler itself is owned by the
// app and must outlive the session, per the SerialExecutor contract.)
class DocumentSession {
public:
    // Opens the document and builds the page layout. Returns NotAvailable when
    // the engine has no working backend, and the backend's own error
    // (InvalidDocument / Io / ...) for files that cannot be opened or whose
    // page metadata cannot be read.
    static core::Result<std::unique_ptr<DocumentSession>> create(
        pdf::PdfEngine& engine,
        core::TaskScheduler& scheduler,
        core::IMainThreadDispatcher* mainDispatcher,
        const std::filesystem::path& path);

    // Cancels pending render work (renderer first) and then tears the rest down.
    ~DocumentSession();

    DocumentSession(const DocumentSession&) = delete;
    DocumentSession& operator=(const DocumentSession&) = delete;

    core::DocumentId id() const { return id_; }
    const std::filesystem::path& path() const { return path_; }
    const pdf::PdfDocumentInfo& info() const { return info_; }

    std::size_t pageCount() const { return pages_.size(); }

    // Asserts index < pageCount.
    core::PageId pageId(std::size_t index) const;

    // Display size (post-rotation) of the page. Asserts index < pageCount.
    core::Size pageSizePoints(std::size_t index) const;

    const render::PageLayout& layout() const { return layout_; }

    // Backend handle for non-rendering wiring (e.g. future editing backends).
    // The app layer must not use it for rendering; go through renderSource().
    pdf::PdfDocument& document() { return *document_; }

    // The DocumentRenderer behind the IRenderSource interface.
    render::IRenderSource& renderSource() { return renderer_; }

    CommandStack& commands() { return commands_; }

    // Document content revision, bumped by markModified(). Starts at 1.
    std::uint64_t revision() const { return revision_; }

    // Marks the document content as changed: bumps the revision (stale cache
    // tiles stop matching) and propagates it to the renderer. Future editing
    // commands call this after a successful apply.
    void markModified();

    // NOTE (future architectural requirement, do not build yet): pages_ is an
    // immutable PageId -> backend page index snapshot taken at open time.
    // That is sufficient for the viewer, but page editing/reordering (a later
    // phase) needs a mutable page model owned by the session and updated by
    // commands, so PageId identities survive reordering. See
    // docs/ARCHITECTURE.md, "Planned evolution".
    //
    // NOTE (performance): create() loads every page's metadata synchronously
    // on the calling (main) thread. Measured against local fixtures, one
    // pageInfo() round-trip costs a few microseconds (Debug build), so even a
    // thousands-page document costs single-digit milliseconds of synchronous
    // work - acceptable for now, but page metadata loading on network or slow
    // storage will need an incremental/lazy design behind the same PageLayout
    // interface instead of this upfront loop.

private:
    struct PageMeta {
        core::PageId id;
        core::Size sizePoints;
        core::PageRotation rotation = core::PageRotation::None;
    };

    DocumentSession(core::DocumentId id,
                    std::filesystem::path path,
                    pdf::PdfDocumentInfo info,
                    std::vector<PageMeta> pages,
                    std::unique_ptr<pdf::PdfDocument> document,
                    core::TaskScheduler& scheduler,
                    core::IMainThreadDispatcher* mainDispatcher);

    // PageId -> zero-based PDF page index. Held through a shared_ptr by the
    // renderer's lookup callable so worker-side lookups stay valid for the
    // renderer's whole lifetime.
    static std::shared_ptr<const std::unordered_map<core::PageId, std::size_t>>
    buildPageIndexMap(const std::vector<PageMeta>& pages);

    core::DocumentId id_;
    std::filesystem::path path_;
    pdf::PdfDocumentInfo info_;
    std::vector<PageMeta> pages_;
    render::PageLayout layout_;
    std::uint64_t revision_ = 1;
    CommandStack commands_;

    // Declaration order = reverse destruction order. renderer_ (last) dies
    // before executor_, cache_ and document_ (see class comment).
    std::unique_ptr<pdf::PdfDocument> document_;
    render::TileCache cache_;
    core::SerialExecutor executor_;
    DocumentRenderer renderer_;
};

} // namespace rivet::editor
