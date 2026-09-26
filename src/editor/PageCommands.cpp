// SPDX-License-Identifier: MPL-2.0
#include "editor/PageCommands.hpp"

#include "core/geometry/Rotation.hpp"

#include <algorithm>
#include <unordered_set>

namespace rivet::editor {
namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "editor");
}

// Current (view, contentRevision) of `ids`, validating presence/uniqueness.
core::Result<std::vector<PageModel::ViewUpdate>> currentViews(const PageModelSnapshot& snapshot,
                                                              std::span<const core::PageId> ids) {
    if (ids.empty()) return std::unexpected(invalid("no pages given"));
    std::unordered_set<core::PageId> seen;
    seen.reserve(ids.size());
    std::vector<PageModel::ViewUpdate> views;
    views.reserve(ids.size());
    for (const core::PageId id : ids) {
        const PageEntry* entry = snapshot.find(id);
        if (entry == nullptr) {
            return std::unexpected(core::makeError(core::ErrorCode::NotFound,
                                                   "page is not in the document", "editor"));
        }
        if (!seen.insert(id).second) return std::unexpected(invalid("page listed twice"));
        views.push_back(PageModel::ViewUpdate{id, entry->view, entry->contentRevision});
    }
    return views;
}

} // namespace

bool PageCommand::record(const core::Status& status) {
    if (status.has_value()) {
        failure_.reset();
        return true;
    }
    failure_ = status.error();
    return false;
}

bool PageCommand::fail(core::Error error) {
    failure_ = std::move(error);
    return false;
}

// --- Move -------------------------------------------------------------------

MovePagesCommand::MovePagesCommand(PageModel& model, std::vector<core::PageId> ids, std::size_t destination)
    : PageCommand(model), ids_(std::move(ids)), destination_(destination) {}

std::size_t MovePagesCommand::destinationForGap(const PageModelSnapshot& snapshot,
                                                std::span<const core::PageId> ids, std::size_t gap) {
    const std::size_t clamped = std::min(gap, snapshot.size());
    std::size_t before = 0; // moved pages above the gap shift it up
    for (const core::PageId id : ids) {
        const std::size_t index = snapshot.indexOf(id);
        if (index != PageModelSnapshot::kInvalidIndex && index < clamped) ++before;
    }
    return clamped - before;
}

bool MovePagesCommand::execute() {
    auto previous = model_.moveBlock(ids_, destination_);
    if (!record(previous)) return false;
    previous_ = std::move(*previous);
    return true;
}

bool MovePagesCommand::undo() {
    return record(model_.restorePositions(previous_));
}

// --- Delete -----------------------------------------------------------------

DeletePagesCommand::DeletePagesCommand(PageModel& model, std::vector<core::PageId> ids)
    : PageCommand(model), ids_(std::move(ids)) {}

bool DeletePagesCommand::execute() {
    auto removed = model_.extract(ids_);
    if (!record(removed)) return false;
    removed_ = std::move(*removed);
    return true;
}

bool DeletePagesCommand::undo() {
    return record(model_.insertAt(removed_));
}

// --- Rotate -----------------------------------------------------------------

RotatePagesCommand::RotatePagesCommand(PageModel& model, std::vector<core::PageId> ids, int degrees)
    : PageCommand(model), ids_(std::move(ids)), degrees_(degrees) {}

bool RotatePagesCommand::execute() {
    if (degrees_ % 90 != 0 || degrees_ % 360 == 0) {
        return fail(invalid("rotation must be a non-zero multiple of 90 degrees"));
    }
    auto before = currentViews(*model_.snapshot(), ids_);
    if (!record(before)) return false;
    const core::PageRotation delta = core::rotationFromQuarterTurns(degrees_ / 90);
    std::vector<PageModel::ViewUpdate> after = *before;
    for (PageModel::ViewUpdate& update : after) {
        update.view.rotation = core::addRotation(update.view.rotation, delta);
        update.contentRevision = model_.mintContentRevision();
    }
    if (!record(model_.updateViews(after))) return false;
    before_ = std::move(*before);
    after_ = std::move(after);
    return true;
}

bool RotatePagesCommand::undo() {
    return record(model_.updateViews(before_));
}

bool RotatePagesCommand::redo() {
    return record(model_.updateViews(after_));
}

// --- Duplicate ----------------------------------------------------------------

DuplicatePagesCommand::DuplicatePagesCommand(PageModel& model, std::vector<core::PageId> ids)
    : PageCommand(model), ids_(std::move(ids)) {}

bool DuplicatePagesCommand::execute() {
    const PageSnapshotPtr snapshot = model_.snapshot();
    auto views = currentViews(*snapshot, ids_);
    if (!record(views)) return false;
    std::vector<std::size_t> positions;
    positions.reserve(ids_.size());
    for (const core::PageId id : ids_) positions.push_back(snapshot->indexOf(id));
    std::sort(positions.begin(), positions.end());

    std::vector<std::pair<std::size_t, PageEntry>> placed;
    placed.reserve(positions.size());
    std::vector<core::PageId> created;
    created.reserve(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        PageEntry copy = snapshot->at(positions[i]);
        copy.id = model_.mintPageId();
        copy.contentRevision = 0; // a new id: its creation view
        created.push_back(copy.id);
        // Copy i goes right after its original, which itself moved down by
        // the i copies inserted before it.
        placed.emplace_back(positions[i] + i + 1, std::move(copy));
    }
    if (!record(model_.insertAt(placed))) return false;
    placed_ = std::move(placed);
    createdIds_ = std::move(created);
    return true;
}

bool DuplicatePagesCommand::undo() {
    return record(model_.extract(createdIds_));
}

bool DuplicatePagesCommand::redo() {
    return record(model_.insertAt(placed_));
}

// --- Insert -----------------------------------------------------------------

InsertPagesCommand::InsertPagesCommand(PageModel& model, std::vector<PageSource> pages, std::size_t index)
    : PageCommand(model), pages_(std::move(pages)), index_(index) {}

bool InsertPagesCommand::execute() {
    if (pages_.empty()) return fail(invalid("no pages to insert"));
    if (index_ > model_.size()) return fail(invalid("insert position out of range"));
    for (const PageSource& page : pages_) {
        if (page.document == nullptr) return fail(invalid("no source document"));
        if (!isValidViewFor(page.nativeView, page.mediaBox)) {
            return fail(invalid("source page has no presentable view"));
        }
    }
    std::vector<std::pair<std::size_t, PageEntry>> placed;
    placed.reserve(pages_.size());
    std::vector<core::PageId> created;
    created.reserve(pages_.size());
    for (std::size_t i = 0; i < pages_.size(); ++i) {
        const PageSource& page = pages_[i];
        PageEntry entry{model_.mintPageId(), page.document, page.pageIndex, page.nativeView, 0,
                        page.mediaBox, page.nativeView};
        created.push_back(entry.id);
        placed.emplace_back(index_ + i, std::move(entry));
    }
    if (!record(model_.insertAt(placed))) return false;
    placed_ = std::move(placed);
    createdIds_ = std::move(created);
    return true;
}

bool InsertPagesCommand::undo() {
    return record(model_.extract(createdIds_));
}

bool InsertPagesCommand::redo() {
    return record(model_.insertAt(placed_));
}

// --- Crop -------------------------------------------------------------------

CropPagesCommand::CropPagesCommand(PageModel& model, std::vector<core::PageId> ids,
                                   std::optional<pdf::PdfBox> cropBox)
    : PageCommand(model), ids_(std::move(ids)), cropBox_(cropBox) {}

bool CropPagesCommand::execute() {
    const PageSnapshotPtr snapshot = model_.snapshot();
    auto before = currentViews(*snapshot, ids_);
    if (!record(before)) return false;
    std::vector<PageModel::ViewUpdate> after = *before;
    for (PageModel::ViewUpdate& update : after) {
        const PageEntry* entry = snapshot->find(update.id);
        if (entry == nullptr) return fail(invalid("page is not in the document"));
        update.view.cropBox = cropBox_.has_value() ? *cropBox_ : entry->nativeView.cropBox;
        if (!isValidViewFor(update.view, entry->mediaBox)) {
            return fail(invalid("crop box must be non-empty and within the page's media box"));
        }
    }
    for (PageModel::ViewUpdate& update : after) update.contentRevision = model_.mintContentRevision();
    if (!record(model_.updateViews(after))) return false;
    before_ = std::move(*before);
    after_ = std::move(after);
    return true;
}

bool CropPagesCommand::undo() {
    return record(model_.updateViews(before_));
}

bool CropPagesCommand::redo() {
    return record(model_.updateViews(after_));
}

} // namespace rivet::editor
