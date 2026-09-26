// SPDX-License-Identifier: MPL-2.0
#include "editor/PageModel.hpp"

#include <algorithm>
#include <unordered_set>

namespace rivet::editor {
namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "editor");
}

} // namespace

bool isValidViewFor(const pdf::PdfPageView& view, const pdf::PdfBox& mediaBox) {
    return view.cropBox.isValid() && mediaBox.isValid() && mediaBox.contains(view.cropBox, 0.01);
}

// ---------------------------------------------------------------------------
// PageModelSnapshot
// ---------------------------------------------------------------------------

std::size_t PageModelSnapshot::SourceKeyHash::operator()(const SourceKey& key) const noexcept {
    std::size_t h = std::hash<const void*>{}(key.document);
    h ^= std::hash<std::size_t>{}(key.pageIndex) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h;
}

PageModelSnapshot::PageModelSnapshot(std::shared_ptr<pdf::PdfDocument> base,
                                     std::vector<PageEntry> entries, std::uint64_t orderRevision,
                                     std::uint64_t documentRevision)
    : base_(std::move(base)),
      entries_(std::move(entries)),
      orderRevision_(orderRevision),
      documentRevision_(documentRevision) {
    indexById_.reserve(entries_.size());
    firstBySource_.reserve(entries_.size());
    identityOrder_ = base_ != nullptr && entries_.size() == base_->info().pageCount;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const PageEntry& entry = entries_[index];
        indexById_.emplace(entry.id, index);
        // emplace keeps the FIRST entry for a source page.
        firstBySource_.emplace(SourceKey{entry.source.get(), entry.sourcePageIndex}, index);
        if (entry.source != base_ || entry.sourcePageIndex != index) identityOrder_ = false;
    }
}

std::size_t PageModelSnapshot::indexOf(core::PageId id) const {
    const auto it = indexById_.find(id);
    return it == indexById_.end() ? kInvalidIndex : it->second;
}

const PageEntry* PageModelSnapshot::find(core::PageId id) const {
    const auto it = indexById_.find(id);
    return it == indexById_.end() ? nullptr : &entries_[it->second];
}

std::vector<core::PageId> PageModelSnapshot::order() const {
    std::vector<core::PageId> ids;
    ids.reserve(entries_.size());
    for (const PageEntry& entry : entries_) ids.push_back(entry.id);
    return ids;
}

std::optional<PageDestination> PageModelSnapshot::resolveDestination(
    const pdf::PdfDocument* source, const pdf::PdfDestination& destination) const {
    if (source == nullptr) return std::nullopt;
    const auto it = firstBySource_.find(SourceKey{source, destination.pageIndex});
    if (it == firstBySource_.end()) return std::nullopt; // page deleted (or never imported)
    const PageEntry& entry = entries_[it->second];
    PageDestination resolved;
    resolved.page = entry.id;
    resolved.index = it->second;
    if (destination.hasUserPoint && isValidViewFor(entry.view, entry.mediaBox)) {
        resolved.hasPoint = true;
        resolved.point = pdf::userToDisplay(entry.view, destination.userX, destination.userY);
    } else if (destination.hasPoint && entry.view == entry.nativeView) {
        // The display point is in the NATIVE view's space: still exact.
        resolved.hasPoint = true;
        resolved.point = destination.point;
    }
    return resolved;
}

core::Result<pdf::PdfAssemblyRequest> PageModelSnapshot::toAssemblyRequest(
    AssemblyMode mode, std::span<const core::PageId> subset) const {
    pdf::PdfAssemblyRequest request;
    request.base = base_.get();
    const auto append = [&request](const PageEntry& entry) {
        request.pages.push_back(pdf::PdfAssemblyPage{entry.source.get(), entry.sourcePageIndex, entry.view});
    };
    if (mode == AssemblyMode::Save) {
        request.mode = pdf::PdfAssemblyRequest::Mode::PreserveBase;
        request.pages.reserve(entries_.size());
        for (const PageEntry& entry : entries_) append(entry);
        return request;
    }

    request.mode = pdf::PdfAssemblyRequest::Mode::Fresh;
    if (subset.empty()) return std::unexpected(invalid("extract needs at least one page"));
    std::vector<bool> chosen(entries_.size(), false);
    for (const core::PageId id : subset) {
        const std::size_t index = indexOf(id);
        if (index == kInvalidIndex) return std::unexpected(invalid("extract names a page that is not in the document"));
        chosen[index] = true;
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (chosen[index]) append(entries_[index]);
    }
    return request;
}

// ---------------------------------------------------------------------------
// PageModel
// ---------------------------------------------------------------------------

core::Result<std::vector<PageSource>> PageModel::describePages(
    const std::shared_ptr<pdf::PdfDocument>& document, std::span<const std::size_t> pageIndices) {
    if (document == nullptr) return std::unexpected(invalid("no source document"));
    std::vector<PageSource> pages;
    pages.reserve(pageIndices.size());
    for (const std::size_t index : pageIndices) {
        auto info = document->pageInfo(index);
        if (!info.has_value()) return std::unexpected(std::move(info).error());
        pages.push_back(PageSource{document, index, info->mediaBox, info->view});
    }
    return pages;
}

core::Result<std::vector<PageSource>> PageModel::describeAllPages(
    const std::shared_ptr<pdf::PdfDocument>& document) {
    if (document == nullptr) return std::unexpected(invalid("no source document"));
    std::vector<std::size_t> indices(document->info().pageCount);
    for (std::size_t i = 0; i < indices.size(); ++i) indices[i] = i;
    return describePages(document, indices);
}

core::Result<std::unique_ptr<PageModel>> PageModel::createIdentity(std::shared_ptr<pdf::PdfDocument> base) {
    auto pages = describeAllPages(base);
    if (!pages.has_value()) return std::unexpected(std::move(pages).error());
    return create(std::move(base), std::move(*pages));
}

core::Result<std::unique_ptr<PageModel>> PageModel::create(std::shared_ptr<pdf::PdfDocument> base,
                                                           std::vector<PageSource> pages) {
    if (base == nullptr) return std::unexpected(invalid("no base document"));
    if (pages.empty()) {
        return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument,
                                               "the document has no pages", "editor"));
    }
    core::IdGenerator<core::PageIdTag> ids;
    std::vector<PageEntry> entries;
    entries.reserve(pages.size());
    for (PageSource& page : pages) {
        if (page.document != base) return std::unexpected(invalid("initial pages must come from the base document"));
        entries.push_back(PageEntry{ids.next(), std::move(page.document), page.pageIndex, page.nativeView, 0,
                                    page.mediaBox, page.nativeView});
    }
    return std::unique_ptr<PageModel>(new PageModel(std::move(base), std::move(entries), ids));
}

PageModel::PageModel(std::shared_ptr<pdf::PdfDocument> base, std::vector<PageEntry> entries,
                     core::IdGenerator<core::PageIdTag> ids)
    : base_(std::move(base)), ids_(ids) {
    snapshot_ = std::make_shared<const PageModelSnapshot>(base_, std::move(entries), orderRevision_,
                                                          documentRevision_);
}

void PageModel::setOnChanged(std::function<void(const PageModelChange&)> onChanged) {
    onChanged_ = std::move(onChanged);
}

core::Result<std::vector<std::size_t>> PageModel::positionsOf(std::span<const core::PageId> ids) const {
    if (ids.empty()) return std::unexpected(invalid("no pages given"));
    std::vector<std::size_t> positions;
    positions.reserve(ids.size());
    std::vector<bool> seen(snapshot_->size(), false);
    for (const core::PageId id : ids) {
        const std::size_t index = snapshot_->indexOf(id);
        if (index == PageModelSnapshot::kInvalidIndex) {
            return std::unexpected(core::makeError(core::ErrorCode::NotFound,
                                                   "page is not in the document", "editor"));
        }
        if (seen[index]) return std::unexpected(invalid("page listed twice"));
        seen[index] = true;
        positions.push_back(index);
    }
    std::sort(positions.begin(), positions.end());
    return positions;
}

core::Result<std::vector<std::pair<std::size_t, PageEntry>>> PageModel::extract(
    std::span<const core::PageId> ids) {
    auto positions = positionsOf(ids);
    if (!positions.has_value()) return std::unexpected(std::move(positions).error());
    const std::vector<PageEntry>& current = snapshot_->entries();
    if (positions->size() >= current.size()) {
        return std::unexpected(invalid("a document must keep at least one page"));
    }
    std::vector<std::pair<std::size_t, PageEntry>> removed;
    removed.reserve(positions->size());
    std::vector<PageEntry> remaining;
    remaining.reserve(current.size() - positions->size());
    std::size_t next = 0;
    for (std::size_t index = 0; index < current.size(); ++index) {
        if (next < positions->size() && (*positions)[next] == index) {
            removed.emplace_back(index, current[index]);
            ++next;
        } else {
            remaining.push_back(current[index]);
        }
    }
    publish(std::move(remaining), true);
    return removed;
}

core::Status PageModel::insertAt(std::vector<std::pair<std::size_t, PageEntry>> placed) {
    if (placed.empty()) return std::unexpected(invalid("no pages given"));
    const std::vector<PageEntry>& current = snapshot_->entries();
    const std::size_t total = current.size() + placed.size();
    std::unordered_set<core::PageId> newIds;
    newIds.reserve(placed.size());
    for (std::size_t i = 0; i < placed.size(); ++i) {
        const auto& [index, entry] = placed[i];
        if (index >= total || (i > 0 && index <= placed[i - 1].first)) {
            return std::unexpected(invalid("insert positions out of range or not ascending"));
        }
        if (!entry.id || entry.source == nullptr) return std::unexpected(invalid("incomplete page entry"));
        if (snapshot_->contains(entry.id) || !newIds.insert(entry.id).second) {
            return std::unexpected(invalid("page id is already in the document"));
        }
        if (entry.sourcePageIndex >= entry.source->info().pageCount) {
            return std::unexpected(invalid("source page index out of range"));
        }
        if (!isValidViewFor(entry.view, entry.mediaBox)) {
            return std::unexpected(invalid("page view is not within the media box"));
        }
    }
    std::vector<PageEntry> merged;
    merged.reserve(total);
    std::size_t next = 0;
    std::size_t from = 0;
    for (std::size_t index = 0; index < total; ++index) {
        if (next < placed.size() && placed[next].first == index) {
            merged.push_back(std::move(placed[next].second));
            ++next;
        } else {
            merged.push_back(current[from++]);
        }
    }
    publish(std::move(merged), true);
    return core::ok();
}

core::Result<std::vector<std::pair<std::size_t, PageEntry>>> PageModel::moveBlock(
    std::span<const core::PageId> ids, std::size_t destination) {
    auto positions = positionsOf(ids);
    if (!positions.has_value()) return std::unexpected(std::move(positions).error());
    const std::vector<PageEntry>& current = snapshot_->entries();
    if (destination > current.size() - positions->size()) {
        return std::unexpected(invalid("move destination out of range"));
    }
    std::vector<std::pair<std::size_t, PageEntry>> previous;
    previous.reserve(positions->size());
    std::vector<PageEntry> rest;
    rest.reserve(current.size() - positions->size());
    std::size_t next = 0;
    for (std::size_t index = 0; index < current.size(); ++index) {
        if (next < positions->size() && (*positions)[next] == index) {
            previous.emplace_back(index, current[index]);
            ++next;
        } else {
            rest.push_back(current[index]);
        }
    }
    bool unchanged = true;
    for (std::size_t i = 0; i < previous.size(); ++i) {
        if (previous[i].first != destination + i) unchanged = false;
    }
    std::vector<PageEntry> result;
    result.reserve(current.size());
    result.insert(result.end(), rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(destination));
    for (const auto& moved : previous) result.push_back(moved.second);
    result.insert(result.end(), rest.begin() + static_cast<std::ptrdiff_t>(destination), rest.end());
    // A no-op move still counts as a mutation (it is an undo step), but the
    // order did not change: order-keyed consumers need not rebuild.
    publish(std::move(result), !unchanged);
    return previous;
}

core::Status PageModel::restorePositions(const std::vector<std::pair<std::size_t, PageEntry>>& positions) {
    if (positions.empty()) return std::unexpected(invalid("no pages given"));
    std::vector<core::PageId> ids;
    ids.reserve(positions.size());
    for (const auto& item : positions) ids.push_back(item.second.id);
    auto current = positionsOf(ids);
    if (!current.has_value()) return std::unexpected(std::move(current).error());
    const std::vector<PageEntry>& entries = snapshot_->entries();
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (positions[i].first >= entries.size() || (i > 0 && positions[i].first <= positions[i - 1].first)) {
            return std::unexpected(invalid("restore positions out of range or not ascending"));
        }
    }
    std::unordered_set<core::PageId> moving(ids.begin(), ids.end());
    std::vector<PageEntry> result;
    result.reserve(entries.size());
    std::size_t next = 0;
    std::size_t from = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (next < positions.size() && positions[next].first == index) {
            // The live entry (same id) - the recorded one may predate a view
            // change that is itself not being undone here.
            result.push_back(entries[snapshot_->indexOf(positions[next].second.id)]);
            ++next;
            continue;
        }
        while (moving.count(entries[from].id) != 0) ++from;
        result.push_back(entries[from++]);
    }
    publish(std::move(result), true);
    return core::ok();
}

core::Status PageModel::updateViews(std::span<const ViewUpdate> updates) {
    if (updates.empty()) return std::unexpected(invalid("no pages given"));
    std::vector<core::PageId> ids;
    ids.reserve(updates.size());
    for (const ViewUpdate& update : updates) ids.push_back(update.id);
    auto positions = positionsOf(ids);
    if (!positions.has_value()) return std::unexpected(std::move(positions).error());
    for (const ViewUpdate& update : updates) {
        const PageEntry* entry = snapshot_->find(update.id);
        if (entry == nullptr || !isValidViewFor(update.view, entry->mediaBox)) {
            return std::unexpected(invalid("page view is not within the media box"));
        }
    }
    std::vector<PageEntry> entries = snapshot_->entries();
    for (const ViewUpdate& update : updates) {
        PageEntry& entry = entries[snapshot_->indexOf(update.id)];
        entry.view = update.view;
        entry.contentRevision = update.contentRevision;
    }
    publish(std::move(entries), false);
    return core::ok();
}

void PageModel::publish(std::vector<PageEntry> entries, bool orderChanged) {
    if (orderChanged) ++orderRevision_;
    ++documentRevision_;
    PageSnapshotPtr previous = std::move(snapshot_);
    snapshot_ = std::make_shared<const PageModelSnapshot>(base_, std::move(entries), orderRevision_,
                                                          documentRevision_);
    if (!onChanged_) return;

    PageModelChange change;
    change.previous = previous;
    change.current = snapshot_;
    change.orderChanged = orderChanged;
    for (const PageEntry& entry : previous->entries()) {
        const PageEntry* now = snapshot_->find(entry.id);
        if (now == nullptr) {
            change.removed.push_back(entry.id);
        } else if (now->view != entry.view || now->contentRevision != entry.contentRevision) {
            change.contentChanged.push_back(entry.id);
        }
    }
    for (const PageEntry& entry : snapshot_->entries()) {
        if (!previous->contains(entry.id)) change.added.push_back(entry.id);
    }
    // Copy: the observer may replace itself.
    auto observer = onChanged_;
    observer(change);
}

} // namespace rivet::editor
