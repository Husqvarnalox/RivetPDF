// SPDX-License-Identifier: MPL-2.0
#include "app/ContentController.hpp"

#include "core/Error.hpp"
#include "editor/DocumentSession.hpp"
#include "platform/ImageDecoder.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextArea.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>

namespace rivet::app {

namespace {

using Intent = ContentInteraction::Intent;
using HitInfo = ContentInteraction::HitInfo;
using KeyInput = ContentInteraction::KeyInput;

// The session's editing lock is held while a save is in flight.
constexpr const char* kLockedMessage = "A save is in progress — editing is paused";
constexpr double kEditorPadding = 2.0;     // logical points inside the editor frame
constexpr double kMinNewBoxWidth = 40.0;   // page points
constexpr double kPageMargin = 4.0;        // page points kept free right of a click-placed box
constexpr double kLineAdvanceFactor = 1.2; // new text: baseline step / font size
constexpr double kBaselineFactor = 0.9;    // new text: baseline offset below the click / font size

double steadySeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool atLeast(editor::ContentCapability capability, editor::ContentCapability minimum) {
    return static_cast<std::uint8_t>(capability) >= static_cast<std::uint8_t>(minimum);
}

ui::Color uiColor(const pdf::PdfColor& color) { return ui::Color::rgba(static_cast<double>(color.r), static_cast<double>(color.g), static_cast<double>(color.b), 1.0); }

ContentController::FontFamily familyOf(const pdf::PdfFontInfo& font) {
    if (font.monospace) return ContentController::FontFamily::Mono;
    if (font.serif) return ContentController::FontFamily::Serif;
    return ContentController::FontFamily::Sans;
}

core::Point nudgeDelta(KeyInput key, double step) {
    switch (key) {
    case KeyInput::Left: return {-step, 0.0};
    case KeyInput::Right: return {step, 0.0};
    case KeyInput::Up: return {0.0, -step};
    case KeyInput::Down: return {0.0, step};
    default: return {};
    }
}

// Up to three code points as "U+XXXX" (document text never goes to the UI log).
std::string describeCodepoints(const std::u32string& codepoints) {
    std::string out;
    std::size_t shown = 0;
    for (const char32_t cp : codepoints) {
        if (shown == 3) {
            out += " …";
            break;
        }
        if (shown > 0) out += ", ";
        out += std::format("U+{:04X}", static_cast<std::uint32_t>(cp));
        ++shown;
    }
    return out;
}

bool blankText(const std::string& text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; });
}

// Reads and decodes an image file. Runs on a worker thread.
core::Result<std::shared_ptr<const pdf::PdfImageData>> loadImage(const platform::IImageDecoder& decoder,
                                                                 const std::filesystem::path& path) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return std::unexpected(core::makeError(core::ErrorCode::Io, "The image file could not be read", "app"));
    if (size > pdf::kMaxImageEncodedBytes) {
        return std::unexpected(
            core::makeError(core::ErrorCode::InvalidArgument, "The image file is larger than 64 MiB", "app"));
    }
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!stream || (size > 0 && !stream.read(reinterpret_cast<char*>(bytes.data()),
                                              static_cast<std::streamsize>(size)))) {
        return std::unexpected(core::makeError(core::ErrorCode::Io, "The image file could not be read", "app"));
    }
    core::Result<pdf::PdfImageData> decoded = decoder.decode(bytes);
    if (!decoded.has_value()) return std::unexpected(decoded.error());
    return std::make_shared<const pdf::PdfImageData>(std::move(*decoded));
}

} // namespace

ContentController::ContentController(ShellContext& context, ui::Widget& parent, AnnotationController& annotations,
                                      core::TaskScheduler& scheduler, std::unique_ptr<ContentBackend> backend,
                                      Clock clock)
    : context_(context), annotations_(annotations), scheduler_(scheduler), backend_(std::move(backend)),
      clock_(clock ? std::move(clock) : Clock(steadySeconds)), layer_(*this),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
    interaction_.setHitTest([this](std::size_t page, core::Point point,
                                   double tolerance) -> std::optional<HitInfo> {
        DocumentTab* tab = activeTab();
        if (tab == nullptr || page >= tab->session()->pageCount()) return std::nullopt;
        editor::DocumentSession& session = *tab->session();
        const core::PageId pageId = session.pageId(page);
        const std::optional<editor::ContentHit> hit = backend_->hitTest(session, pageId, point, tolerance);
        if (!hit.has_value()) return std::nullopt;
        return infoFor(*tab, pageId, hit->id, hit->isBlock);
    });

    auto area = std::make_unique<ui::TextArea>();
    editorArea_ = area.get();
    editorArea_->setBackground(ui::Color::white());
    editorArea_->setOnFocusRequested([this] { context_.setFocus(editorArea_); });
    editorArea_->setOnCommit([this] { commitEditor(); });
    editorArea_->setOnEscape([this] { cancelEditor(); });
    editorArea_->setFrame(kHiddenFrame);
    parent.addChild(std::move(area));

    // One tool state: choosing any annotation tool ends the content tool.
    annotations_.setOnToolActivated([this] {
        if (switching_ || interaction_.tool() == ContentTool::None) return;
        if (!commitEditor()) cancelEditor();
        setTool(ContentTool::None);
    });

    context_.viewport.addLayer(&layer_);
    pendingEditsToken_ = context_.pendingEdits.add([this] { return commitPendingEdits(); });
}

ContentController::~ContentController() {
    // Drop undelivered image completions and wait for the workers.
    *alive_ = false;
    scope_.closeAndWait();
    context_.pendingEdits.remove(pendingEditsToken_);
    annotations_.setOnToolActivated(nullptr);
    annotations_.setSuspended(false);
    context_.viewport.removeLayer(&layer_);
    // The workspace (and every session) outlives the controllers: drop the
    // observers so no callback fires into a dead controller.
    for (const TabId tabId : observedTabs_) {
        if (DocumentTab* tab = context_.workspace.tabById(tabId); tab != nullptr && tab->session() != nullptr) {
            backend_->observe(*tab->session(), nullptr);
        }
    }
}

void ContentController::bindTab(DocumentTab* tab) {
    // Commit against the tab the editor was opened on (a refused commit is
    // dropped: the tab is being left), then reset the tool state.
    if (editing_.has_value() && !commitEditor()) cancelEditor();
    selected_.clear();
    nudge_.valid = false;
    overflowCheck_.reset();
    std::erase_if(observedTabs_, [this](TabId id) { return context_.workspace.tabById(id) == nullptr; });
    if (tab != nullptr && tab->state() == DocumentTab::State::Ready &&
        std::find(observedTabs_.begin(), observedTabs_.end(), tab->id()) == observedTabs_.end()) {
        observedTabs_.push_back(tab->id());
        backend_->observe(*tab->session(), [this](core::PageId) {
            repaint();
            checkOverflow();
            notifyState();
        });
    }
    setTool(ContentTool::None);
}

void ContentController::layout(const core::Rect& viewportFrame) { viewportFrame_ = viewportFrame; }

// --- Tool state ---------------------------------------------------------------------------

void ContentController::setTool(ContentTool next) {
    if (switching_) return;
    if (editing_.has_value() && !commitEditor()) return; // refused: stay where the text is
    switching_ = true;
    interaction_.setTool(next); // cancels a running gesture, clears hover
    if (DocumentTab* tab = activeTab(); tab != nullptr) selected_.erase(tab->id());
    interaction_.setSelection(std::nullopt);
    nudge_.valid = false;
    overflowCheck_.reset();
    if (next != ContentTool::None) {
        annotations_.setTool(AnnotationTool::Select);
        annotations_.clearSelection();
        annotations_.setSuspended(true);
    } else {
        annotations_.setSuspended(false);
    }
    switching_ = false;
    notifyState();
    repaint();
}

void ContentController::notifyState() {
    if (onStateChanged_) onStateChanged_();
}

void ContentController::repaint() { context_.viewport.invalidate(); }

bool ContentController::lockedForEditing(const DocumentTab& tab) const {
    return tab.session() != nullptr && tab.session()->isEditingLocked();
}

bool ContentController::handleEscape() {
    if (editing_.has_value()) {
        cancelEditor();
        return true;
    }
    if (!active()) return false;
    if (interaction_.gestureInProgress()) {
        interaction_.cancelGesture();
        repaint();
        return true;
    }
    if (currentSelection().has_value()) {
        select(std::nullopt);
        return true;
    }
    return false;
}

// --- Running commands ---------------------------------------------------------------------

std::optional<std::vector<core::ObjectId>> ContentController::runOn(DocumentTab& tab,
                                                                    core::Result<editor::ContentEdit> edit) {
    if (tab.session() == nullptr) return std::nullopt;
    if (lockedForEditing(tab)) {
        context_.setStatus(kLockedMessage);
        return std::nullopt;
    }
    if (!edit.has_value()) {
        context_.setStatus(std::format("Edit: {}", core::describe(edit.error())));
        return std::nullopt;
    }
    const core::Status status = tab.session()->execute(std::move(edit->command));
    if (!status.has_value()) {
        context_.setStatus(std::format("Edit: {}", core::describe(status.error())));
        return std::nullopt;
    }
    repaint();
    return std::move(edit->ids);
}

std::optional<std::vector<core::ObjectId>> ContentController::run(core::Result<editor::ContentEdit> edit) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return std::nullopt;
    return runOn(*tab, std::move(edit));
}

// --- Selection ------------------------------------------------------------------------------

std::optional<HitInfo> ContentController::infoFor(DocumentTab& tab, core::PageId page, core::ObjectId id,
                                                  bool isBlock) const {
    editor::DocumentSession& session = *tab.session();
    HitInfo info;
    info.id = id;
    info.isBlock = isBlock;
    if (isBlock) {
        const std::optional<editor::TextBlockView> block = backend_->findBlock(session, page, id);
        if (!block.has_value()) return std::nullopt;
        info.kind = ContentInteraction::Kind::Text;
        info.bounds = block->bounds;
        info.quad = block->quad;
        info.capability = block->capability;
        info.reason = block->capabilityReason;
        info.canMove = atLeast(block->capability, editor::ContentCapability::MoveOnly);
        info.canDelete = info.canMove;
        info.canEditText = atLeast(block->capability, editor::ContentCapability::Replaceable);
        info.canWrap = info.canEditText;
        info.rivetBlock = block->tag != 0;
        info.wrapBase = block->wrapWidth > 0.0
                            ? block->wrapWidth
                            : std::hypot(block->quad[1].x - block->quad[0].x, block->quad[1].y - block->quad[0].y);
        return info;
    }
    const std::optional<editor::ContentObjectView> object = backend_->findObject(session, page, id);
    if (!object.has_value()) return std::nullopt;
    switch (object->type) {
    case pdf::PdfContentObjectType::Image: info.kind = ContentInteraction::Kind::Image; break;
    case pdf::PdfContentObjectType::Path: info.kind = ContentInteraction::Kind::Path; break;
    default: info.kind = ContentInteraction::Kind::Other; break;
    }
    info.bounds = object->bounds;
    info.quad = object->quad;
    info.capability = object->capability;
    info.reason = object->capabilityReason;
    info.canMove = atLeast(object->capability, editor::ContentCapability::MoveOnly);
    info.canDelete = info.canMove;
    info.canResize = info.canMove && (info.kind == ContentInteraction::Kind::Image ||
                                      info.kind == ContentInteraction::Kind::Path);
    return info;
}

std::optional<ContentController::Resolved> ContentController::resolveSelected() const {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return std::nullopt;
    const auto it = selected_.find(tab->id());
    if (it == selected_.end()) return std::nullopt;
    editor::DocumentSession& session = *tab->session();
    const std::size_t index = session.pageIndexFor(it->second.page);
    if (index != editor::DocumentSession::kInvalidPage) {
        const editor::PageContentViewPtr content = backend_->content(session, it->second.page);
        if (content == nullptr || !content->loaded) return std::nullopt; // still loading: keep the selection
        if (std::optional<HitInfo> info = infoFor(*tab, it->second.page, it->second.id, it->second.isBlock);
            info.has_value()) {
            return Resolved{it->second.page, index, std::move(*info)};
        }
    }
    selected_.erase(it); // no longer resolves (undone, deleted, page gone)
    return std::nullopt;
}

std::optional<ContentInteraction::Selected> ContentController::currentSelection() const {
    const std::optional<Resolved> resolved = resolveSelected();
    if (!resolved.has_value()) return std::nullopt;
    return ContentInteraction::Selected{resolved->pageIndex, resolved->info};
}

void ContentController::select(std::optional<ContentInteraction::Selected> selection) {
    DocumentTab* tab = activeTab();
    nudge_.valid = false;
    if (tab == nullptr) {
        interaction_.setSelection(std::nullopt);
        return;
    }
    if (selection.has_value() && selection->page < tab->session()->pageCount()) {
        selected_[tab->id()] =
            StoredSelection{tab->session()->pageId(selection->page), selection->info.id, selection->info.isBlock};
        interaction_.setSelection(selection);
        announceSelection(selection->info);
    } else {
        selected_.erase(tab->id());
        interaction_.setSelection(std::nullopt);
    }
    notifyState();
    repaint();
}

void ContentController::selectStored(DocumentTab& tab, core::PageId page, core::ObjectId id, bool isBlock) {
    selected_[tab.id()] = StoredSelection{page, id, isBlock};
    interaction_.setSelection(currentSelection());
    notifyState();
    repaint();
}

void ContentController::announceSelection(const HitInfo& info) {
    const std::string suffix = info.reason.empty() ? std::string() : std::format(" — {}", info.reason);
    switch (info.capability) {
    case editor::ContentCapability::ReadOnly:
        context_.setStatus(std::format("Edit: read-only{}", suffix));
        break;
    case editor::ContentCapability::MoveOnly:
        context_.setStatus(std::format("Edit: can be moved or deleted{}", suffix));
        break;
    case editor::ContentCapability::Replaceable:
        context_.setStatus(std::format("Edit: text can be retyped; a bundled font will be used{}", suffix));
        break;
    case editor::ContentCapability::FullyEditable:
        if (info.kind == ContentInteraction::Kind::Text) {
            context_.setStatus("Edit: double-click or press Return to edit the text");
        }
        break;
    }
}

void ContentController::applyIntent(const Intent& intent, bool shift) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr) return;
    editor::DocumentSession& session = *tab->session();
    const bool pageValid = intent.page < session.pageCount();
    const core::PageId page = pageValid ? session.pageId(intent.page) : core::PageId{};
    switch (intent.kind) {
    case Intent::Kind::None:
    case Intent::Kind::PassThrough:
        return;
    case Intent::Kind::Select:
        select(ContentInteraction::Selected{intent.page, intent.info});
        return;
    case Intent::Kind::ClearSelection:
        select(std::nullopt);
        return;
    case Intent::Kind::Move:
        if (!pageValid) return;
        nudge_.valid = false;
        run(backend_->moveContent(session, page, {intent.info.id}, intent.delta));
        return;
    case Intent::Kind::Resize:
        if (!pageValid) return;
        nudge_.valid = false;
        run(backend_->resizeContent(session, page, intent.info.id, intent.rect));
        return;
    case Intent::Kind::SetWrap: {
        if (!pageValid) return;
        editor::TextBlockPatch patch;
        patch.wrapWidth = intent.width;
        patchSelectedBlock(std::move(patch));
        return;
    }
    case Intent::Kind::Delete:
        deleteInfo(ContentInteraction::Selected{intent.page, intent.info});
        return;
    case Intent::Kind::OpenEditor:
        select(ContentInteraction::Selected{intent.page, intent.info});
        openEditorOnBlock(ContentInteraction::Selected{intent.page, intent.info});
        return;
    case Intent::Kind::AddTextClick:
        select(std::nullopt);
        openEditorForNew(intent.page, intent.point, 0.0);
        return;
    case Intent::Kind::AddTextBox:
        select(std::nullopt);
        openEditorForNew(intent.page, intent.rect.origin, intent.rect.size.width);
        return;
    case Intent::Kind::Nudge:
        nudge(intent, shift);
        return;
    }
}

void ContentController::nudge(const Intent& intent, bool shift) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || intent.page >= tab->session()->pageCount()) return;
    editor::DocumentSession& session = *tab->session();
    if (lockedForEditing(*tab)) {
        context_.setStatus(kLockedMessage);
        return;
    }
    const core::PageId page = session.pageId(intent.page);
    const core::ObjectId id = intent.info.id;
    const core::Point step = nudgeDelta(intent.key, shift ? ContentInteraction::kNudgeStepShift
                                                          : ContentInteraction::kNudgeStep);
    const double now = clock_();

    // A burst of presses of one key on one object is ONE undo step: the
    // previous nudge command (still the top of the history) is undone and
    // one combined move replaces it.
    core::Point total = step;
    bool coalesced = false;
    if (nudge_.valid && nudge_.tab == tab->id() && nudge_.id == id && nudge_.key == intent.key &&
        now - nudge_.time <= kNudgeCoalesceSeconds && session.commands().stateId() == nudge_.stateAfter) {
        if (session.undo()) {
            total = core::Point{nudge_.total.x + step.x, nudge_.total.y + step.y};
            coalesced = true;
        }
    }
    const std::optional<std::vector<core::ObjectId>> ids = run(backend_->moveContent(session, page, {id}, total));
    if (!ids.has_value()) {
        if (coalesced) session.redo(); // keep the previous nudge
        nudge_.valid = false;
        return;
    }
    nudge_.valid = true;
    nudge_.tab = tab->id();
    nudge_.id = id;
    nudge_.key = intent.key;
    nudge_.total = total;
    nudge_.time = now;
    nudge_.stateAfter = session.commands().stateId();
}

void ContentController::deleteInfo(const ContentInteraction::Selected& selection) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || selection.page >= tab->session()->pageCount()) return;
    if (!selection.info.canDelete) {
        context_.setStatus(std::format("Edit: read-only{}", selection.info.reason.empty()
                                                                ? std::string()
                                                                : std::format(" — {}", selection.info.reason)));
        return;
    }
    editor::DocumentSession& session = *tab->session();
    nudge_.valid = false;
    if (run(backend_->deleteContent(session, session.pageId(selection.page), {selection.info.id})).has_value()) {
        select(std::nullopt);
    }
}

void ContentController::deleteSelected() {
    if (const std::optional<ContentInteraction::Selected> selection = currentSelection(); selection.has_value()) {
        deleteInfo(*selection);
    }
}

void ContentController::editSelectedText() {
    const std::optional<ContentInteraction::Selected> selection = currentSelection();
    if (!selection.has_value()) return;
    if (!selection->info.canEditText) {
        context_.setStatus(selection->info.reason.empty() ? std::string("Edit: this text cannot be retyped")
                                                          : std::format("Edit: {}", selection->info.reason));
        return;
    }
    openEditorOnBlock(*selection);
}

void ContentController::bringSelectedToFront() {
    DocumentTab* tab = activeTab();
    const std::optional<ContentInteraction::Selected> selection = currentSelection();
    if (tab == nullptr || !selection.has_value() || !selection->info.rivetBlock) return;
    editor::DocumentSession& session = *tab->session();
    nudge_.valid = false;
    run(backend_->bringToFront(session, session.pageId(selection->page), selection->info.id));
}

// --- Text style ---------------------------------------------------------------------------

pdf::PdfBundledFont ContentController::bundledFor(FontFamily family, bool bold) {
    switch (family) {
    case FontFamily::Serif: return bold ? pdf::PdfBundledFont::SerifBold : pdf::PdfBundledFont::SerifRegular;
    case FontFamily::Mono: return bold ? pdf::PdfBundledFont::MonoBold : pdf::PdfBundledFont::MonoRegular;
    case FontFamily::Sans: break;
    }
    return bold ? pdf::PdfBundledFont::SansBold : pdf::PdfBundledFont::SansRegular;
}

pdf::PdfBundledFont ContentController::bundledFor(const TextStyle& style) const {
    return bundledFor(style.family, style.bold);
}

ContentController::TextStyle ContentController::displayedStyle() const {
    if (const std::optional<Resolved> resolved = resolveSelected();
        resolved.has_value() && resolved->info.kind == ContentInteraction::Kind::Text) {
        DocumentTab* tab = activeTab();
        if (const std::optional<editor::TextBlockView> block =
                backend_->findBlock(*tab->session(), resolved->page, resolved->info.id);
            block.has_value()) {
            TextStyle style;
            style.family = familyOf(block->font);
            style.bold = block->font.bold;
            style.size = block->fontSize;
            style.color = block->color;
            return style;
        }
    }
    return newStyle_;
}

void ContentController::restyle(const std::function<void(TextStyle&)>& mutate, StyleField field) {
    if (editing_.has_value()) {
        if (editing_->isNew) {
            mutate(newStyle_);
            newStyle_.size = std::clamp(newStyle_.size, kMinFontSize, kMaxFontSize);
            editing_->fontSize = newStyle_.size;
            editing_->lineAdvance = newStyle_.size * kLineAdvanceFactor;
            editing_->color = newStyle_.color;
            editing_->bundled = bundledFor(newStyle_);
            editorArea_->setTextColor(uiColor(newStyle_.color));
            repaint();
            notifyState();
            return;
        }
        if (!commitEditor()) return; // refused: the text stays in the editor
    }
    if (const std::optional<Resolved> resolved = resolveSelected();
        resolved.has_value() && resolved->info.kind == ContentInteraction::Kind::Text) {
        if (!resolved->info.canEditText) {
            context_.setStatus(resolved->info.reason.empty() ? std::string("Edit: this text cannot be restyled")
                                                             : std::format("Edit: {}", resolved->info.reason));
            return;
        }
        TextStyle style = displayedStyle();
        mutate(style);
        editor::TextBlockPatch patch;
        switch (field) {
        case StyleField::Font:
            if (!resolved->info.rivetBlock) {
                context_.setStatus("Edit: the font of existing text cannot be changed");
                return;
            }
            patch.font = bundledFor(style);
            break;
        case StyleField::Size: patch.fontSize = std::clamp(style.size, kMinFontSize, kMaxFontSize); break;
        case StyleField::Color: patch.color = style.color; break;
        }
        patchSelectedBlock(std::move(patch));
        return;
    }
    if (currentSelection().has_value()) return; // an image / path: no text style
    mutate(newStyle_);
    newStyle_.size = std::clamp(newStyle_.size, kMinFontSize, kMaxFontSize);
    notifyState();
}

void ContentController::patchSelectedBlock(editor::TextBlockPatch patch) {
    DocumentTab* tab = activeTab();
    const std::optional<Resolved> resolved = resolveSelected();
    if (tab == nullptr || !resolved.has_value() || !resolved->info.isBlock) return;
    const bool sizeChange = patch.fontSize.has_value() || patch.font.has_value();
    const double originalMaxY = resolved->info.bounds.maxY();
    nudge_.valid = false;
    const std::optional<std::vector<core::ObjectId>> ids = runOn(
        *tab, backend_->editTextBlock(*tab->session(), resolved->page, resolved->info.id, std::move(patch)));
    if (!ids.has_value()) return;
    const core::ObjectId block = ids->empty() ? resolved->info.id : ids->front();
    selectStored(*tab, resolved->page, block, true);
    if (sizeChange) {
        overflowCheck_ = OverflowCheck{tab->id(), resolved->page, block, originalMaxY};
        checkOverflow();
    }
}

void ContentController::setFontFamily(FontFamily family) {
    restyle([family](TextStyle& style) { style.family = family; }, StyleField::Font);
}

void ContentController::setBold(bool bold) {
    restyle([bold](TextStyle& style) { style.bold = bold; }, StyleField::Font);
}

void ContentController::setFontSize(double size) {
    restyle([size](TextStyle& style) { style.size = size; }, StyleField::Size);
}

void ContentController::setTextColor(const pdf::PdfColor& color) {
    restyle([color](TextStyle& style) { style.color = color; }, StyleField::Color);
}

std::pair<std::uint32_t, std::uint32_t> ContentController::selectedImagePixels() const {
    const std::optional<Resolved> resolved = resolveSelected();
    DocumentTab* tab = activeTab();
    if (tab == nullptr || !resolved.has_value() || resolved->info.kind != ContentInteraction::Kind::Image) {
        return {0, 0};
    }
    const std::optional<editor::ContentObjectView> object =
        backend_->findObject(*tab->session(), resolved->page, resolved->info.id);
    if (!object.has_value()) return {0, 0};
    return {object->pixelWidth, object->pixelHeight};
}

bool ContentController::selectionIsText() const {
    const std::optional<Resolved> resolved = resolveSelected();
    return resolved.has_value() && resolved->info.kind == ContentInteraction::Kind::Text;
}

bool ContentController::selectionIsImage() const {
    const std::optional<Resolved> resolved = resolveSelected();
    return resolved.has_value() && resolved->info.kind == ContentInteraction::Kind::Image;
}

bool ContentController::selectionIsRivetBlock() const {
    const std::optional<Resolved> resolved = resolveSelected();
    return resolved.has_value() && resolved->info.rivetBlock;
}

void ContentController::checkOverflow() {
    if (!overflowCheck_.has_value()) return;
    const OverflowCheck check = *overflowCheck_;
    DocumentTab* tab = context_.workspace.tabById(check.tab);
    if (tab == nullptr || tab->session() == nullptr ||
        tab->session()->pageIndexFor(check.page) == editor::DocumentSession::kInvalidPage) {
        overflowCheck_.reset();
        return;
    }
    const std::optional<editor::TextBlockView> block = backend_->findBlock(*tab->session(), check.page, check.block);
    if (!block.has_value() || !(block->edited || block->tag != 0)) return; // not resolved yet: wait
    overflowCheck_.reset();
    if (block->bounds.maxY() > check.originalMaxY + 0.5) {
        context_.setStatus("Edit: Text extends below the original block");
    }
}

// --- Inline text editor -----------------------------------------------------------------------

void ContentController::openEditorOnBlock(const ContentInteraction::Selected& selection) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || !selection.info.isBlock || selection.page >= tab->session()->pageCount()) return;
    if (editing_.has_value() && !commitEditor()) return;
    editor::DocumentSession& session = *tab->session();
    if (lockedForEditing(*tab)) {
        context_.setStatus(kLockedMessage);
        return;
    }
    const core::PageId page = session.pageId(selection.page);
    const std::optional<editor::TextBlockView> block = backend_->findBlock(session, page, selection.info.id);
    if (!block.has_value() || !atLeast(block->capability, editor::ContentCapability::Replaceable)) return;

    Editing editing;
    editing.tab = tab->id();
    editing.page = page;
    editing.pageIndex = selection.page;
    editing.block = block->id;
    editing.isNew = false;
    editing.original = block->text;
    editing.fontSize = block->fontSize > 0.0 ? block->fontSize : kDefaultFontSize;
    editing.lineAdvance = block->lineAdvance > 0.0 ? block->lineAdvance : editing.fontSize * kLineAdvanceFactor;
    editing.color = block->color;
    editing.wrapped = block->wrapWidth > 0.0;
    if (block->tag != 0 || block->fontSubstituted || block->capability == editor::ContentCapability::Replaceable) {
        editing.bundled = bundledFor(familyOf(block->font), block->font.bold);
    }
    const double width = editing.wrapped ? block->wrapWidth : block->bounds.size.width + editing.fontSize;
    editing.displayRect =
        core::Rect{block->bounds.origin.x, block->bounds.origin.y, width, block->bounds.size.height};
    editing.quad = block->quad;
    editing.originalMaxY = block->bounds.maxY();
    editing_ = std::move(editing);
    if (std::abs(block->rotationDegrees) > 0.5) {
        context_.setStatus("Edit: rotated text is edited unrotated and keeps its rotation on commit");
    }
    showEditor();
}

void ContentController::openEditorForNew(std::size_t pageIndex, core::Point topLeft, double boxWidth) {
    DocumentTab* tab = activeTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    if (editing_.has_value() && !commitEditor()) return;
    editor::DocumentSession& session = *tab->session();
    if (lockedForEditing(*tab)) {
        context_.setStatus(kLockedMessage);
        return;
    }
    const core::Size pageSize = session.pageSizePoints(pageIndex);

    Editing editing;
    editing.tab = tab->id();
    editing.page = session.pageId(pageIndex);
    editing.pageIndex = pageIndex;
    editing.isNew = true;
    editing.fontSize = newStyle_.size;
    editing.lineAdvance = newStyle_.size * kLineAdvanceFactor;
    editing.color = newStyle_.color;
    editing.bundled = bundledFor(newStyle_);
    const double width = boxWidth > 0.0 ? boxWidth
                                        : std::clamp(pageSize.width - topLeft.x - kPageMargin, kMinNewBoxWidth,
                                                     kDefaultBoxWidth);
    editing.requestedWrap = boxWidth > 0.0 ? boxWidth : 0.0;
    editing.displayRect = core::Rect{topLeft.x, topLeft.y, width, editing.lineAdvance};
    editing.origin = core::Point{topLeft.x, topLeft.y + kBaselineFactor * editing.fontSize};
    editing.quad = {core::Point{topLeft.x, topLeft.y + editing.lineAdvance},
                    core::Point{topLeft.x + width, topLeft.y + editing.lineAdvance},
                    core::Point{topLeft.x + width, topLeft.y},
                    core::Point{topLeft.x, topLeft.y}};
    editing_ = std::move(editing);
    showEditor();
}

void ContentController::showEditor() {
    editorArea_->setText(editing_->original);
    editorArea_->setTextColor(uiColor(editing_->color));
    editorArea_->setBackground(ui::Color::white());
    editorArea_->setMetrics(editing_->fontSize * context_.viewport.zoomFactor(),
                            editing_->lineAdvance * context_.viewport.zoomFactor(), kEditorPadding);
    placeEditor(context_.viewport);
    context_.setFocus(editorArea_);
    notifyState();
    repaint();
}

void ContentController::hideEditor() {
    const bool focused = editorArea_->isFocused();
    editing_.reset();
    editorArea_->setFrame(kHiddenFrame);
    editorArea_->setText({});
    if (focused) context_.setFocus(nullptr);
    notifyState();
    repaint();
}

void ContentController::cancelEditor() {
    if (editing_.has_value()) hideEditor();
}

void ContentController::applyEditorMetrics(double zoom) {
    editorArea_->setMetrics(editing_->fontSize * zoom, editing_->lineAdvance * zoom, kEditorPadding);
}

void ContentController::placeEditor(const ui::ViewportToolHost& host) {
    if (!editing_.has_value()) return;
    const std::optional<core::Rect> pageRect = host.pageRectInViewport(editing_->pageIndex);
    const double zoom = host.zoomFactor();
    if (!pageRect.has_value() || zoom <= 0.0) {
        editorArea_->setFrame(kHiddenFrame);
        return;
    }
    applyEditorMetrics(zoom);
    const double x = viewportFrame_.origin.x + pageRect->origin.x + editing_->displayRect.origin.x * zoom -
                     kEditorPadding;
    const double y = viewportFrame_.origin.y + pageRect->origin.y + editing_->displayRect.origin.y * zoom -
                     kEditorPadding;
    const double width = editing_->displayRect.size.width * zoom + 2.0 * kEditorPadding;
    const double minHeight = editing_->displayRect.size.height * zoom + 2.0 * kEditorPadding;
    // Wrapping depends on the width: set it, then grow the height to the lines.
    editorArea_->setFrame(core::Rect{x, y, width, minHeight});
    const double lines = static_cast<double>(std::max<std::size_t>(1, editorArea_->lineCount()));
    const double height = std::max(minHeight, lines * editing_->lineAdvance * zoom + 2.0 * kEditorPadding);
    const core::Rect frame{x, y, width, height};
    const core::Rect view = viewportFrame_;
    const bool visible = frame.maxX() > view.minX() && frame.minX() < view.maxX() && frame.maxY() > view.minY() &&
                         frame.minY() < view.maxY();
    editorArea_->setFrame(visible ? frame : kHiddenFrame);
}

std::optional<ContentLayerClient::EditingOutline> ContentController::editingOutline() const {
    if (!editing_.has_value()) return std::nullopt;
    return EditingOutline{editing_->pageIndex, editing_->quad};
}

void ContentController::pointerPressed() {
    if (!editing_.has_value()) return;
    if (!commitEditor() && editing_.has_value()) context_.setFocus(editorArea_);
}

bool ContentController::commitEditor() {
    if (!editing_.has_value()) return true;
    const Editing editing = *editing_;
    DocumentTab* tab = context_.workspace.tabById(editing.tab);
    if (tab == nullptr || tab->session() == nullptr || tab->state() != DocumentTab::State::Ready ||
        editing.page == core::PageId{} ||
        tab->session()->pageIndexFor(editing.page) == editor::DocumentSession::kInvalidPage) {
        hideEditor(); // the tab or page is gone: nothing to commit against
        return true;
    }
    editor::DocumentSession& session = *tab->session();
    const std::string text = editorArea_->text();

    if (editing.isNew ? blankText(text) : text == editing.original) {
        hideEditor(); // nothing new / nothing changed: no command
        return true;
    }
    if (!editing.isNew && blankText(text)) {
        hideEditor();
        context_.setStatus("Edit: empty text was not applied (use Delete to remove a block)");
        return true;
    }
    if (session.isEditingLocked()) {
        context_.setStatus(kLockedMessage);
        return false;
    }
    if (editing.bundled.has_value()) {
        const std::u32string uncovered = backend_->uncoveredCodepoints(*editing.bundled, text);
        if (!uncovered.empty()) {
            context_.setStatus(std::format("Edit: the bundled font cannot write {}", describeCodepoints(uncovered)));
            return false;
        }
    }

    // The editor soft-wrapped the text: record its width as the block's wrap
    // width so the page layout matches what was typed.
    const std::size_t hardLines = static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n')) + 1;
    const bool softWrapped = editorArea_->lineCount() > hardLines;

    std::optional<std::vector<core::ObjectId>> ids;
    if (editing.isNew) {
        editor::NewTextBlock block;
        block.text = text;
        block.font = editing.bundled.value_or(pdf::PdfBundledFont::SansRegular);
        block.fontSize = editing.fontSize;
        block.color = editing.color;
        block.displayOrigin = editing.origin;
        block.displayWrapWidth = editing.requestedWrap > 0.0
                                     ? editing.requestedWrap
                                     : (softWrapped ? editing.displayRect.size.width : 0.0);
        ids = runOn(*tab, backend_->addTextBlock(session, editing.page, std::move(block)));
    } else {
        editor::TextBlockPatch patch;
        patch.text = text;
        if (!editing.wrapped && softWrapped) patch.wrapWidth = editing.displayRect.size.width;
        ids = runOn(*tab, backend_->editTextBlock(session, editing.page, editing.block, std::move(patch)));
    }
    if (!ids.has_value()) return false; // the status says why; the editor keeps the text

    hideEditor();
    nudge_.valid = false;
    const core::ObjectId block = ids->empty() ? editing.block : ids->front();
    if (!(block == core::ObjectId{})) {
        selectStored(*tab, editing.page, block, true);
        if (!editing.isNew) {
            overflowCheck_ = OverflowCheck{tab->id(), editing.page, block, editing.originalMaxY};
            checkOverflow();
        }
    }
    return true;
}

// --- Replace Image --------------------------------------------------------------------------------

void ContentController::replaceSelectedImage() {
    DocumentTab* tab = activeTab();
    const std::optional<Resolved> resolved = resolveSelected();
    if (tab == nullptr || !resolved.has_value() || resolved->info.kind != ContentInteraction::Kind::Image ||
        !resolved->info.canMove) {
        context_.setStatus("Edit: select an image first");
        return;
    }
    if (lockedForEditing(*tab)) {
        context_.setStatus(kLockedMessage);
        return;
    }
    if (context_.services.fileDialog == nullptr || context_.services.imageDecoder == nullptr) {
        context_.setStatus("Edit: replacing images is not available on this platform");
        return;
    }
    const core::Result<std::filesystem::path> chosen = context_.services.fileDialog->openImage();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            context_.setStatus(std::format("Edit: {}", core::describe(chosen.error())));
        }
        return;
    }
    std::optional<core::AsyncScope::Token> entered = scope_.enter();
    if (!entered.has_value()) return;
    auto token = std::make_shared<core::AsyncScope::Token>(std::move(*entered));
    context_.setStatus("Loading image…");

    const TabId tabId = tab->id();
    const core::PageId page = resolved->page;
    const core::ObjectId id = resolved->info.id;
    const platform::IImageDecoder* decoder = context_.services.imageDecoder;
    core::IMainThreadDispatcher* dispatcher = context_.services.mainDispatcher;
    // The token is declared first: it is released last, after the decode.
    scheduler_.post([this, token, decoder, dispatcher, path = *chosen, tabId, page, id, alive = alive_] {
        auto result = std::make_shared<core::Result<std::shared_ptr<const pdf::PdfImageData>>>(
            loadImage(*decoder, path));
        if (dispatcher != nullptr) {
            dispatcher->post([this, tabId, page, id, result, alive] {
                if (!*alive) return;
                onImageDecoded(tabId, page, id, std::move(*result));
            });
        } else if (*alive) {
            onImageDecoded(tabId, page, id, std::move(*result));
        }
    });
}

void ContentController::onImageDecoded(TabId tabId, core::PageId page, core::ObjectId id,
                                       core::Result<std::shared_ptr<const pdf::PdfImageData>> image) {
    if (!image.has_value()) {
        context_.setStatus(std::format("Edit: {}", core::describe(image.error())));
        return;
    }
    DocumentTab* tab = context_.workspace.tabById(tabId);
    if (tab == nullptr || tab->session() == nullptr || tab->state() != DocumentTab::State::Ready) return;
    if (tab->session()->pageIndexFor(page) == editor::DocumentSession::kInvalidPage) return;
    nudge_.valid = false;
    if (runOn(*tab, backend_->replaceImage(*tab->session(), page, id, std::move(*image))).has_value()) {
        context_.setStatus("Image replaced");
        notifyState();
    }
}

// --- Menus --------------------------------------------------------------------------------------------

bool ContentController::canPerform(ContentCommand command) const {
    if (context_.readyActiveTab() == nullptr) return false;
    if (command == ContentCommand::ToolEdit || command == ContentCommand::ToolAddText) return true;
    const std::optional<Resolved> resolved = resolveSelected();
    if (!resolved.has_value()) return false;
    switch (command) {
    case ContentCommand::EditText: return resolved->info.canEditText;
    case ContentCommand::ReplaceImage: return resolved->info.kind == ContentInteraction::Kind::Image && resolved->info.canMove;
    case ContentCommand::BringToFront: return resolved->info.rivetBlock;
    case ContentCommand::DeleteObject: return resolved->info.canDelete;
    default: return false;
    }
}

void ContentController::perform(ContentCommand command) {
    if (context_.readyActiveTab() == nullptr) {
        context_.setStatus("Open a document to edit");
        return;
    }
    switch (command) {
    case ContentCommand::ToolEdit: toggleTool(ContentTool::SelectObject); break;
    case ContentCommand::ToolAddText: toggleTool(ContentTool::AddText); break;
    case ContentCommand::EditText: editSelectedText(); break;
    case ContentCommand::ReplaceImage: replaceSelectedImage(); break;
    case ContentCommand::BringToFront: bringSelectedToFront(); break;
    case ContentCommand::DeleteObject: deleteSelected(); break;
    }
}

} // namespace rivet::app
