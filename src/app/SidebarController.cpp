// SPDX-License-Identifier: MPL-2.0
#include "app/SidebarController.hpp"

#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PageThumbnailList.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace rivet::app {

namespace {

// Untrusted signature strings for display: control characters become
// spaces, the text is capped at a UTF-8 boundary.
std::string displayText(std::string_view text) {
    constexpr std::size_t kMaxBytes = 160;
    std::string out;
    out.reserve(std::min(text.size(), kMaxBytes));
    for (const char ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        out.push_back(byte < 0x20 || byte == 0x7F ? ' ' : ch);
    }
    if (out.size() > kMaxBytes) {
        std::size_t cut = kMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) --cut;
        out.resize(cut);
        out += "\xE2\x80\xA6";
    }
    return out;
}

} // namespace

SignatureRows buildSignatureRows(const pdf::PdfSignatureList* list) {
    SignatureRows result;
    const auto add = [&result](std::string title, int depth, std::optional<std::size_t> signature) {
        ui::OutlineRow row;
        row.title = std::move(title);
        row.depth = depth;
        row.hasChildren = false; // details are always shown (no disclosure marker)
        result.rows.push_back(std::move(row));
        result.signatureOfRow.push_back(signature);
    };
    if (list == nullptr) {
        add("Loading signatures\xE2\x80\xA6", 0, std::nullopt);
        return result;
    }
    if (list->signatures.empty()) {
        add("No digital signatures", 0, std::nullopt);
        return result;
    }
    for (std::size_t i = 0; i < list->signatures.size(); ++i) {
        const pdf::PdfSignatureInfo& info = list->signatures[i];
        add(pdf::signatureStatusText(info), 0, i);
        if (info.pageIndex.has_value()) add("Page " + std::to_string(*info.pageIndex + 1), 1, i);
        if (!info.reason.empty()) add("Reason: " + displayText(info.reason), 1, i);
        if (!info.signingTime.empty()) add("Signed: " + displayText(info.signingTime), 1, i);
    }
    if (list->truncated) add("More signatures are not listed", 0, std::nullopt);
    return result;
}

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
            flat.targets.push_back(frame.node->destination);
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
    pagesButton_->setFrame(core::Rect{6.0, 6.0, 56.0, 24.0});
    pagesButton_->setOnClick([this] { setMode(Mode::Pages); });
    container_->addChild(std::move(pagesButton));

    auto outlineButton = std::make_unique<ui::Button>("Outline");
    outlineButton_ = outlineButton.get();
    outlineButton_->setFrame(core::Rect{64.0, 6.0, 66.0, 24.0});
    outlineButton_->setOnClick([this] { setMode(Mode::Outline); });
    container_->addChild(std::move(outlineButton));

    auto signaturesButton = std::make_unique<ui::Button>("Signatures");
    signaturesButton_ = signaturesButton.get();
    signaturesButton_->setFrame(core::Rect{132.0, 6.0, 82.0, 24.0});
    signaturesButton_->setOnClick([this] { setMode(Mode::Signatures); });
    container_->addChild(std::move(signaturesButton));

    auto thumbnails = std::make_unique<ui::PageThumbnailList>();
    thumbnails_ = thumbnails.get();
    container_->addChild(std::move(thumbnails));

    auto outlinePanel = std::make_unique<ui::OutlinePanel>();
    outlinePanel_ = outlinePanel.get();
    outlinePanel_->setOnRowActivated([this](std::size_t row) { activateRow(row); });
    outlinePanel_->setOnExpansionToggled(
        [this](std::size_t row, bool expanded) { setRowExpanded(row, expanded); });
    container_->addChild(std::move(outlinePanel));

    auto signaturesPanel = std::make_unique<ui::OutlinePanel>();
    signaturesPanel_ = signaturesPanel.get();
    signaturesPanel_->setOnRowActivated([this](std::size_t row) { activateSignatureRow(row); });
    container_->addChild(std::move(signaturesPanel));

    setMode(Mode::Pages);
}

void SidebarController::bindTab(DocumentTab* tab) {
    if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
        thumbnails_->clearDocument();
        clearOutline();
        signatures_ = SignatureRows{};
        shownSignatures_ = nullptr;
        signaturesPanel_->setRows({});
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

    // Digital signatures load once per base document (also needed by the
    // save warning); the panel refreshes when they arrive, if this session
    // is still the active one. The delivery dies with the session.
    session->signatures().setOnChanged([this, session] {
        DocumentTab* active = context_.readyActiveTab();
        if (active != nullptr && active->session() == session) rebuildSignatureRows();
    });
    session->signatures().ensureLoaded();
    rebuildSignatureRows();

    // Restore the per-tab current page.
    thumbnails_->setSelectedIndex(tab->currentPage());
}

void SidebarController::pagesChanged() {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr) return;
    thumbnails_->reloadPages();
    thumbnails_->setPageLabels(tab->session()->pageLabels());
    thumbnails_->setSelectedIndex(tab->currentPage());
    rebuildOutlineRows();
}

void SidebarController::setCurrentPage(std::size_t page) {
    thumbnails_->setSelectedIndex(page);
    thumbnails_->revealPage(page);
}

void SidebarController::layout(const core::Rect& frame) {
    container_->setFrame(frame);
    panelFrame_ = core::Rect{0.0, kHeaderHeight, frame.size.width,
                             std::max(0.0, frame.size.height - kHeaderHeight)};
    setMode(mode_);
}

void SidebarController::setMode(Mode mode) {
    mode_ = mode;
    // All panels share one slot; only the active mode's panel occupies it.
    thumbnails_->setFrame(mode == Mode::Pages ? panelFrame_ : kHiddenFrame);
    outlinePanel_->setFrame(mode == Mode::Outline ? panelFrame_ : kHiddenFrame);
    signaturesPanel_->setFrame(mode == Mode::Signatures ? panelFrame_ : kHiddenFrame);
    pagesButton_->setLabel(mode == Mode::Pages ? "Pages \xE2\x80\xA2" : "Pages");
    outlineButton_->setLabel(mode == Mode::Outline ? "Outline \xE2\x80\xA2" : "Outline");
    signaturesButton_->setLabel(mode == Mode::Signatures ? "Signatures \xE2\x80\xA2" : "Signatures");
    container_->invalidate();
}

void SidebarController::rebuildSignatureRows() {
    DocumentTab* tab = context_.readyActiveTab();
    shownSignatures_ = tab != nullptr ? tab->session()->signatures().list() : nullptr;
    signatures_ = buildSignatureRows(shownSignatures_.get());
    signaturesPanel_->setRows(signatures_.rows);
}

void SidebarController::activateSignatureRow(std::size_t row) {
    if (row >= signatures_.signatureOfRow.size() || shownSignatures_ == nullptr) return;
    const std::optional<std::size_t> index = signatures_.signatureOfRow[row];
    if (!index.has_value() || *index >= shownSignatures_->signatures.size()) return;
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr) return;
    const std::optional<std::size_t> page = shownSignatures_->signatures[*index].pageIndex;
    if (!page.has_value()) {
        context_.setStatus("This signature has no visible field");
        return;
    }
    // The page index is in the BASE document: resolve it through the page
    // model (reordered / deleted pages).
    pdf::PdfDestination destination;
    destination.pageIndex = *page;
    const std::optional<editor::PageDestination> resolved = tab->session()->resolveOutlineDestination(destination);
    if (!resolved.has_value()) {
        context_.setStatus("The signature's page is no longer in the document");
        return;
    }
    context_.viewport.goToPage(resolved->index);
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
    // Outline destinations point into the base document: resolve them
    // through the page model (reordered / deleted pages).
    if (row < outline_.targets.size() && outline_.targets[row].has_value()) {
        const std::optional<editor::PageDestination> resolved =
            tab->session()->resolveOutlineDestination(*outline_.targets[row]);
        if (!resolved.has_value()) {
            context_.setStatus("The outline entry's page is no longer in the document");
            return;
        }
        context_.viewport.goToPage(resolved->index);
        return;
    }
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
