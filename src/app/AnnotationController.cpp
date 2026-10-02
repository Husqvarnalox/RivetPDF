// SPDX-License-Identifier: MPL-2.0
#include "app/AnnotationController.hpp"

#include "app/AnnotationPainter.hpp"
#include "core/Error.hpp"
#include "editor/AnnotationGeometry.hpp"
#include "editor/SelectionText.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextArea.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace rivet::app {

namespace {

using Intent = AnnotationInteraction::Intent;

// The session's editing lock is held while a save is in flight.
constexpr const char* kLockedMessage = "A save is in progress — editing is paused";

AnnotationInteraction::HitInfo hitInfoFor(const editor::AnnotationView& view) {
    AnnotationInteraction::HitInfo info;
    info.id = view.id;
    info.canMove = (view.caps & editor::kCapMove) != 0;
    info.canResize = (view.caps & editor::kCapResize) != 0;
    info.canEditContents = (view.caps & editor::kCapEditContents) != 0;
    info.canDelete = (view.caps & editor::kCapDelete) != 0;
    info.isLine = view.kind == pdf::PdfAnnotationKind::Line || view.kind == pdf::PdfAnnotationKind::Arrow;
    info.bounds = view.bounds;
    info.lineStart = view.lineStart;
    info.lineEnd = view.lineEnd;
    return info;
}

editor::DisplayQuad quadOf(const core::Rect& rect) {
    return editor::DisplayQuad{core::Point{rect.minX(), rect.minY()}, core::Point{rect.maxX(), rect.minY()},
                               core::Point{rect.minX(), rect.maxY()}, core::Point{rect.maxX(), rect.maxY()}};
}

} // namespace

AnnotationController::AnnotationController(ShellContext& context, ui::Widget& parent)
    : context_(context), layer_(*this) {
    for (std::size_t i = 0; i < kAnnotationToolCount; ++i) {
        styles_[i] = defaultToolStyle(static_cast<AnnotationTool>(i));
    }

    interaction_.setHitTest([this](std::size_t page, core::Point point,
                                   double tolerance) -> std::optional<AnnotationInteraction::HitInfo> {
        DocumentTab* tab = activeTab();
        if (tab == nullptr || page >= tab->session()->pageCount()) return std::nullopt;
        editor::DocumentSession& session = *tab->session();
        const core::PageId pageId = session.pageId(page);
        const std::optional<core::AnnotationId> id = session.annotations().hitTest(pageId, point, tolerance);
        if (!id.has_value()) return std::nullopt;
        const std::optional<editor::AnnotationView> view = session.annotations().find(pageId, *id);
        if (!view.has_value()) return std::nullopt;
        return hitInfoFor(*view);
    });

    // The note editor: a TextArea with a Done button, floating over the
    // viewport beside the note while open.
    auto panel = std::make_unique<ui::Container>();
    notePanel_ = panel.get();
    notePanel_->setBackgroundColor(ui::Color::rgba(0.97, 0.97, 0.97, 1.0));
    parent.addChild(std::move(panel));

    auto area = std::make_unique<ui::TextArea>("Note");
    noteArea_ = area.get();
    noteArea_->setMaxBytes(pdf::kMaxContentsBytes);
    noteArea_->setFrame(core::Rect{8.0, 8.0, kNotePanelWidth - 16.0, kNotePanelHeight - 48.0});
    noteArea_->setOnFocusRequested([this] { context_.setFocus(noteArea_); });
    noteArea_->setOnCommit([this] { closeNoteEditor(); });
    noteArea_->setOnEscape([this] { closeNoteEditor(); });
    notePanel_->addChild(std::move(area));

    auto done = std::make_unique<ui::Button>("Done");
    noteDone_ = done.get();
    noteDone_->setFrame(core::Rect{kNotePanelWidth - 8.0 - 64.0, kNotePanelHeight - 32.0, 64.0, 24.0});
    noteDone_->setOnClick([this] { closeNoteEditor(); });
    notePanel_->addChild(std::move(done));

    notePanel_->setFrame(kHiddenFrame);
    context_.viewport.setAnnotationLayer(&layer_);
}

AnnotationController::~AnnotationController() {
    context_.viewport.setAnnotationLayer(nullptr);
    // The workspace (and every session) outlives the controllers: drop the
    // observers so no callback fires into a dead controller.
    for (const TabId tabId : observedTabs_) {
        if (DocumentTab* tab = context_.workspace.tabById(tabId); tab != nullptr && tab->session() != nullptr) {
            tab->session()->setOnAnnotationsChanged(nullptr);
        }
    }
}

void AnnotationController::bindTab(DocumentTab* tab) {
    // Commit against the tab the editor was opened on, then switch.
    closeNoteEditor();
    interaction_.cancelGesture();
    interaction_.setSelection(std::nullopt);
    for (auto it = selected_.begin(); it != selected_.end();) {
        it = context_.workspace.tabById(it->first) == nullptr ? selected_.erase(it) : std::next(it);
    }
    std::erase_if(observedTabs_, [this](TabId id) { return context_.workspace.tabById(id) == nullptr; });
    if (tab != nullptr && tab->state() == DocumentTab::State::Ready &&
        std::find(observedTabs_.begin(), observedTabs_.end(), tab->id()) == observedTabs_.end()) {
        observedTabs_.push_back(tab->id());
        tab->session()->setOnAnnotationsChanged([this](core::PageId) { repaint(); });
    }
    notifyState();
    repaint();
}

void AnnotationController::layout(const core::Rect& viewportFrame) {
    viewportFrame_ = viewportFrame;
    placeNotePanel();
}

void AnnotationController::notifyState() {
    if (onStateChanged_) onStateChanged_();
}

void AnnotationController::repaint() { context_.viewport.invalidate(); }

// --- Tool and style state --------------------------------------------------------

void AnnotationController::setTool(AnnotationTool tool) {
    closeNoteEditor();
    const bool changed = tool != interaction_.tool();
    interaction_.setTool(tool); // cancels a running gesture
    if (changed) select(std::nullopt);
    if (isMarkupTool(tool) && textSelectionNonEmpty()) convertTextSelection(tool);
    notifyState();
    repaint();
}

editor::AnnotationStyle AnnotationController::displayedStyle() const {
    if (const std::optional<Resolved> selected = resolveSelected(); selected.has_value()) {
        return selected->view.style;
    }
    return styles_[static_cast<std::size_t>(interaction_.tool())].style;
}

pdf::PdfStampName AnnotationController::displayedStampName() const {
    if (const std::optional<Resolved> selected = resolveSelected();
        selected.has_value() && selected->view.kind == pdf::PdfAnnotationKind::Stamp) {
        return selected->view.stampName;
    }
    return styles_[static_cast<std::size_t>(AnnotationTool::Stamp)].stampName;
}

void AnnotationController::restyleSelection(editor::StylePatch patch) {
    DocumentTab* tab = activeTab();
    const std::optional<core::AnnotationId> id = selectedId();
    if (tab == nullptr || !id.has_value()) return;
    run(editor::restyleAnnotations(*tab->session(), {*id}, std::move(patch)));
    notifyState();
}

void AnnotationController::setColor(const pdf::PdfColor& color) {
    if (const std::optional<Resolved> selected = resolveSelected(); selected.has_value()) {
        editor::StylePatch patch;
        patch.color = color;
        if (selected->view.style.interiorColor.has_value()) {
            patch.interiorColor.emplace(std::optional<pdf::PdfColor>{color});
        }
        restyleSelection(std::move(patch));
        return;
    }
    if (interaction_.tool() == AnnotationTool::Select) return;
    editor::AnnotationStyle& style = styles_[static_cast<std::size_t>(interaction_.tool())].style;
    style.color = color;
    if (style.interiorColor.has_value()) style.interiorColor = color;
    notifyState();
}

void AnnotationController::setOpacity(float opacity) {
    if (selectedId().has_value()) {
        editor::StylePatch patch;
        patch.opacity = opacity;
        restyleSelection(std::move(patch));
        return;
    }
    if (interaction_.tool() == AnnotationTool::Select) return;
    styles_[static_cast<std::size_t>(interaction_.tool())].style.opacity = opacity;
    notifyState();
}

void AnnotationController::setBorderWidth(float width) {
    if (selectedId().has_value()) {
        editor::StylePatch patch;
        patch.borderWidth = width;
        restyleSelection(std::move(patch));
        return;
    }
    if (interaction_.tool() == AnnotationTool::Select) return;
    styles_[static_cast<std::size_t>(interaction_.tool())].style.borderWidth = width;
    notifyState();
}

void AnnotationController::setFillEnabled(bool enabled) {
    if (const std::optional<Resolved> selected = resolveSelected(); selected.has_value()) {
        editor::StylePatch patch;
        if (enabled) patch.interiorColor.emplace(std::optional<pdf::PdfColor>{selected->view.style.color});
        else patch.interiorColor.emplace(std::optional<pdf::PdfColor>{});
        restyleSelection(std::move(patch));
        return;
    }
    if (interaction_.tool() == AnnotationTool::Select) return;
    editor::AnnotationStyle& style = styles_[static_cast<std::size_t>(interaction_.tool())].style;
    if (enabled) style.interiorColor = style.color;
    else style.interiorColor.reset();
    notifyState();
}

void AnnotationController::setStampName(pdf::PdfStampName name) {
    styles_[static_cast<std::size_t>(AnnotationTool::Stamp)].stampName = name;
    notifyState();
}

// --- Selection ---------------------------------------------------------------------

std::optional<AnnotationController::Resolved> AnnotationController::resolveSelected() const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return std::nullopt;
    const auto it = selected_.find(tab->id());
    if (it == selected_.end()) return std::nullopt;
    const editor::DocumentSession& session = *tab->session();
    const std::optional<editor::AnnotationService::Located> located = session.annotations().locate(it->second);
    if (located.has_value()) {
        const std::size_t index = session.pageIndexFor(located->page);
        if (index != editor::DocumentSession::kInvalidPage) {
            if (std::optional<editor::AnnotationView> view = session.annotations().find(located->page, it->second);
                view.has_value()) {
                return Resolved{located->page, index, std::move(*view)};
            }
        }
    }
    selected_.erase(it); // no longer resolves (undone, deleted, page gone)
    return std::nullopt;
}

std::optional<core::AnnotationId> AnnotationController::selectedId() const {
    const std::optional<Resolved> selected = resolveSelected();
    if (!selected.has_value()) return std::nullopt;
    return selected->view.id;
}

std::optional<AnnotationInteraction::Selected> AnnotationController::currentSelection() const {
    const std::optional<Resolved> selected = resolveSelected();
    if (!selected.has_value()) return std::nullopt;
    return AnnotationInteraction::Selected{selected->pageIndex, hitInfoFor(selected->view)};
}

void AnnotationController::select(std::optional<core::AnnotationId> id) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    if (id.has_value()) selected_[tab->id()] = *id;
    else selected_.erase(tab->id());
    interaction_.setSelection(currentSelection());
    notifyState();
    repaint();
}

std::shared_ptr<const std::vector<editor::AnnotationView>>
AnnotationController::pageAnnotations(std::size_t pageIndex) const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return nullptr;
    return tab->session()->annotations().annotations(tab->session()->pageId(pageIndex));
}

editor::AnnotationStyle AnnotationController::previewStyle() const {
    return styles_[static_cast<std::size_t>(interaction_.tool())].style;
}

bool AnnotationController::textSelectionNonEmpty() const {
    const DocumentTab* tab = activeTab();
    // Active AND spanning at least one character position (a plain click
    // leaves an active-but-collapsed selection).
    return tab != nullptr && !tab->selection().empty() && !tab->selection().selection().empty();
}

// --- Running commands -------------------------------------------------------------------

bool AnnotationController::lockedForEditing(const DocumentTab& tab) const {
    return tab.session() != nullptr && tab.session()->isEditingLocked();
}

std::optional<std::vector<core::AnnotationId>>
AnnotationController::run(core::Result<editor::AnnotationEdit> edit) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->session() == nullptr) return std::nullopt;
    if (lockedForEditing(*tab)) {
        context_.setStatus(kLockedMessage);
        return std::nullopt;
    }
    if (!edit.has_value()) {
        context_.setStatus(std::format("Annotate: {}", core::describe(edit.error())));
        return std::nullopt;
    }
    const core::Status status = tab->session()->execute(std::move(edit->command));
    if (!status.has_value()) {
        context_.setStatus(std::format("Annotate: {}", core::describe(status.error())));
        return std::nullopt;
    }
    repaint();
    return std::move(edit->ids);
}

editor::AnnotationDraft AnnotationController::draftFor(AnnotationTool tool, core::PageId page) const {
    const ToolStyle& style = styles_[static_cast<std::size_t>(tool)];
    editor::AnnotationDraft draft;
    draft.page = page;
    draft.kind = annotationKindForTool(tool).value_or(pdf::PdfAnnotationKind::Other);
    draft.style = style.style;
    draft.stampName = style.stampName;
    return draft;
}

void AnnotationController::create(const Intent& intent) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || intent.page >= tab->session()->pageCount()) return;
    editor::DocumentSession& session = *tab->session();
    const core::PageId page = session.pageId(intent.page);

    AnnotationTool tool = intent.tool;
    switch (intent.kind) {
    case Intent::Kind::CreateNote: tool = AnnotationTool::Note; break;
    case Intent::Kind::CreateInk: tool = AnnotationTool::Ink; break;
    case Intent::Kind::CreateStamp: tool = AnnotationTool::Stamp; break;
    default: break;
    }
    editor::AnnotationDraft draft = draftFor(tool, page);
    switch (tool) {
    case AnnotationTool::Note:
    case AnnotationTool::Stamp:
    case AnnotationTool::Rectangle:
    case AnnotationTool::Ellipse:
        draft.rect = intent.rect;
        break;
    case AnnotationTool::Line:
    case AnnotationTool::Arrow:
        draft.lineStart = intent.a;
        draft.lineEnd = intent.b;
        break;
    case AnnotationTool::Ink: {
        const double zoom = intent.zoom > 0.0 ? intent.zoom : 1.0;
        for (const std::vector<core::Point>& raw : intent.strokes) {
            std::vector<core::Point> reduced = editor::geometry::reduceStroke(raw, 0.5 / zoom);
            if (reduced.size() > pdf::kMaxInkPointsPerStroke) {
                // Thin evenly, keeping both ends.
                std::vector<core::Point> thinned;
                const std::size_t keep = pdf::kMaxInkPointsPerStroke;
                thinned.reserve(keep);
                for (std::size_t i = 0; i < keep; ++i) {
                    thinned.push_back(reduced[i * (reduced.size() - 1) / (keep - 1)]);
                }
                reduced = std::move(thinned);
            }
            if (reduced.size() >= 2) draft.strokes.push_back(std::move(reduced));
        }
        if (draft.strokes.empty()) return;
        break;
    }
    default:
        return;
    }

    std::vector<editor::AnnotationDraft> drafts;
    drafts.push_back(std::move(draft));
    const std::optional<std::vector<core::AnnotationId>> ids =
        run(editor::createAnnotations(session, std::move(drafts)));
    if (!ids.has_value() || ids->empty()) return;
    select(ids->front());
    if (tool == AnnotationTool::Note) openNoteEditor(ids->front());
}

void AnnotationController::applyIntent(const Intent& intent) {
    DocumentTab* tab = activeTab();
    switch (intent.kind) {
    case Intent::Kind::None:
    case Intent::Kind::PassThrough:
        return;
    case Intent::Kind::Select:
        select(intent.id);
        return;
    case Intent::Kind::ClearSelection:
        select(std::nullopt);
        return;
    case Intent::Kind::CreateNote:
    case Intent::Kind::CreateInk:
    case Intent::Kind::CreateShape:
    case Intent::Kind::CreateStamp:
        create(intent);
        return;
    case Intent::Kind::Move:
        if (tab != nullptr) run(editor::moveAnnotation(*tab->session(), intent.id, intent.delta));
        return;
    case Intent::Kind::Resize:
        if (tab != nullptr) run(editor::resizeAnnotation(*tab->session(), intent.id, intent.rect));
        return;
    case Intent::Kind::LineEndpoints:
        if (tab != nullptr) run(editor::setLineEndpoints(*tab->session(), intent.id, intent.a, intent.b));
        return;
    case Intent::Kind::Delete:
        if (tab == nullptr) return;
        if (run(editor::deleteAnnotations(*tab->session(), {intent.id})).has_value()) select(std::nullopt);
        return;
    case Intent::Kind::OpenNoteEditor:
        openNoteEditor(intent.id);
        return;
    case Intent::Kind::ConvertTextSelection:
        convertTextSelection(intent.tool);
        return;
    case Intent::Kind::SelectTool:
        // The machine already switched to Select; the toolbar follows.
        notifyState();
        repaint();
        return;
    }
}

void AnnotationController::deleteSelected() {
    DocumentTab* tab = activeTab();
    const std::optional<Resolved> selected = resolveSelected();
    if (tab == nullptr || !selected.has_value()) return;
    if (run(editor::deleteAnnotations(*tab->session(), {selected->view.id})).has_value()) select(std::nullopt);
}

void AnnotationController::editSelectedNote() {
    if (const std::optional<core::AnnotationId> id = selectedId(); id.has_value()) openNoteEditor(*id);
}

// Markup from the text selection: every page's rectangles become one draft
// (one quad per rectangle); all drafts go through ONE createAnnotations call
// so the whole markup is one undo step. A page whose text is not extracted
// yet is requested and nothing at all is created (no partial markup).
void AnnotationController::convertTextSelection(AnnotationTool tool) {
    if (!isMarkupTool(tool)) return;
    DocumentTab* tab = activeTab();
    if (tab == nullptr || !textSelectionNonEmpty()) return;
    editor::DocumentSession& session = *tab->session();
    const std::vector<editor::TextRange> ranges = editor::orderedSelectionRanges(
        tab->selection().selection(), [&session](core::PageId id) { return session.pageIndexFor(id); },
        [&session](std::size_t index) { return session.pageId(index); }, editor::DocumentSession::kInvalidPage);

    std::vector<editor::AnnotationDraft> drafts;
    bool missing = false;
    for (const editor::TextRange& range : ranges) {
        const std::shared_ptr<const pdf::PdfTextPage> text = session.textService().cachedTextPage(range.page);
        if (text == nullptr) {
            session.textService().ensureTextPage(range.page);
            missing = true;
            continue;
        }
        const std::uint32_t end = static_cast<std::uint32_t>(std::min<std::size_t>(range.end, text->charCount()));
        if (range.begin >= end) continue;
        std::vector<editor::DisplayQuad> quads;
        for (const core::Rect& rect : text->rectsForRange(range.begin, end - range.begin)) {
            quads.push_back(quadOf(rect));
        }
        if (quads.empty()) continue;
        if (drafts.empty() || drafts.back().page != range.page) drafts.push_back(draftFor(tool, range.page));
        for (editor::DisplayQuad& quad : quads) drafts.back().quads.push_back(quad);
    }
    if (missing) {
        context_.setStatus("Annotate: Text not ready yet");
        return;
    }
    if (drafts.empty()) {
        context_.setStatus("Annotate: Nothing to mark up");
        return;
    }
    if (run(editor::createAnnotations(session, std::move(drafts))).has_value()) {
        tab->selection().clear();
        repaint();
    }
}

// --- Note editor ---------------------------------------------------------------------------

void AnnotationController::openNoteEditor(core::AnnotationId id) {
    closeNoteEditor();
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    const editor::DocumentSession& session = *tab->session();
    const std::optional<editor::AnnotationService::Located> located = session.annotations().locate(id);
    if (!located.has_value()) return;
    const std::optional<editor::AnnotationView> view = session.annotations().find(located->page, id);
    if (!view.has_value() || (view->caps & editor::kCapEditContents) == 0) return;
    editing_ = Editing{tab->id(), id, view->contents};
    noteArea_->setText(view->contents);
    placeNotePanel();
    context_.setFocus(noteArea_);
    repaint();
}

void AnnotationController::hideNotePanel() {
    const bool focused = noteArea_->isFocused();
    notePanel_->setFrame(kHiddenFrame);
    if (focused) context_.setFocus(nullptr);
}

void AnnotationController::placeNotePanel() {
    if (!editing_.has_value()) {
        notePanel_->setFrame(kHiddenFrame);
        return;
    }
    DocumentTab* tab = activeTab();
    if (tab == nullptr || tab->id() != editing_->tab) {
        notePanel_->setFrame(kHiddenFrame);
        return;
    }
    const editor::DocumentSession& session = *tab->session();
    const std::optional<editor::AnnotationService::Located> located = session.annotations().locate(editing_->id);
    const std::size_t pageIndex = located.has_value() ? session.pageIndexFor(located->page)
                                                      : editor::DocumentSession::kInvalidPage;
    const std::optional<editor::AnnotationView> view =
        located.has_value() ? session.annotations().find(located->page, editing_->id) : std::nullopt;
    const std::optional<core::Rect> pageRect =
        pageIndex != editor::DocumentSession::kInvalidPage ? context_.viewport.pageRectInViewport(pageIndex)
                                                           : std::nullopt;
    core::Rect anchor{viewportFrame_.origin.x + 8.0, viewportFrame_.origin.y + 8.0, 0.0, 0.0};
    if (view.has_value() && pageRect.has_value()) {
        const core::Rect local = AnnotationPainter::toViewport(*pageRect, context_.viewport.zoomFactor(), view->bounds);
        anchor = core::Rect{viewportFrame_.origin.x + local.origin.x, viewportFrame_.origin.y + local.origin.y,
                            local.size.width, local.size.height};
    }
    double x = anchor.maxX() + 8.0;
    if (x + kNotePanelWidth > viewportFrame_.maxX()) x = anchor.minX() - 8.0 - kNotePanelWidth;
    x = std::clamp(x, viewportFrame_.minX(), std::max(viewportFrame_.minX(), viewportFrame_.maxX() - kNotePanelWidth));
    const double y = std::clamp(anchor.minY(), viewportFrame_.minY(),
                                std::max(viewportFrame_.minY(), viewportFrame_.maxY() - kNotePanelHeight));
    notePanel_->setFrame(core::Rect{x, y, kNotePanelWidth, kNotePanelHeight});
}

void AnnotationController::closeNoteEditor() {
    if (!editing_.has_value()) return;
    const Editing editing = std::move(*editing_);
    editing_.reset();
    std::string text = noteArea_->text();
    hideNotePanel();
    if (text == editing.original) return;
    // Against the tab the editor was opened on (it may no longer be active).
    DocumentTab* tab = context_.workspace.tabById(editing.tab);
    if (tab == nullptr || tab->session() == nullptr) return;
    if (lockedForEditing(*tab)) {
        context_.setStatus(kLockedMessage);
        return;
    }
    core::Result<editor::AnnotationEdit> edit = editor::editContents(*tab->session(), editing.id, std::move(text));
    if (!edit.has_value()) {
        context_.setStatus(std::format("Annotate: {}", core::describe(edit.error())));
        return;
    }
    const core::Status status = tab->session()->execute(std::move(edit->command));
    if (!status.has_value()) context_.setStatus(std::format("Annotate: {}", core::describe(status.error())));
    repaint();
}

// --- Menus -----------------------------------------------------------------------------------

bool AnnotationController::canPerform(AnnotationCommand command) const {
    const DocumentTab* tab = activeTab();
    if (tab == nullptr) return false;
    switch (command) {
    case AnnotationCommand::DeleteAnnotation: {
        const std::optional<Resolved> selected = resolveSelected();
        return selected.has_value() && (selected->view.caps & editor::kCapDelete) != 0 && !lockedForEditing(*tab);
    }
    case AnnotationCommand::EditNote: {
        const std::optional<Resolved> selected = resolveSelected();
        return selected.has_value() && (selected->view.caps & editor::kCapEditContents) != 0 &&
               !lockedForEditing(*tab);
    }
    default:
        return static_cast<std::size_t>(command) < kAnnotationToolCount;
    }
}

void AnnotationController::perform(AnnotationCommand command) {
    if (activeTab() == nullptr) {
        context_.setStatus("Open a document to annotate");
        return;
    }
    switch (command) {
    case AnnotationCommand::DeleteAnnotation:
        deleteSelected();
        return;
    case AnnotationCommand::EditNote:
        editSelectedNote();
        return;
    default:
        if (static_cast<std::size_t>(command) < kAnnotationToolCount) {
            setTool(static_cast<AnnotationTool>(command));
        }
        return;
    }
}

} // namespace rivet::app
