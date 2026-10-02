// SPDX-License-Identifier: MPL-2.0
#include "app/PageEditingController.hpp"

#include "app/SidebarController.hpp"
#include "app/StatusBarController.hpp"
#include "core/Error.hpp"
#include "editor/PageCommands.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace rivet::app {

namespace {

// The session's editing lock is held while a save is in flight.
constexpr const char* kLockedMessage = "A save is in progress — editing is paused";

} // namespace

std::optional<PageEditCommand> pageEditCommandForShortcut(const ui::KeyEvent& event) {
    const bool command = event.modifiers.command || event.modifiers.control;
    if (!command || event.key != ui::Key::Character) return std::nullopt;
    const bool shift = event.modifiers.shift;
    if (event.text == "z" || event.text == "Z") return shift ? PageEditCommand::Redo : PageEditCommand::Undo;
    if (!shift && (event.text == "r" || event.text == "R")) return PageEditCommand::RotateRight;
    if (!shift && (event.text == "l" || event.text == "L")) return PageEditCommand::RotateLeft;
    return std::nullopt;
}

PageEditingController::PageEditingController(ShellContext& context, SidebarController& sidebar,
                                             StatusBarController& statusBar)
    : context_(context), sidebar_(sidebar), statusBar_(statusBar), thumbnails_(sidebar.thumbnails()) {
    // Thumbnail intents -> selection and commands (see PageThumbnailList).
    thumbnails_.setOnRowClicked([this](std::size_t row, ui::PageThumbnailList::ClickGesture gesture) {
        handleRowClicked(row, gesture);
    });
    thumbnails_.setOnNavigate([this](int delta, bool extend) { handleNavigate(delta, extend); });
    thumbnails_.setOnDeleteRequested([this] { deletePages(); });
    thumbnails_.setOnSelectAllRequested([this] { selectAllPages(); });
    thumbnails_.setOnMoveRequested([this](std::size_t gap) { dropSelectionAtGap(gap); });
    thumbnails_.setOnFocusRequested([this] { context_.setFocus(&thumbnails_); });

    // The tool never mutates the document: its verdicts run as commands.
    cropTool_.setCallbacks(CropTool::Callbacks{
        [this](const pdf::PdfBox& cropBox) {
            if (cropTargets_.empty()) {
                endCrop();
                return;
            }
            DocumentTab* tab = activeTab();
            if (tab == nullptr || tab->session() == nullptr) {
                endCrop();
                return;
            }
            if (run(std::make_unique<editor::CropPagesCommand>(tab->session()->pageModel(), cropTargets_,
                                                               cropBox),
                    "Crop")) {
                endCrop();
            } else {
                // Keep the tool open so the rectangle can be adjusted.
            }
        },
        [this] {
            DocumentTab* tab = activeTab();
            if (tab != nullptr && !cropTargets_.empty()) {
                run(std::make_unique<editor::CropPagesCommand>(tab->session()->pageModel(), cropTargets_,
                                                               std::nullopt),
                    "Reset Crop");
            }
            endCrop();
        },
        [this] { endCrop(); },
    });
}

PageEditingController::~PageEditingController() {
    // The workspace (and every session) outlives the controllers: drop the
    // page-model observers so no callback fires into a dead controller.
    for (const TabId tabId : observedTabs_) {
        if (DocumentTab* tab = context_.workspace.tabById(tabId); tab != nullptr && tab->session() != nullptr) {
            tab->session()->setOnPageModelChanged(nullptr);
        }
    }
}

DocumentTab* PageEditingController::activeTab() const { return context_.readyActiveTab(); }

const editor::PageSelection& PageEditingController::selectionOf(const DocumentTab* tab) const {
    static const editor::PageSelection kEmpty;
    const auto it = selections_.find(tab->id());
    return it != selections_.end() ? it->second : kEmpty;
}

editor::PageSelection* PageEditingController::activeSelection() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return nullptr;
    return &selections_[tab->id()];
}

std::vector<core::PageId> PageEditingController::targetPages() const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return {};
    const editor::PageSelection& selection = selectionOf(tab);
    const editor::PageSnapshotPtr& snapshot = tab->session()->pageSnapshot();
    if (!selection.empty()) return selection.inOrder(*snapshot);
    const core::PageId current = tab->session()->pageId(tab->currentPage());
    if (snapshot->find(current) != nullptr) return {current};
    return {};
}

void PageEditingController::bindTab(DocumentTab* tab) {
    endCrop();
    prunePageSelections();
    if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
        syncSelectionView();
        return;
    }
    selections_.try_emplace(tab->id());
    // One observer per session; (re)installing it for the newly active tab
    // keeps earlier tabs' observers alive (background edits stay handled).
    if (std::find(observedTabs_.begin(), observedTabs_.end(), tab->id()) == observedTabs_.end()) {
        observedTabs_.push_back(tab->id());
        tab->session()->setOnPageModelChanged([this, tabId = tab->id()](const editor::PageModelChange& change) {
            handlePageModelChanged(tabId, change);
        });
    }
    syncSelectionView();
}

bool PageEditingController::run(std::unique_ptr<editor::Command> command, const char* what) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return false;
    const core::Status status = tab->session()->execute(std::move(command));
    if (!status.has_value()) {
        context_.setStatus(std::format("{}: {}", what, core::describe(status.error())));
        return false;
    }
    return true;
}

void PageEditingController::handlePageModelChanged(TabId tabId, const editor::PageModelChange& change) {
    if (auto it = selections_.find(tabId); it != selections_.end()) it->second.applyChange(change);

    DocumentTab* tab = context_.workspace.tabById(tabId);
    if (tab == nullptr || tab->session() == nullptr) return;

    // Text selection spans removed/re-rendered pages; the search result set
    // indexes pages that may have moved. Both re-derive from the new model.
    if (editor::textSelectionInvalidatedBy(tab->selection().selection(), change)) {
        tab->selection().clear();
    }
    if (tab->search() != nullptr) tab->search()->handlePageModelChanged(change);

    const DocumentTab* active = context_.workspace.activeTab();
    if (active == nullptr || tab->id() != active->id()) return;

    // The crop tool holds a page index and a frame (view/mediaBox) captured
    // at begin: any page-model change (undo/redo, delete, rotate, a move) can
    // leave them stale, so the tool closes WITHOUT applying. The tool's own
    // Apply/Reset commands land here too; their callbacks end the tool
    // anyway (endCrop is idempotent).
    if (isCropping()) endCrop();

    // The active tab's visible state follows the new model. The viewport
    // anchor was recorded at the last current-page update, i.e. BEFORE the
    // edit: resolve it against the new order.
    sidebar_.pagesChanged();
    editor::DocumentSession* session = tab->session();
    std::optional<std::size_t> anchorIndex;
    double anchorFraction = 0.0;
    if (const std::optional<ui::PdfViewport::PageAnchor> anchor = context_.viewport.pageAnchor();
        anchor.has_value()) {
        const std::size_t index = session->pageIndexFor(anchor->page);
        if (index != editor::DocumentSession::kInvalidPage) {
            anchorIndex = index;
            anchorFraction = anchor->fraction;
        }
    }
    context_.viewport.documentLayoutChanged(anchorIndex, anchorFraction);
    syncSelectionView();
    statusBar_.updatePageIndicator();
}

void PageEditingController::syncSelectionView() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) {
        statusBar_.setSelectionSummary("");
        return;
    }
    const editor::PageSelection& selection = selectionOf(tab);
    const editor::PageSnapshotPtr& snapshot = tab->session()->pageSnapshot();
    std::vector<bool> selected;
    selected.reserve(snapshot->size());
    for (const editor::PageEntry& entry : snapshot->entries()) {
        selected.push_back(selection.contains(entry.id));
    }
    std::optional<std::size_t> active;
    const std::size_t activeIndex = tab->session()->pageIndexFor(selection.active());
    if (activeIndex != editor::DocumentSession::kInvalidPage) active = activeIndex;
    thumbnails_.setPageSelection(std::move(selected), active);
    statusBar_.setSelectionSummary(selectionSummary());
}

std::string PageEditingController::selectionSummary() const {
    const DocumentTab* tab = activeTab();
    if (tab == nullptr) return "";
    const std::size_t count = selectionOf(tab).count();
    if (count == 0) return "";
    return std::format("{} page{} selected", count, count == 1 ? "" : "s");
}

void PageEditingController::handleRowClicked(std::size_t row, ui::PageThumbnailList::ClickGesture gesture) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr || row >= tab->session()->pageCount()) return;
    editor::PageSelection* selectionPtr = activeSelection();
    if (selectionPtr == nullptr) return;
    editor::PageSelection& selection = *selectionPtr;
    const core::PageId page = tab->session()->pageId(row);
    switch (gesture) {
    case ui::PageThumbnailList::ClickGesture::Replace:
        selection.select(page);
        // A plain click also navigates (the pre-editing behavior).
        context_.viewport.goToPage(row);
        break;
    case ui::PageThumbnailList::ClickGesture::Toggle:
        selection.toggle(page);
        break;
    case ui::PageThumbnailList::ClickGesture::Extend:
        selection.selectRange(page, *tab->session()->pageSnapshot());
        break;
    }
    syncSelectionView();
}

void PageEditingController::handleNavigate(int delta, bool extend) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr || tab->session()->pageCount() == 0) return;
    const editor::PageSnapshotPtr& snapshot = tab->session()->pageSnapshot();
    editor::PageSelection* selectionPtr = activeSelection();
    if (selectionPtr == nullptr) return;
    editor::PageSelection& selection = *selectionPtr;
    std::size_t row = tab->currentPage();
    if (const std::size_t active = tab->session()->pageIndexFor(selection.active());
        active != editor::DocumentSession::kInvalidPage) {
        row = active;
    }
    const long long moved = static_cast<long long>(row) + delta;
    const long long last = static_cast<long long>(tab->session()->pageCount()) - 1;
    const std::size_t target = static_cast<std::size_t>(std::clamp(moved, 0LL, last));
    const core::PageId page = tab->session()->pageId(target);
    if (extend) {
        selection.selectRange(page, *snapshot);
    } else {
        selection.select(page);
        context_.viewport.goToPage(target);
    }
    syncSelectionView();
}

bool PageEditingController::canPerform(PageEditCommand command) const {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr) return false;
    editor::DocumentSession& session = *tab->session();
    switch (command) {
    case PageEditCommand::Undo:
        return !session.isEditingLocked() && session.commands().canUndo();
    case PageEditCommand::Redo:
        return !session.isEditingLocked() && session.commands().canRedo();
    case PageEditCommand::RotateRight:
    case PageEditCommand::RotateLeft:
    case PageEditCommand::DeletePages:
    case PageEditCommand::DuplicatePages:
    case PageEditCommand::MovePagesUp:
    case PageEditCommand::MovePagesDown:
    case PageEditCommand::SelectAllPages:
    case PageEditCommand::Crop:
    case PageEditCommand::ResetCrop:
        return true;
    }
    return false;
}

void PageEditingController::perform(PageEditCommand command) {
    switch (command) {
    case PageEditCommand::Undo: undo(); break;
    case PageEditCommand::Redo: redo(); break;
    case PageEditCommand::RotateRight: rotate(90); break;
    case PageEditCommand::RotateLeft: rotate(-90); break;
    case PageEditCommand::DeletePages: deletePages(); break;
    case PageEditCommand::DuplicatePages: duplicatePages(); break;
    case PageEditCommand::MovePagesUp: movePagesBy(-1); break;
    case PageEditCommand::MovePagesDown: movePagesBy(1); break;
    case PageEditCommand::SelectAllPages: selectAllPages(); break;
    case PageEditCommand::Crop: beginCrop(); break;
    case PageEditCommand::ResetCrop: resetCrop(); break;
    }
}

bool PageEditingController::handleShortcut(const ui::KeyEvent& event) {
    const std::optional<PageEditCommand> command = pageEditCommandForShortcut(event);
    if (!command.has_value()) return false;
    perform(*command);
    return true;
}

bool PageEditingController::handleToolKey(const ui::KeyEvent& event) {
    if (!cropTool_.isActive()) return false;
    return cropTool_.onKey(context_.viewport, event);
}

void PageEditingController::rotate(int degrees) {
    const std::vector<core::PageId> targets = targetPages();
    if (targets.empty()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    if (tab->session()->isEditingLocked()) {
        context_.setStatus(kLockedMessage);
        return;
    }
    run(std::make_unique<editor::RotatePagesCommand>(tab->session()->pageModel(), targets, degrees),
        degrees > 0 ? "Rotate Right" : "Rotate Left");
}

void PageEditingController::deletePages() {
    const std::vector<core::PageId> targets = targetPages();
    if (targets.empty()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    if (tab->session()->isEditingLocked()) {
        context_.setStatus(kLockedMessage);
        return;
    }
    run(std::make_unique<editor::DeletePagesCommand>(tab->session()->pageModel(), targets), "Delete Pages");
}

void PageEditingController::duplicatePages() {
    const std::vector<core::PageId> targets = targetPages();
    if (targets.empty()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    if (tab->session()->isEditingLocked()) {
        context_.setStatus(kLockedMessage);
        return;
    }
    auto command = std::make_unique<editor::DuplicatePagesCommand>(tab->session()->pageModel(), targets);
    editor::DuplicatePagesCommand* raw = command.get();
    if (!run(std::move(command), "Duplicate Pages")) return;
    // The duplicates become the selection (Preview-style); originals stay.
    editor::PageSelection* selectionPtr = activeSelection();
    if (selectionPtr == nullptr) return;
    editor::PageSelection& selection = *selectionPtr;
    selection.clear();
    for (const core::PageId created : raw->createdIds()) selection.toggle(created);
    syncSelectionView();
}

void PageEditingController::moveBlock(const std::vector<core::PageId>& ids, std::size_t destination) {
    if (ids.empty()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    if (tab->session()->isEditingLocked()) {
        context_.setStatus(kLockedMessage);
        return;
    }
    run(std::make_unique<editor::MovePagesCommand>(tab->session()->pageModel(), ids, destination),
        "Move Pages");
}

void PageEditingController::movePagesBy(int delta) {
    const std::vector<core::PageId> ids = targetPages();
    if (ids.empty()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    const editor::PageSnapshotPtr& snapshot = tab->session()->pageSnapshot();
    const std::size_t first = snapshot->indexOf(ids.front());
    const std::size_t maxDestination = snapshot->size() - ids.size();
    const long long destination =
        std::clamp(static_cast<long long>(first) + delta, 0LL, static_cast<long long>(maxDestination));
    if (static_cast<std::size_t>(destination) == first) return; // no-op: no command
    moveBlock(ids, static_cast<std::size_t>(destination));
}

void PageEditingController::movePagesTo(std::size_t index) {
    const std::vector<core::PageId> ids = targetPages();
    if (ids.empty()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    const editor::PageSnapshotPtr& snapshot = tab->session()->pageSnapshot();
    const std::size_t first = snapshot->indexOf(ids.front());
    const std::size_t destination =
        std::min(index, snapshot->size() - ids.size());
    if (destination == first) return; // no-op: no command
    moveBlock(ids, destination);
}

void PageEditingController::dropSelectionAtGap(std::size_t gap) {
    const std::vector<core::PageId> ids = targetPages();
    if (ids.empty()) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    const editor::PageSnapshotPtr& snapshot = tab->session()->pageSnapshot();
    const std::size_t destination =
        editor::MovePagesCommand::destinationForGap(*snapshot, ids, gap);
    if (destination == snapshot->indexOf(ids.front())) return; // no-op drop: no command
    moveBlock(ids, destination);
}

void PageEditingController::selectAllPages() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    if (editor::PageSelection* selection = activeSelection(); selection != nullptr) {
        selection->selectAll(*tab->session()->pageSnapshot());
    }
    syncSelectionView();
}

void PageEditingController::undo() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    if (tab->session()->isEditingLocked()) {
        context_.setStatus(kLockedMessage);
        return;
    }
    if (!tab->session()->undo()) context_.setStatus("Nothing to undo");
}

void PageEditingController::redo() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    if (tab->session()->isEditingLocked()) {
        context_.setStatus(kLockedMessage);
        return;
    }
    if (!tab->session()->redo()) context_.setStatus("Nothing to redo");
}

void PageEditingController::beginCrop() {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr || isCropping()) return;
    const editor::PageSnapshotPtr& snapshot = tab->session()->pageSnapshot();
    const core::PageId current = tab->session()->pageId(tab->currentPage());
    const editor::PageEntry* entry = snapshot->find(current);
    if (entry == nullptr) return;
    // The tool edits the current page; the command applies to the whole
    // selection when the current page belongs to it.
    const std::vector<core::PageId> targets = targetPages();
    cropTargets_ =
        std::find(targets.begin(), targets.end(), current) != targets.end() ? targets
                                                                            : std::vector<core::PageId>{current};
    cropTool_.begin(snapshot->indexOf(current), CropFrame{entry->view, entry->mediaBox});
    context_.viewport.setActiveTool(&cropTool_);
    context_.setStatus("Drag the handles to crop; Enter applies, Esc cancels");
}

void PageEditingController::resetCrop() {
    if (isCropping()) {
        cropTool_.reset(); // -> onReset callback: reset command + endCrop
        return;
    }
    const std::vector<core::PageId> targets = targetPages();
    DocumentTab* tab = activeTab();
    if (targets.empty() || tab == nullptr || tab->session() == nullptr) return;
    run(std::make_unique<editor::CropPagesCommand>(tab->session()->pageModel(), targets, std::nullopt),
        "Reset Crop");
}

void PageEditingController::endCrop() {
    // The tool may have deactivated itself before calling back (cancel does);
    // the uninstall must happen regardless.
    cropTool_.end();
    context_.viewport.setActiveTool(nullptr);
    cropTargets_.clear();
}

void PageEditingController::prunePageSelections() {
    std::erase_if(selections_, [this](const auto& item) {
        return context_.workspace.tabById(item.first) == nullptr;
    });
    std::erase_if(observedTabs_, [this](TabId tabId) {
        return context_.workspace.tabById(tabId) == nullptr;
    });
}

} // namespace rivet::app
