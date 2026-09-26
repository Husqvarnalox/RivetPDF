// SPDX-License-Identifier: MPL-2.0
#include "editor/DocumentSession.hpp"

#include <cassert>
#include <mutex>
#include <utility>

namespace rivet::editor {
namespace {

// Process-static generator: document ids are unique across every session in
// the process. IdGenerator itself is not thread-safe, hence the mutex.
core::DocumentId mintDocumentId() {
    static std::mutex mutex;
    static core::IdGenerator<core::DocumentIdTag> generator;
    std::lock_guard<std::mutex> lock(mutex);
    return generator.next();
}

std::vector<std::string> readLabels(const pdf::PdfDocument& document, std::size_t pageCount) {
    std::vector<std::string> labels;
    labels.reserve(pageCount);
    for (std::size_t index = 0; index < pageCount; ++index) {
        // Optional metadata: a label failure never blocks the document.
        labels.push_back(document.pageLabel(index).value_or(std::string{}));
    }
    return labels;
}

bool nearlyEqual(const pdf::PdfPageView& a, const pdf::PdfPageView& b) {
    constexpr double kEps = 0.01;
    const auto near = [](double x, double y) { return x - y <= kEps && y - x <= kEps; };
    return a.rotation == b.rotation && near(a.cropBox.left, b.cropBox.left) &&
           near(a.cropBox.bottom, b.cropBox.bottom) && near(a.cropBox.right, b.cropBox.right) &&
           near(a.cropBox.top, b.cropBox.top);
}

} // namespace

DocumentSession::DocumentSession(core::DocumentId id,
                                 std::filesystem::path path,
                                 pdf::PdfDocumentInfo info,
                                 std::vector<std::string> pageLabels,
                                 std::shared_ptr<pdf::PdfDocument> document,
                                 std::unique_ptr<PageModel> model,
                                 core::TaskScheduler& scheduler,
                                 core::IMainThreadDispatcher* mainDispatcher)
    : id_(id),
      path_(std::move(path)),
      info_(std::move(info)),
      mainDispatcher_(mainDispatcher),
      baseLabels_(std::move(pageLabels)),
      document_(std::move(document)),
      model_(std::move(model)),
      executor_(scheduler),
      renderer_(id_,
                [this](core::PageId pageId) { return resolveRenderTarget(pageId); },
                cache_,
                scheduler,
                executor_,
                mainDispatcher),
      textService_(*this),
      linkService_(*this) {
    layout_.setPageGapPoints(16.0);
    layout_.setPageMarginPoints(24.0);
    rebuildLayout();
    model_->setOnChanged([this](const PageModelChange& change) { handleModelChanged(change); });
    commands_.setOnChanged([this] { handleCommandsChanged(); });
    savedStateId_ = commands_.stateId();
}

DocumentSession::~DocumentSession() {
    // Commands never run during teardown; detach the observers first.
    commands_.setOnChanged(nullptr);
    model_->setOnChanged(nullptr);
}

core::Result<std::unique_ptr<DocumentSession>> DocumentSession::create(
    pdf::PdfEngine& engine,
    core::TaskScheduler& scheduler,
    core::IMainThreadDispatcher* mainDispatcher,
    const std::filesystem::path& path,
    std::string_view password) {
    if (!engine.isAvailable()) {
        return std::unexpected(
            core::Error{core::ErrorCode::NotAvailable, "no PDF backend is available", "editor"});
    }

    auto opened = engine.openDocument(path, password);
    if (!opened.has_value()) {
        return std::unexpected(std::move(opened).error());
    }
    std::shared_ptr<pdf::PdfDocument> document = std::move(*opened);

    const pdf::PdfDocumentInfo info = document->info();
    auto model = PageModel::createIdentity(document);
    if (!model.has_value()) {
        return std::unexpected(std::move(model).error());
    }
    std::vector<std::string> labels = readLabels(*document, info.pageCount);

    return std::unique_ptr<DocumentSession>(new DocumentSession(
        mintDocumentId(), path, info, std::move(labels), std::move(document), std::move(*model),
        scheduler, mainDispatcher));
}

std::size_t DocumentSession::pageIndexFor(core::PageId pageId) const {
    return pageSnapshot()->indexOf(pageId);
}

core::PageId DocumentSession::pageId(std::size_t index) const {
    assert(index < pageCount());
    return pageSnapshot()->at(index).id;
}

std::vector<core::PageId> DocumentSession::pageOrder() const {
    return pageSnapshot()->order();
}

core::Size DocumentSession::pageSizePoints(std::size_t index) const {
    assert(index < pageCount());
    return pdf::displaySize(pageSnapshot()->at(index).view);
}

std::string DocumentSession::pageLabel(std::size_t index) const {
    assert(index < pageCount());
    const PageModelSnapshot& snapshot = *pageSnapshot();
    if (!snapshot.isIdentityOrder() || index >= baseLabels_.size()) return {};
    return baseLabels_[index];
}

std::vector<std::string> DocumentSession::pageLabels() const {
    const PageModelSnapshot& snapshot = *pageSnapshot();
    if (snapshot.isIdentityOrder()) return baseLabels_;
    return std::vector<std::string>(snapshot.size());
}

void DocumentSession::setOnPageModelChanged(std::function<void(const PageModelChange&)> onChanged) {
    onPageModelChanged_ = std::move(onChanged);
}

std::optional<PageDestination> DocumentSession::resolveOutlineDestination(
    const pdf::PdfDestination& destination) const {
    return pageSnapshot()->resolveDestination(document_.get(), destination);
}

std::optional<PageDestination> DocumentSession::resolveLinkDestination(
    core::PageId fromPage, const pdf::PdfDestination& destination) const {
    const PageModelSnapshot& snapshot = *pageSnapshot();
    const PageEntry* from = snapshot.find(fromPage);
    // A link on a page that is gone still points into the base document's
    // indexing only if it came from there; without its page we cannot tell.
    if (from == nullptr) return std::nullopt;
    return snapshot.resolveDestination(from->source.get(), destination);
}

core::Status DocumentSession::execute(std::unique_ptr<Command> command) {
    if (editingLocked_) {
        return std::unexpected(core::makeError(
            core::ErrorCode::Unsupported,
            editingLockReason_.empty() ? std::string("the document cannot be edited right now") : editingLockReason_,
            "editor"));
    }
    if (commands_.execute(std::move(command))) return core::ok();
    if (const auto& error = commands_.lastError(); error.has_value()) return std::unexpected(*error);
    return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "the command could not be applied",
                                           "editor"));
}

void DocumentSession::setEditingLocked(bool locked, std::string reason) {
    editingLocked_ = locked;
    editingLockReason_ = locked ? std::move(reason) : std::string{};
}

core::Result<DocumentSession::RebaseTarget> DocumentSession::prepareRebase(
    pdf::PdfEngine& engine, const std::filesystem::path& path, const pdf::PdfDocument& credentialsOf) {
    auto opened = engine.reopenWithCredentialsOf(credentialsOf, path);
    if (!opened.has_value()) return std::unexpected(std::move(opened).error());
    RebaseTarget target;
    target.document = std::move(*opened);
    target.info = target.document->info();
    auto pages = PageModel::describeAllPages(target.document);
    if (!pages.has_value()) return std::unexpected(std::move(pages).error());
    target.pages = std::move(*pages);
    target.labels = readLabels(*target.document, target.info.pageCount);
    return target;
}

core::Status DocumentSession::rebaseOnto(RebaseTarget target, std::optional<std::filesystem::path> newPath) {
    const PageSnapshotPtr current = pageSnapshot(); // keep alive across the swap
    if (target.document == nullptr || target.pages.size() != current->size() ||
        target.info.pageCount != current->size()) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                               "the saved file does not match the document's pages", "editor"));
    }
    std::vector<PageEntry> entries;
    entries.reserve(current->size());
    for (std::size_t index = 0; index < current->size(); ++index) {
        const PageEntry& old = current->at(index);
        PageSource& page = target.pages[index];
        if (page.document != target.document || page.pageIndex != index) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument,
                                                   "rebase pages must be the new document's pages in order",
                                                   "editor"));
        }
        // Same view as presented (up to float noise): tiles/text keyed by
        // (PageId, contentRevision) stay valid. Otherwise a fresh revision.
        const std::uint64_t revision =
            nearlyEqual(old.view, page.nativeView) ? old.contentRevision : model_->mintContentRevision();
        entries.push_back(PageEntry{old.id, target.document, index, page.nativeView, revision, page.mediaBox,
                                    page.nativeView});
    }

    // Base-level state first: the page-model observer (fired by the model
    // rebase below) may read labels/info/document.
    std::shared_ptr<pdf::PdfDocument> previousBase = std::move(document_);
    document_ = target.document;
    info_ = target.info;
    baseLabels_ = std::move(target.labels);
    if (newPath.has_value()) path_ = std::move(*newPath);
    // Links (and the outline) index the previous documents' pages.
    linkService_.resetForNewBase();
    // Recorded commands reference entries of the previous documents; they
    // cannot be re-targeted (see header). Clear before the publish so no
    // stale command can run against the new model.
    commands_.clear();

    const core::Status rebased = model_->rebase(target.document, std::move(entries));
    if (!rebased.has_value()) {
        // Validated above; restore the previous base for consistency.
        document_ = std::move(previousBase);
        return rebased;
    }
    return core::ok();
}

void DocumentSession::markSaved() {
    savedStateId_ = commands_.stateId();
    handleCommandsChanged();
}

void DocumentSession::setOnDirtyChanged(std::function<void(bool)> onDirtyChanged) {
    onDirtyChanged_ = std::move(onDirtyChanged);
}

void DocumentSession::handleCommandsChanged() {
    const bool dirty = isDirty();
    if (dirty == lastDirty_) return;
    lastDirty_ = dirty;
    if (onDirtyChanged_) {
        auto callback = onDirtyChanged_; // may replace itself
        callback(dirty);
    }
}

void DocumentSession::rebuildLayout() {
    const PageModelSnapshot& snapshot = *pageSnapshot();
    std::vector<render::PageLayout::PageInfo> pages;
    pages.reserve(snapshot.size());
    for (const PageEntry& entry : snapshot.entries()) {
        pages.push_back(render::PageLayout::PageInfo{entry.id, pdf::displaySize(entry.view), entry.view.rotation});
    }
    layout_.setPages(std::move(pages));
}

void DocumentSession::handleModelChanged(const PageModelChange& change) {
    rebuildLayout();
    if (!change.removed.empty()) {
        textService_.evictPages(change.removed);
        linkService_.evictPages(change.removed);
    }
    if (onPageModelChanged_) {
        auto callback = onPageModelChanged_; // may replace itself
        callback(change);
    }
}

std::optional<RenderPageTarget> DocumentSession::resolveRenderTarget(core::PageId pageId) const {
    const PageEntry* entry = pageSnapshot()->find(pageId);
    if (entry == nullptr) return std::nullopt;
    return RenderPageTarget{entry->source, entry->sourcePageIndex, entry->view, entry->contentRevision};
}

void DocumentSession::markModified() {
    ++revision_;
    renderer_.setRevision(revision_);
}

} // namespace rivet::editor
