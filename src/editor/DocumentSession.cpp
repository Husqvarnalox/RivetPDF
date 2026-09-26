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
    std::vector<std::string> labels;
    labels.reserve(info.pageCount);
    for (std::size_t index = 0; index < info.pageCount; ++index) {
        // Optional metadata: a label failure never blocks the document.
        labels.push_back(document->pageLabel(index).value_or(std::string{}));
    }

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
    if (commands_.execute(std::move(command))) return core::ok();
    if (const auto& error = commands_.lastError(); error.has_value()) return std::unexpected(*error);
    return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "the command could not be applied",
                                           "editor"));
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
