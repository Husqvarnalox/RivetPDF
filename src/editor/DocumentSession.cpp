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

std::shared_ptr<const std::unordered_map<core::PageId, std::size_t>>
DocumentSession::buildPageIndexMap(const std::vector<PageMeta>& pages) {
    auto map = std::make_shared<std::unordered_map<core::PageId, std::size_t>>();
    map->reserve(pages.size());
    for (std::size_t index = 0; index < pages.size(); ++index) {
        (*map)[pages[index].id] = index;
    }
    return map;
}

DocumentSession::DocumentSession(core::DocumentId id,
                                 std::filesystem::path path,
                                 pdf::PdfDocumentInfo info,
                                 std::vector<PageMeta> pages,
                                 std::unique_ptr<pdf::PdfDocument> document,
                                 core::TaskScheduler& scheduler,
                                 core::IMainThreadDispatcher* mainDispatcher,
                                 std::shared_ptr<const std::unordered_map<core::PageId, std::size_t>> pageIndexMap)
    : id_(id),
      path_(std::move(path)),
      info_(std::move(info)),
      mainDispatcher_(mainDispatcher),
      pages_(std::move(pages)),
      document_(std::move(document)),
      executor_(scheduler),
      renderer_(id_,
                *document_,
                [indexMap = pageIndexMap](core::PageId pageId) -> std::size_t {
                    const auto it = indexMap->find(pageId);
                    return it == indexMap->end() ? kInvalidPageIndex : it->second;
                },
                cache_,
                scheduler,
                executor_,
                mainDispatcher),
      pageIndexMap_(std::move(pageIndexMap)),
      textService_(*this) {
    std::vector<render::PageLayout::PageInfo> layoutPages;
    layoutPages.reserve(pages_.size());
    for (const PageMeta& page : pages_) {
        layoutPages.push_back(render::PageLayout::PageInfo{page.id, page.sizePoints, page.rotation});
    }
    layout_.setPageGapPoints(16.0);
    layout_.setPageMarginPoints(24.0);
    layout_.setPages(std::move(layoutPages));
}

DocumentSession::~DocumentSession() = default;

core::Result<std::unique_ptr<DocumentSession>> DocumentSession::create(
    pdf::PdfEngine& engine,
    core::TaskScheduler& scheduler,
    core::IMainThreadDispatcher* mainDispatcher,
    const std::filesystem::path& path) {
    if (!engine.isAvailable()) {
        return std::unexpected(
            core::Error{core::ErrorCode::NotAvailable, "no PDF backend is available", "editor"});
    }

    auto opened = engine.openDocument(path);
    if (!opened.has_value()) {
        return std::unexpected(std::move(opened).error());
    }
    std::unique_ptr<pdf::PdfDocument> document = std::move(*opened);

    const pdf::PdfDocumentInfo info = document->info();
    std::vector<PageMeta> pages;
    pages.reserve(info.pageCount);
    core::IdGenerator<core::PageIdTag> pageIds;
    for (std::size_t index = 0; index < info.pageCount; ++index) {
        auto page = document->pageInfo(index);
        if (!page.has_value()) {
            return std::unexpected(std::move(page).error());
        }
        pages.push_back(PageMeta{pageIds.next(), page->sizePoints, page->rotation});
    }

    // Build the index map BEFORE pages is moved into the constructor.
    auto pageIndexMap = buildPageIndexMap(pages);
    return std::unique_ptr<DocumentSession>(new DocumentSession(
        mintDocumentId(), path, info, std::move(pages), std::move(document), scheduler,
        mainDispatcher, std::move(pageIndexMap)));
}

std::size_t DocumentSession::pageIndexFor(core::PageId pageId) const {
    const auto it = pageIndexMap_->find(pageId);
    return it == pageIndexMap_->end() ? kInvalidPage : it->second;
}

core::PageId DocumentSession::pageId(std::size_t index) const {
    assert(index < pages_.size());
    return pages_[index].id;
}

core::Size DocumentSession::pageSizePoints(std::size_t index) const {
    assert(index < pages_.size());
    return pages_[index].sizePoints;
}

void DocumentSession::markModified() {
    ++revision_;
    renderer_.setRevision(revision_);
}

} // namespace rivet::editor
