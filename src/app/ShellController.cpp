// SPDX-License-Identifier: MPL-2.0
#include "app/ShellController.hpp"

#include "core/Error.hpp"
#include "ui/Button.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kTabStripHeight = 28.0;
constexpr double kToolbarHeight = 40.0;

} // namespace

std::unique_ptr<ShellController> ShellController::create(const platform::ShellServices& services) {
    return createWithEngine(services, pdf::createEngine());
}

std::unique_ptr<ShellController> ShellController::createWithEngine(const platform::ShellServices& services,
                                                                    std::unique_ptr<pdf::PdfEngine> engine) {
    auto controller = std::unique_ptr<ShellController>(new ShellController(services, std::move(engine)));
    controller->buildWidgets();
    return controller;
}

ShellController::ShellController(const platform::ShellServices& services, std::unique_ptr<pdf::PdfEngine> engine)
    : services_(services),
      scheduler_(0),
      engine_(std::move(engine)),
      workspace_(*engine_, scheduler_, services.mainDispatcher),
      printCoordinator_(workspace_, *engine_, scheduler_, services.mainDispatcher, services.printService,
                        [this](std::string text) { setStatus(std::move(text)); }) {}

ShellController::~ShellController() {
    // A print spool borrows a session's document: stop it before anything
    // is torn down.
    printCoordinator_.cancel();
    // The viewport must drop its render-source/layout/state pointers before
    // the sessions and the widget tree die.
    viewport_->clearDocument();
    markdownView_->bind(nullptr);
    // The controllers die before the widget tree: the viewport must not keep
    // a pointer to the text bridge past its owner.
    viewport_->setTextBridge(nullptr);
    if (services_.setWindowTitle) services_.setWindowTitle("Rivet");
}

// Child order below is the paint/hit-test order: tab strip, toolbar,
// sidebar, viewport, loading overlay, then the floating find bar and
// password prompt above the viewport, then the status bar.
void ShellController::buildWidgets() {
    root_ = std::make_unique<ShellRoot>();

    // The viewport exists before the controllers (they reference it through
    // the context) but joins the tree after the sidebar.
    auto viewport = std::make_unique<ui::PdfViewport>();
    viewport_ = viewport.get();
    context_ = std::make_unique<ShellContext>(ShellContext{
        workspace_,
        services_,
        *viewport_,
        [this](std::string text) { setStatus(std::move(text)); },
        [this](ui::Widget* widget) { setFocus(widget); },
        [this] { layoutShell(); },
    });

    auto tabStrip = std::make_unique<ui::TabStrip>();
    tabStrip_ = tabStrip.get();
    tabStrip_->setOnTabActivated([this](std::size_t index) { workspace_.activateTab(index); });
    tabStrip_->setOnTabCloseRequested([this](std::size_t index) { requestCloseTab(index); });
    root_->addChild(std::move(tabStrip));

    auto toolbar = std::make_unique<ui::Toolbar>(kToolbarHeight);
    toolbar_ = toolbar.get();
    root_->addChild(std::move(toolbar));
    buildToolbar();

    sidebar_ = std::make_unique<SidebarController>(*context_, *root_);

    viewport_->setZoomChangedCallback([this](double zoom) { setZoomDisplay(zoom); });
    viewport_->setOnFocusRequested([this] { setFocus(nullptr); });
    // Current-page funnel: keeps the tab's tracked page, the sidebar
    // selection/reveal and the page field in sync.
    viewport_->setCurrentPageChangedCallback([this](std::size_t page) {
        if (DocumentTab* tab = workspace_.activeTab(); tab != nullptr) tab->setCurrentPage(page);
        sidebar_->setCurrentPage(page);
        statusBar_->updatePageIndicator();
    });
    root_->addChild(std::move(viewport));

    // Content slot of Markdown tabs: hidden (empty frame) unless a Markdown
    // tab is active. A later agent replaces the placeholder view behind
    // createMarkdownHostView().
    auto markdownView = createMarkdownHostView(MarkdownHostEnvironment{
        services_.mainDispatcher,
        &scheduler_,
        &services_,
        [this](std::string text) { setStatus(std::move(text)); },
        [this](ui::Widget* widget) { setFocus(widget); },
    });
    markdownView_ = markdownView.get();
    markdownView_->setFrame(kHiddenFrame);
    root_->addChild(std::move(markdownView));

    // Loading / error overlay, drawn above the viewport area.
    auto overlay = std::make_unique<TextLabel>("", ui::Font{14.0, ui::Font::Weight::Semibold},
                                               ui::Color::gray(0.35), ui::TextAlign::Center);
    overlayLabel_ = overlay.get();
    root_->addChild(std::move(overlay));

    searchBar_ = std::make_unique<SearchBarController>(*context_, *root_);
    passwordPrompt_ = std::make_unique<PasswordPromptController>(*context_, *root_);
    statusBar_ = std::make_unique<StatusBarController>(
        *context_, *root_,
        engine_->isAvailable() ? "Ready" : "Ready — PDF support is not built into this binary");

    // Text interaction over the active tab.
    textInteraction_ = std::make_unique<TextInteractionController>(*context_);
    viewport_->setTextBridge(textInteraction_.get());

    // Page editing over the active tab (selection, thumbnails, crop tool).
    pageEditing_ = std::make_unique<PageEditingController>(*context_, *sidebar_, *statusBar_);

    // Annotation tools (above the viewport and the find bar: the note editor
    // floats over them) and their strip under the main toolbar.
    annotations_ = std::make_unique<AnnotationController>(*context_, *root_);
    annotationBar_ = std::make_unique<AnnotationBarController>(*context_, *root_, *annotations_);
    annotationBar_->setOnVisibilityChanged([this] {
        if (annotateButton_ != nullptr) annotateButton_->setActive(annotationBar_->visible());
    });

    // Content editing (Edit / Add Text tools) and its properties strip. The
    // tools share one tool state with the annotation tools.
    content_ = std::make_unique<ContentController>(*context_, *root_, *annotations_, scheduler_,
                                                   createEditorContentBackend());
    contentBar_ = std::make_unique<ContentBarController>(*context_, *root_, *content_);
    const auto syncContentButtons = [this] {
        if (editButton_ != nullptr) editButton_->setActive(content_->tool() == ContentTool::SelectObject);
        if (addTextButton_ != nullptr) addTextButton_->setActive(content_->tool() == ContentTool::AddText);
    };
    content_->setOnStateChanged([this, syncContentButtons] {
        syncContentButtons();
        contentBar_->refresh();
    });

    // File lifecycle (save/save-as/import/merge/extract, dirty close/quit).
    fileLifecycle_ = std::make_unique<FileController>(
        *engine_, *context_, scheduler_, [this](std::string text) { setStatus(std::move(text)); },
        [this] {
            refreshTabStrip();
            updateWindowTitle();
        });
    fileLifecycle_->setSelectionProvider([this] {
        return pageEditing_ != nullptr && pageEditing_->activeSelection() != nullptr
                   ? pageEditing_->targetPages()
                   : std::vector<core::PageId>{};
    });
    fileLifecycle_->installLifecycleHandlers();

    // Workspace events.
    workspace_.setOnTabsChanged([this] { refreshTabStrip(); });
    workspace_.setOnActiveTabChanged([this] { bindActiveTab(); });

    // Layout only once every controller exists (the controllers relayout the
    // shell through the context).
    root_->onLayout = [this] { layoutShell(); };
    layoutShell();
    refreshTabStrip();
    bindActiveTab();
}

// Toolbar items. Fixed frames: the toolbar positions items from their
// current frame sizes. Open is common; the zoom/annotate/edit/print items are
// PDF-only and the mode switcher Markdown-only (applyChromeForKind toggles
// them per active tab kind).
void ShellController::buildToolbar() {
    const auto addPdf = [this](std::unique_ptr<ui::Widget> item, double spacing = ui::Toolbar::kDefaultItemSpacing) {
        pdfToolbarItems_.push_back(item.get());
        toolbar_->addItem(std::move(item), spacing);
    };

    auto openButton = std::make_unique<ui::Button>("Open");
    openButton->setOnClick([this] { handleOpenRequest(); });
    openButton->setFrame(core::Rect{0.0, 0.0, 64.0, 28.0});
    toolbar_->addItem(std::move(openButton), 12.0);

    auto zoomOut = std::make_unique<ui::Button>("−");
    zoomOut->setOnClick([this] { viewport_->zoomOutStep(); });
    zoomOut->setFrame(core::Rect{0.0, 0.0, 36.0, 28.0});
    addPdf(std::move(zoomOut));

    auto zoomLabel = std::make_unique<TextLabel>("100%", ui::Font{13.0}, ui::Color::gray(0.25),
                                                 ui::TextAlign::Center);
    zoomLabel->setFrame(core::Rect{0.0, 0.0, 58.0, 28.0});
    zoomLabel_ = zoomLabel.get();
    addPdf(std::move(zoomLabel));

    auto zoomIn = std::make_unique<ui::Button>("+");
    zoomIn->setOnClick([this] { viewport_->zoomInStep(); });
    zoomIn->setFrame(core::Rect{0.0, 0.0, 36.0, 28.0});
    addPdf(std::move(zoomIn));

    auto actualSize = std::make_unique<ui::Button>("1:1");
    actualSize->setOnClick([this] { viewport_->zoomActualSize(); });
    actualSize->setFrame(core::Rect{0.0, 0.0, 52.0, 28.0});
    addPdf(std::move(actualSize), 0.0);

    auto fitWidth = std::make_unique<ui::Button>("Fit W");
    fitWidth->setOnClick([this] { viewport_->setFitMode(render::ZoomState::FitMode::Width); });
    fitWidth->setFrame(core::Rect{0.0, 0.0, 56.0, 28.0});
    addPdf(std::move(fitWidth), 0.0);

    auto fitPage = std::make_unique<ui::Button>("Fit P");
    fitPage->setOnClick([this] { viewport_->setFitMode(render::ZoomState::FitMode::Page); });
    fitPage->setFrame(core::Rect{0.0, 0.0, 56.0, 28.0});
    addPdf(std::move(fitPage));

    auto annotateButton = std::make_unique<ui::Button>("Annotate");
    annotateButton_ = annotateButton.get();
    annotateButton->setOnClick([this] {
        if (annotationBar_ != nullptr) annotationBar_->toggle();
    });
    annotateButton->setFrame(core::Rect{0.0, 0.0, 84.0, 28.0});
    addPdf(std::move(annotateButton));

    auto editButton = std::make_unique<ui::Button>("Edit");
    editButton_ = editButton.get();
    editButton->setOnClick([this] { performContent(ContentCommand::ToolEdit); });
    editButton->setFrame(core::Rect{0.0, 0.0, 52.0, 28.0});
    addPdf(std::move(editButton));

    auto addTextButton = std::make_unique<ui::Button>("Add Text");
    addTextButton_ = addTextButton.get();
    addTextButton->setOnClick([this] { performContent(ContentCommand::ToolAddText); });
    addTextButton->setFrame(core::Rect{0.0, 0.0, 76.0, 28.0});
    addPdf(std::move(addTextButton));

    auto printButton = std::make_unique<ui::Button>("Print");
    printButton->setOnClick([this] { handlePrintRequest(); });
    printButton->setFrame(core::Rect{0.0, 0.0, 60.0, 28.0});
    addPdf(std::move(printButton));

    // Markdown display modes (segmented-style buttons; hidden for PDF tabs).
    struct ModeButton {
        const char* label;
        MarkdownDisplayMode mode;
        double width;
    };
    const ModeButton modes[] = {{"Rendered", MarkdownDisplayMode::Rendered, 84.0},
                                {"Source", MarkdownDisplayMode::Source, 70.0},
                                {"Split", MarkdownDisplayMode::Split, 60.0}};
    for (std::size_t i = 0; i < 3; ++i) {
        auto button = std::make_unique<ui::Button>(modes[i].label);
        modeButtons_[i] = button.get();
        const MarkdownDisplayMode mode = modes[i].mode;
        button->setOnClick([this, mode] { performMarkdownMode(mode); });
        button->setFrame(core::Rect{0.0, 0.0, modes[i].width, 28.0});
        markdownToolbarItems_.push_back(button.get());
        toolbar_->addItem(std::move(button), i == 2 ? ui::Toolbar::kDefaultItemSpacing : 0.0);
    }
    for (ui::Widget* item : markdownToolbarItems_) toolbar_->setItemVisible(item, false);
}

void ShellController::layoutShell() {
    const core::Rect bounds = root_->bounds();
    const double width = bounds.size.width;
    const double height = bounds.size.height;
    // Markdown tabs have no sidebar, annotation or content bars and no PDF
    // viewport: their view fills the whole content area.
    const bool markdown = chromeMarkdown_;
    core::Rect contentRect{0.0, 0.0, width, height};

    if (presentationMode_) {
        // Chrome collapses to hidden frames; the viewport fills the window.
        tabStrip_->setFrame(kHiddenFrame);
        toolbar_->setFrame(kHiddenFrame);
        sidebar_->layout(kHiddenFrame);
        statusBar_->layout(kHiddenFrame);
        if (annotationBar_ != nullptr) annotationBar_->layout(kHiddenFrame);
        if (contentBar_ != nullptr) contentBar_->layout(kHiddenFrame);
    } else {
        const double top = kTabStripHeight;
        const double statusHeight = StatusBarController::kHeight;
        const double barHeight = (!markdown && annotationBar_ != nullptr) ? annotationBar_->height() : 0.0;
        const double contentBarHeight = (!markdown && contentBar_ != nullptr) ? contentBar_->height() : 0.0;
        const double middleTop = top + kToolbarHeight + barHeight + contentBarHeight;
        const double middleHeight = std::max(0.0, height - middleTop - statusHeight);
        const double sidebarWidth = markdown ? 0.0 : SidebarController::kWidth;
        tabStrip_->setFrame(core::Rect{0.0, 0.0, width, kTabStripHeight});
        toolbar_->setFrame(core::Rect{0.0, top, width, kToolbarHeight});
        if (annotationBar_ != nullptr) {
            annotationBar_->layout(markdown ? kHiddenFrame
                                            : core::Rect{0.0, top + kToolbarHeight, width, barHeight});
        }
        if (contentBar_ != nullptr) {
            contentBar_->layout(markdown ? kHiddenFrame
                                         : core::Rect{0.0, top + kToolbarHeight + barHeight, width,
                                                      contentBarHeight});
        }
        sidebar_->layout(markdown ? kHiddenFrame : core::Rect{0.0, middleTop, sidebarWidth, middleHeight});
        statusBar_->layout(core::Rect{0.0, height - statusHeight, width, statusHeight});
        contentRect = core::Rect{sidebarWidth, middleTop, std::max(0.0, width - sidebarWidth), middleHeight};
    }
    viewport_->setFrame(markdown ? kHiddenFrame : contentRect);
    markdownView_->setFrame(markdown ? contentRect : kHiddenFrame);
    // Everything over the content follows its frame.
    overlayLabel_->setFrame(contentRect);
    searchBar_->layout(viewport_->frame());
    passwordPrompt_->layout(viewport_->frame());
    if (annotations_ != nullptr) annotations_->layout(viewport_->frame());
    if (content_ != nullptr) content_->layout(viewport_->frame());
}

void ShellController::refreshTabStrip() {
    std::vector<ui::TabStrip::Tab> tabs;
    tabs.reserve(workspace_.tabCount());
    for (std::size_t i = 0; i < workspace_.tabCount(); ++i) {
        tabs.push_back(ui::TabStrip::Tab{workspace_.tab(i)->title(), workspace_.tab(i)->isDirty()});
    }
    const std::optional<std::size_t> active =
        workspace_.activeIndex() == DocumentWorkspace::kNoTab
            ? std::nullopt
            : std::optional<std::size_t>{workspace_.activeIndex()};
    tabStrip_->setTabs(std::move(tabs), active);
    viewport_->invalidate();
    updateDocumentEdited();
}

void ShellController::updateDocumentEdited() {
    if (services_.setDocumentEdited == nullptr) return;
    const DocumentTab* tab = workspace_.activeTab();
    services_.setDocumentEdited(tab != nullptr && tab->isDirty());
}

void ShellController::applyChromeForKind(bool markdown) {
    for (ui::Widget* item : pdfToolbarItems_) toolbar_->setItemVisible(item, !markdown);
    for (ui::Widget* item : markdownToolbarItems_) toolbar_->setItemVisible(item, markdown);
    if (chromeMarkdown_ == markdown) return;
    chromeMarkdown_ = markdown;
    layoutShell();
}

void ShellController::syncModeButtons() {
    const std::optional<MarkdownDisplayMode> mode = activeMarkdownMode();
    for (std::size_t i = 0; i < 3; ++i) {
        if (modeButtons_[i] != nullptr) {
            modeButtons_[i]->setActive(mode.has_value() && static_cast<std::size_t>(*mode) == i);
        }
    }
}

std::optional<MarkdownDisplayMode> ShellController::activeMarkdownMode() const {
    const DocumentTab* tab = context_ != nullptr ? context_->readyMarkdownTab() : nullptr;
    if (tab == nullptr) return std::nullopt;
    return tab->markdown()->mode();
}

void ShellController::performMarkdownMode(MarkdownDisplayMode mode) {
    if (DocumentTab* tab = context_->readyMarkdownTab(); tab != nullptr) tab->markdown()->setMode(mode);
}

bool ShellController::canPerformMarkdownMode() const {
    return context_ != nullptr && context_->readyMarkdownTab() != nullptr;
}

// A Markdown tab's state changed (edit, undo/redo, mode, save): repaint the
// view when it is the active tab, always refresh the dirty markers.
void ShellController::onMarkdownChanged(TabId tab) {
    refreshTabStrip();
    const DocumentTab* active = workspace_.activeTab();
    if (active == nullptr || active->id() != tab) return;
    syncModeButtons();
    markdownView_->stateChanged();
}

// Rebinds every view to the active tab. Loading / error / password states
// appear inside this tab's view, never replacing other documents.
void ShellController::bindActiveTab() {
    DocumentTab* tab = workspace_.activeTab();
    overlayLabel_->setText("");
    passwordPrompt_->bindTab(tab);

    // Chrome follows the tab's kind (also while it loads or failed to open).
    const bool markdown = tab != nullptr && tab->isMarkdown();
    // Detach the PDF viewport from the previous tab's view state BEFORE the
    // relayout hides it: a zero-size layout must not rewrite that tab's zoom.
    if (markdown) viewport_->clearDocument();
    applyChromeForKind(markdown);
    if (markdown) {
        bindMarkdownTab(tab);
        return;
    }
    markdownView_->bind(nullptr);

    if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
        viewport_->clearDocument();
        sidebar_->bindTab(nullptr);
        pageEditing_->bindTab(nullptr);
        annotations_->bindTab(nullptr);
        content_->bindTab(nullptr);
        if (tab == nullptr) {
            setStatus("No document open");
            setZoomDisplay(viewport_->zoom().zoom());
        } else if (tab->state() == DocumentTab::State::Loading) {
            overlayLabel_->setText(std::format("Loading {}…", tab->title()));
            setStatus(std::format("Loading {}…", tab->title()));
        } else if (tab->state() == DocumentTab::State::Error) {
            overlayLabel_->setText(std::format("Could not open {} — {}", tab->title(), tab->errorText()));
            setStatus(std::format("Failed to open {}: {}", tab->title(), tab->errorText()));
        } else { // NeedsPassword: the prompt (bound above) has focus
            overlayLabel_->setText(std::format("{} is password protected", tab->title()));
            setStatus(std::format("{} requires a password", tab->title()));
        }
        statusBar_->updatePageIndicator();
        updateWindowTitle();
        return;
    }

    editor::DocumentSession* session = tab->session();
    // Dirty flips (edit, undo back to saved, save) refresh the tab marker and
    // the window's edited dot.
    session->setOnDirtyChanged([this](bool) { refreshTabStrip(); });
    viewport_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                           [session] { return session->revision(); }, &tab->viewState());
    // Thumbnails, page labels, outline and the restored current page.
    sidebar_->bindTab(tab);
    pageEditing_->bindTab(tab);
    annotations_->bindTab(tab);
    content_->bindTab(tab);

    // First bind of a fresh tab: open fit-to-width (Phase 1 behavior). The
    // mode stays active (resizes recompute the zoom) until the user zooms.
    if (!tab->viewStateInitialized()) {
        tab->markViewStateInitialized();
        viewport_->setFitMode(render::ZoomState::FitMode::Width);
    }

    statusBar_->updatePageIndicator();
    setZoomDisplay(viewport_->zoom().zoom());
    setStatus(std::format("{} — {} page{}", tab->title(), session->pageCount(),
                          session->pageCount() == 1 ? "" : "s"));
    updateWindowTitle();

    // Per-tab text interaction: selection repaints the view; the search
    // drives the find bar (which closes: a different tab, a different search).
    textInteraction_->bindTab(*tab);
    searchBar_->bindTab(*tab);
    viewport_->invalidate();
}

// A Markdown tab owns the content area: every PDF binding is dropped (no
// stale session/selection/search reaches the hidden PDF chrome) and the host
// view is bound to the tab's state once it is Ready.
void ShellController::bindMarkdownTab(DocumentTab* tab) {
    viewport_->clearDocument();
    sidebar_->bindTab(nullptr);
    pageEditing_->bindTab(nullptr);
    annotations_->bindTab(nullptr);
    content_->bindTab(nullptr);
    searchBar_->setVisible(false);
    setPresentationMode(false);

    if (tab->state() == DocumentTab::State::Ready && tab->markdown() != nullptr) {
        MarkdownTabState* state = tab->markdown();
        const TabId id = tab->id();
        state->setOnChanged([this, id] { onMarkdownChanged(id); });
        markdownView_->bind(state);
        setStatus(std::format("{} — Markdown ({})", tab->title(),
                              state->lineEnding() == LineEnding::CRLF ? "CRLF" : "LF"));
    } else {
        markdownView_->bind(nullptr);
        if (tab->state() == DocumentTab::State::Loading) {
            overlayLabel_->setText(std::format("Loading {}…", tab->title()));
            setStatus(std::format("Loading {}…", tab->title()));
        } else {
            overlayLabel_->setText(std::format("Could not open {} — {}", tab->title(), tab->errorText()));
            setStatus(std::format("Failed to open {}: {}", tab->title(), tab->errorText()));
        }
    }
    syncModeButtons();
    statusBar_->updatePageIndicator();
    updateWindowTitle();
    updateDocumentEdited();
    markdownView_->invalidate();
}

void ShellController::handleOpenRequest() {
    if (services_.fileDialog == nullptr) {
        setStatus("No file dialog available on this platform backend");
        return;
    }
    const core::Result<std::filesystem::path> chosen = services_.fileDialog->openDocument();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            setStatus("Could not choose a file: " + core::describe(chosen.error()));
        }
        return;
    }
    openDocument(*chosen);
}

void ShellController::openDocument(const std::filesystem::path& path) {
    // Availability is per kind: Markdown never needs the PDF engine.
    if (documentKindForPath(path) == DocumentKind::Pdf && !engine_->isAvailable()) {
        setStatus("PDF support is not built into this binary (RIVET_WITH_PDFIUM=OFF)");
        return;
    }
    // Asynchronous: the loading tab appears immediately; bindActiveTab shows
    // its state. Completing fires onTabsChanged / onActiveTabChanged.
    workspace_.openDocument(path);
}

void ShellController::performPageEdit(PageEditCommand command) {
    if (DocumentTab* tab = context_ != nullptr ? context_->readyMarkdownTab() : nullptr; tab != nullptr) {
        // Markdown: Undo/Redo drive the tab's text command stack; page
        // commands do not exist there.
        if (command != PageEditCommand::Undo && command != PageEditCommand::Redo) {
            setStatus("Page commands are only available for PDF documents");
            return;
        }
        if (tab->markdown()->isEditingLocked()) {
            setStatus("A save is in progress — editing is paused");
        } else if (command == PageEditCommand::Undo) {
            if (!tab->undo()) setStatus("Nothing to undo");
        } else if (!tab->redo()) {
            setStatus("Nothing to redo");
        }
        return;
    }
    if (pageEditing_ != nullptr) pageEditing_->perform(command);
}

bool ShellController::canPerformPageEdit(PageEditCommand command) const {
    if (const DocumentTab* tab = context_ != nullptr ? context_->readyMarkdownTab() : nullptr; tab != nullptr) {
        if (command == PageEditCommand::Undo) return tab->canUndo();
        if (command == PageEditCommand::Redo) return tab->canRedo();
        return false;
    }
    return pageEditing_ != nullptr && pageEditing_->canPerform(command);
}

void ShellController::performAnnotation(AnnotationCommand command) {
    if (annotations_ == nullptr) return;
    // A tool chosen from the menu reveals the strip that shows it.
    if (static_cast<std::size_t>(command) < kAnnotationToolCount && annotationBar_ != nullptr &&
        command != AnnotationCommand::ToolSelect && readyActiveTab() != nullptr) {
        annotationBar_->setVisible(true);
    }
    annotations_->perform(command);
}

bool ShellController::canPerformAnnotation(AnnotationCommand command) const {
    return annotations_ != nullptr && annotations_->canPerform(command);
}

void ShellController::performContent(ContentCommand command) {
    if (content_ != nullptr) content_->perform(command);
}

bool ShellController::canPerformContent(ContentCommand command) const {
    return content_ != nullptr && content_->canPerform(command);
}

void ShellController::performFile(FileCommand command) {
    if (fileLifecycle_ != nullptr) fileLifecycle_->perform(command);
}

bool ShellController::canPerformFile(FileCommand command) const {
    return fileLifecycle_ != nullptr && fileLifecycle_->canPerform(command);
}

void ShellController::requestCloseTab(std::size_t index) {
    cancelPrintForTab(index);
    DocumentTab* tab = workspace_.tab(index);
    if (tab != nullptr && fileLifecycle_ != nullptr) {
        // The prompt is modal and the controller may close the tab itself
        // (Don't Save) or run completions that shift/close other tabs: the
        // index is only valid again through the id.
        const TabId id = tab->id();
        if (!fileLifecycle_->confirmCloseTab(*tab)) {
            return; // the user chose Cancel / Save (closes with the save)
        }
        index = workspace_.indexOfTab(id);
        if (index == DocumentWorkspace::kNoTab) return; // already closed
    }
    workspace_.closeTab(index);
}

void ShellController::setPresentationMode(bool enabled) {
    if (presentationMode_ == enabled) return;
    presentationMode_ = enabled;
    // layoutShell() hides the chrome (or restores its layout slots) and
    // positions everything else.
    viewport_->setPresentationMode(enabled);
    if (enabled) setFocus(nullptr);
    layoutShell();
}

void ShellController::setFocus(ui::Widget* widget) {
    if (focusedWidget_ == widget) return;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(false);
    focusedWidget_ = widget;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(true);
}

bool ShellController::handleKeyEvent(const ui::KeyEvent& event) {
    // 0. Escape cancels a print job being prepared before anything else.
    if (event.key == ui::Key::Escape && printCoordinator_.requestCancel()) return true;

    // 1. The focused widget (a text field) consumes its keys first.
    if (focusedWidget_ != nullptr && focusedWidget_->onKey(event)) return true;

    // Keys of the active viewport tool (Enter/Esc while cropping) run before
    // the shell's own Escape priorities.
    if (pageEditing_ != nullptr && pageEditing_->handleToolKey(event)) return true;

    // Escape: the content tools first (inline editor, a running gesture, the
    // selection), then the find bar / presentation mode below.
    if (event.key == ui::Key::Escape && content_ != nullptr && content_->handleEscape()) return true;

    // Escape closes the search bar / exits presentation mode; otherwise it
    // blurs the focused widget.
    if (event.key == ui::Key::Escape && searchBar_->handleEscape()) return true;
    if (event.key == ui::Key::Escape && presentationMode_) {
        setPresentationMode(false);
        return true;
    }
    if (event.key == ui::Key::Escape && focusedWidget_ != nullptr) {
        setFocus(nullptr);
        return true;
    }

    // 2. Application shortcuts (command/ctrl based).
    if (handleShortcut(event)) return true;

    // 3. Default: the active content (Markdown host or the PDF viewport).
    if (chromeMarkdown_) return markdownView_->onKey(event);
    return viewport_->onKey(event);
}

bool ShellController::handleShortcut(const ui::KeyEvent& event) {
    // Markdown tabs: undo/redo go to the text command stack (rotate has no
    // meaning there) and Cmd+1/2/3 switch the display mode.
    if (context_->readyMarkdownTab() != nullptr) {
        const std::optional<PageEditCommand> edit = pageEditCommandForShortcut(event);
        if (edit == PageEditCommand::Undo || edit == PageEditCommand::Redo) {
            performPageEdit(*edit);
            return true;
        }
        if ((event.modifiers.command || event.modifiers.control) && !event.modifiers.shift &&
            !event.modifiers.option && event.key == ui::Key::Character) {
            if (event.text == "1") { performMarkdownMode(MarkdownDisplayMode::Rendered); return true; }
            if (event.text == "2") { performMarkdownMode(MarkdownDisplayMode::Source); return true; }
            if (event.text == "3") { performMarkdownMode(MarkdownDisplayMode::Split); return true; }
        }
    } else if (pageEditing_ != nullptr && pageEditing_->handleShortcut(event)) {
        // Page editing shortcuts (undo/redo, rotate).
        return true;
    }

    const bool command = event.modifiers.command;
    const bool control = event.modifiers.control;

    if (command || control) {
        switch (event.key) {
        case ui::Key::Plus:
            viewport_->zoomInStep();
            event.accepted = true;
            return true;
        case ui::Key::Minus:
            viewport_->zoomOutStep();
            event.accepted = true;
            return true;
        case ui::Key::Character:
            if (event.text == "0") {
                viewport_->zoomActualSize();
                return true;
            }
            if (event.text == "w") {
                requestCloseTab(workspace_.activeIndex());
                return true;
            }
            if (event.text == "s" && !event.modifiers.shift) {
                performFile(FileCommand::Save);
                return true;
            }
            if (event.text == "S" && event.modifiers.shift) {
                performFile(FileCommand::SaveAs);
                return true;
            }
            if (event.text == "f") {
                if (!chromeMarkdown_) searchBar_->toggle(); // Markdown find: the Markdown view's job
                return true;
            }
            if (event.text == "c") {
                textInteraction_->copySelection();
                return true;
            }
            if (event.text == "p" && !control) {
                handlePrintRequest();
                return true;
            }
            if (event.text == "p" && control && command && !chromeMarkdown_) {
                // Ctrl+Cmd+F is taken by macOS fullscreen; presentation uses
                // Ctrl+Cmd+P (print stays Cmd+P).
                setPresentationMode(!presentationMode_);
                return true;
            }
            if (event.text == "{" || event.text == "[") {
                activateAdjacentTab(-1);
                return true;
            }
            if (event.text == "}" || event.text == "]") {
                activateAdjacentTab(+1);
                return true;
            }
            break;
        case ui::Key::Tab:
            if (control) { // Ctrl+Tab cycles tabs
                activateAdjacentTab(+1);
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

void ShellController::activateAdjacentTab(int delta) {
    const std::size_t count = workspace_.tabCount();
    if (count == 0 || workspace_.activeIndex() == DocumentWorkspace::kNoTab) return;
    // Signed wraparound: prev from 0 lands on the last tab and vice versa.
    const long long next =
        (static_cast<long long>(workspace_.activeIndex()) + delta + static_cast<long long>(count)) %
        static_cast<long long>(count);
    workspace_.activateTab(static_cast<std::size_t>(next));
}

void ShellController::setStatus(std::string text) {
    if (statusBar_ != nullptr) statusBar_->setStatus(std::move(text));
}

void ShellController::setZoomDisplay(double zoom) {
    if (zoomLabel_ != nullptr) zoomLabel_->setText(zoomPercentText(zoom));
}

void ShellController::updateWindowTitle() {
    if (services_.setWindowTitle == nullptr) return;
    const DocumentTab* tab = workspace_.activeTab();
    services_.setWindowTitle(tab != nullptr ? std::format("{} — Rivet", tab->title()) : "Rivet");
}

// Printing: the coordinator runs panel -> worker spool -> platform print for
// the ACTIVE tab; nothing is rasterized on the main thread.
void ShellController::handlePrintRequest() {
    if (context_ != nullptr && context_->readyMarkdownTab() != nullptr) {
        setStatus("Printing is only available for PDF documents");
        return;
    }
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr) {
        setStatus("Nothing to print — open a document first");
        return;
    }
    // The printout reads the document as a whole: commit an open note
    // editor's text first.
    if (context_ != nullptr) context_->pendingEdits.commitAll();
    printCoordinator_.print(tab->id());
}

void ShellController::cancelPrintForTab(std::size_t index) {
    if (DocumentTab* tab = workspace_.tab(index); tab != nullptr && tab->session() != nullptr) {
        printCoordinator_.cancelIfDocument(tab->session()->id());
    }
}

std::unique_ptr<ShellController> createShell(const platform::ShellServices& services) {
    return ShellController::create(services);
}

} // namespace rivet::app
