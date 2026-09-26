// SPDX-License-Identifier: MPL-2.0
#include "app/SidebarController.hpp"

#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PageThumbnailList.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace rivet::app {

// Iterative DFS with an explicit path stack (the tree is depth-bounded by
// the adapter, but recursion is still avoided here for uniformity). The
// synthetic root is SKIPPED: its children are the top-level rows.
FlattenedOutline flattenOutline(const pdf::PdfOutlineNode& root, const std::set<OutlinePath>& expanded,
                                std::size_t pageCount) {
    FlattenedOutline flat;
    struct Frame {
        const pdf::PdfOutlineNode* node;
        OutlinePath path;
        std::size_t childIndex;
    };
    std::vector<Frame> stack;
    for (std::size_t i = root.children.size(); i > 0; --i) {
        stack.push_back(Frame{&root.children[i - 1], OutlinePath{i - 1}, 0});
    }

    while (!stack.empty()) {
        Frame& frame = stack.back();
        const bool isExpanded = !frame.node->children.empty() && expanded.count(frame.path) > 0;
        if (frame.childIndex == 0) {
            ui::OutlineRow row;
            row.title = frame.node->title;
            row.depth = static_cast<int>(frame.path.size());
            row.hasChildren = !frame.node->children.empty();
            row.expanded = isExpanded;
            // Outline rows navigate to the node's destination page.
            flat.destinations.push_back(frame.node->destination.has_value() &&
                                                frame.node->destination->pageIndex < pageCount
                                            ? frame.node->destination->pageIndex
                                            : 0);
            flat.paths.push_back(frame.path);
            flat.rows.push_back(std::move(row));
        }
        if (isExpanded && frame.childIndex < frame.node->children.size()) {
            const pdf::PdfOutlineNode* child = &frame.node->children[frame.childIndex];
            OutlinePath childPath = frame.path;
            childPath.push_back(frame.childIndex);
            ++frame.childIndex;
            // May reallocate the stack: `frame` is not used after this.
            stack.push_back(Frame{child, std::move(childPath), 0});
        } else {
            stack.pop_back();
        }
    }
    return flat;
}

SidebarController::SidebarController(ShellContext& context, ui::Widget& parent) : context_(context) {
    auto container = std::make_unique<ui::Container>();
    container_ = container.get();
    container_->setBackgroundColor(ui::Color::rgba(0.93, 0.93, 0.93, 1.0));
    parent.addChild(std::move(container));

    auto pagesButton = std::make_unique<ui::Button>("Pages");
    pagesButton_ = pagesButton.get();
    pagesButton_->setFrame(core::Rect{8.0, 6.0, 64.0, 24.0});
    pagesButton_->setOnClick([this] { setMode(Mode::Pages); });
    container_->addChild(std::move(pagesButton));

    auto outlineButton = std::make_unique<ui::Button>("Outline");
    outlineButton_ = outlineButton.get();
    outlineButton_->setFrame(core::Rect{76.0, 6.0, 72.0, 24.0});
    outlineButton_->setOnClick([this] { setMode(Mode::Outline); });
    container_->addChild(std::move(outlineButton));

    auto thumbnails = std::make_unique<ui::PageThumbnailList>();
    thumbnails_ = thumbnails.get();
    container_->addChild(std::move(thumbnails));

    auto outlinePanel = std::make_unique<ui::OutlinePanel>();
    outlinePanel_ = outlinePanel.get();
    outlinePanel_->setOnRowActivated([this](std::size_t row) { activateRow(row); });
    outlinePanel_->setOnExpansionToggled(
        [this](std::size_t row, bool expanded) { setRowExpanded(row, expanded); });
    container_->addChild(std::move(outlinePanel));

    setMode(Mode::Pages);
}

void SidebarController::bindTab(DocumentTab* tab) {
    if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
        thumbnails_->clearDocument();
        clearOutline();
        return;
    }
    editor::DocumentSession* session = tab->session();
    thumbnails_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                             [session] { return session->revision(); });
    thumbnails_->setPageLabels(session->pageLabels());

    // Expansion state is per document: a different document starts over
    // (top level expanded once its outline arrives).
    if (session->id() != outlineDocument_) {
        outlineDocument_ = session->id();
        expandedPaths_.clear();
        topLevelExpanded_ = false;
    }
    rebuildOutlineRows();

    // Restore the per-tab current page.
    thumbnails_->setSelectedIndex(tab->currentPage());
}

void SidebarController::setCurrentPage(std::size_t page) {
    thumbnails_->setSelectedIndex(page);
    thumbnails_->revealPage(page);
}

void SidebarController::layout(const core::Rect& frame) {
    container_->setFrame(frame);
    panelFrame_ = core::Rect{0.0, kHeaderHeight, frame.size.width,
                             std::max(0.0, frame.size.height - kHeaderHeight)};
    // Both panels share one slot; only the active mode's panel occupies it.
    const bool pages = mode_ == Mode::Pages;
    thumbnails_->setFrame(pages ? panelFrame_ : kHiddenFrame);
    outlinePanel_->setFrame(pages ? kHiddenFrame : panelFrame_);
}

void SidebarController::setMode(Mode mode) {
    mode_ = mode;
    const bool pages = mode == Mode::Pages;
    thumbnails_->setFrame(pages ? panelFrame_ : kHiddenFrame);
    outlinePanel_->setFrame(pages ? kHiddenFrame : panelFrame_);
    pagesButton_->setLabel(pages ? "Pages •" : "Pages");
    outlineButton_->setLabel(!pages ? "Outline •" : "Outline");
    container_->invalidate();
}

void SidebarController::setRowExpanded(std::size_t row, bool expanded) {
    if (row >= outline_.paths.size()) return;
    if (expanded) {
        expandedPaths_.insert(outline_.paths[row]);
    } else {
        expandedPaths_.erase(outline_.paths[row]);
    }
    rebuildOutlineRows();
}

void SidebarController::activateRow(std::size_t row) {
    if (row >= outline_.destinations.size()) return;
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr) return;
    const std::size_t pageIndex = outline_.destinations[row];
    if (pageIndex >= tab->session()->pageCount()) return;
    context_.viewport.goToPage(pageIndex);
}

void SidebarController::clearOutline() {
    outline_ = FlattenedOutline{};
    outlinePanel_->setRows({});
}

void SidebarController::rebuildOutlineRows() {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr) {
        clearOutline();
        return;
    }
    // The outline loads on the session's navigation stream (a PDFium walk
    // never runs on the main thread); rebuild once it arrives, if this
    // session is still the active one. The delivery dies with the session.
    editor::DocumentSession* session = tab->session();
    const editor::LinkService::Outline outline = session->linkService().cachedOutline();
    if (outline == nullptr) {
        clearOutline();
        session->linkService().requestOutline([this, session](editor::LinkService::Outline) {
            DocumentTab* active = context_.readyActiveTab();
            if (active != nullptr && active->session() == session) rebuildOutlineRows();
        });
        return;
    }
    if (!outline->has_value()) {
        clearOutline();
        return;
    }

    const pdf::PdfOutlineNode& root = **outline;
    // Top-level nodes start expanded so the outline is readable on first
    // open; applied once per document so the user can collapse them.
    if (!topLevelExpanded_) {
        topLevelExpanded_ = true;
        for (std::size_t i = 0; i < root.children.size(); ++i) expandedPaths_.insert(OutlinePath{i});
    }
    outline_ = flattenOutline(root, expandedPaths_, session->pageCount());
    outlinePanel_->setRows(outline_.rows);
}

} // namespace rivet::app
