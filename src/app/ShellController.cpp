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
    auto controller = std::unique_ptr<ShellController>(new ShellController(services));
    controller->buildWidgets();
    return controller;
}

ShellController::ShellController(const platform::ShellServices& services)
    : services_(services),
      scheduler_(0),
      engine_(pdf::createEngine()),
      workspace_(*engine_, scheduler_, services.mainDispatcher),
      printCoordinator_(scheduler_, services.mainDispatcher, services.printService,
                        [this](std::string text) { setStatus(std::move(text)); }) {}

ShellController::~ShellController() {
    // A print spool borrows a session's document: stop it before anything
    // is torn down.
    printCoordinator_.cancel();
    // The viewport must drop its render-source/layout/state pointers before
    // the sessions and the widget tree die.
    viewport_->clearDocument();
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
    tabStrip_->setOnTabCloseRequested([this](std::size_t index) {
        cancelPrintForTab(index);
        workspace_.closeTab(index);
    });
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
// current frame sizes.
void ShellController::buildToolbar() {
    auto openButton = std::make_unique<ui::Button>("Open");
    openButton->setOnClick([this] { handleOpenRequest(); });
    openButton->setFrame(core::Rect{0.0, 0.0, 64.0, 28.0});
    toolbar_->addItem(std::move(openButton), 12.0);

    auto zoomOut = std::make_unique<ui::Button>("−");
    zoomOut->setOnClick([this] { viewport_->zoomOutStep(); });
    zoomOut->setFrame(core::Rect{0.0, 0.0, 36.0, 28.0});
    toolbar_->addItem(std::move(zoomOut));

    auto zoomLabel = std::make_unique<TextLabel>("100%", ui::Font{13.0}, ui::Color::gray(0.25),
                                                 ui::TextAlign::Center);
    zoomLabel->setFrame(core::Rect{0.0, 0.0, 58.0, 28.0});
    zoomLabel_ = zoomLabel.get();
    toolbar_->addItem(std::move(zoomLabel));

    auto zoomIn = std::make_unique<ui::Button>("+");
    zoomIn->setOnClick([this] { viewport_->zoomInStep(); });
    zoomIn->setFrame(core::Rect{0.0, 0.0, 36.0, 28.0});
    toolbar_->addItem(std::move(zoomIn));

    auto actualSize = std::make_unique<ui::Button>("1:1");
    actualSize->setOnClick([this] { viewport_->zoomActualSize(); });
    actualSize->setFrame(core::Rect{0.0, 0.0, 52.0, 28.0});
    toolbar_->addItem(std::move(actualSize), 0.0);

    auto fitWidth = std::make_unique<ui::Button>("Fit W");
    fitWidth->setOnClick([this] { viewport_->setFitMode(render::ZoomState::FitMode::Width); });
    fitWidth->setFrame(core::Rect{0.0, 0.0, 56.0, 28.0});
    toolbar_->addItem(std::move(fitWidth), 0.0);

    auto fitPage = std::make_unique<ui::Button>("Fit P");
    fitPage->setOnClick([this] { viewport_->setFitMode(render::ZoomState::FitMode::Page); });
    fitPage->setFrame(core::Rect{0.0, 0.0, 56.0, 28.0});
    toolbar_->addItem(std::move(fitPage));

    auto printButton = std::make_unique<ui::Button>("Print");
    printButton->setOnClick([this] { handlePrintRequest(); });
    printButton->setFrame(core::Rect{0.0, 0.0, 60.0, 28.0});
    toolbar_->addItem(std::move(printButton));
}

void ShellController::layoutShell() {
    const core::Rect bounds = root_->bounds();
    const double width = bounds.size.width;
    const double height = bounds.size.height;

    if (presentationMode_) {
        // Chrome collapses to hidden frames; the viewport fills the window.
        tabStrip_->setFrame(kHiddenFrame);
        toolbar_->setFrame(kHiddenFrame);
        sidebar_->layout(kHiddenFrame);
        statusBar_->layout(kHiddenFrame);
        viewport_->setFrame(core::Rect{0.0, 0.0, width, height});
    } else {
        const double top = kTabStripHeight;
        const double statusHeight = StatusBarController::kHeight;
        const double middleHeight = std::max(0.0, height - top - kToolbarHeight - statusHeight);
        const double sidebarWidth = SidebarController::kWidth;
        tabStrip_->setFrame(core::Rect{0.0, 0.0, width, kTabStripHeight});
        toolbar_->setFrame(core::Rect{0.0, top, width, kToolbarHeight});
        sidebar_->layout(core::Rect{0.0, top + kToolbarHeight, sidebarWidth, middleHeight});
        statusBar_->layout(core::Rect{0.0, height - statusHeight, width, statusHeight});
        viewport_->setFrame(core::Rect{sidebarWidth, top + kToolbarHeight,
                                       std::max(0.0, width - sidebarWidth), middleHeight});
    }
    // Everything over the viewport follows its frame.
    overlayLabel_->setFrame(viewport_->frame());
    searchBar_->layout(viewport_->frame());
    passwordPrompt_->layout(viewport_->frame());
}

void ShellController::refreshTabStrip() {
    std::vector<ui::TabStrip::Tab> tabs;
    tabs.reserve(workspace_.tabCount());
    for (std::size_t i = 0; i < workspace_.tabCount(); ++i) {
        tabs.push_back(ui::TabStrip::Tab{workspace_.tab(i)->title()});
    }
    const std::optional<std::size_t> active =
        workspace_.activeIndex() == DocumentWorkspace::kNoTab
            ? std::nullopt
            : std::optional<std::size_t>{workspace_.activeIndex()};
    tabStrip_->setTabs(std::move(tabs), active);
    viewport_->invalidate();
}

// Rebinds every view to the active tab. Loading / error / password states
// appear inside this tab's view, never replacing other documents.
void ShellController::bindActiveTab() {
    DocumentTab* tab = workspace_.activeTab();
    overlayLabel_->setText("");
    passwordPrompt_->bindTab(tab);

    if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
        viewport_->clearDocument();
        sidebar_->bindTab(nullptr);
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
    viewport_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                           [session] { return session->revision(); }, &tab->viewState());
    // Thumbnails, page labels, outline and the restored current page.
    sidebar_->bindTab(tab);

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

void ShellController::handleOpenRequest() {
    if (services_.fileDialog == nullptr) {
        setStatus("No file dialog available on this platform backend");
        return;
    }
    const core::Result<std::filesystem::path> chosen = services_.fileDialog->openPdf();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            setStatus("Could not choose a file: " + core::describe(chosen.error()));
        }
        return;
    }
    openDocument(*chosen);
}

void ShellController::openDocument(const std::filesystem::path& path) {
    if (!engine_->isAvailable()) {
        setStatus("PDF support is not built into this binary (RIVET_WITH_PDFIUM=OFF)");
        return;
    }
    // Asynchronous: the loading tab appears immediately; bindActiveTab shows
    // its state. Completing fires onTabsChanged / onActiveTabChanged.
    workspace_.openDocument(path);
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

    // 3. Default: the viewport (scrolling, zoom, page keys).
    return viewport_->onKey(event);
}

bool ShellController::handleShortcut(const ui::KeyEvent& event) {
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
                cancelPrintForTab(workspace_.activeIndex());
                workspace_.closeActiveTab();
                return true;
            }
            if (event.text == "f") {
                searchBar_->toggle();
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
            if (event.text == "p" && control && command) {
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
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr) {
        setStatus("Nothing to print — open a document first");
        return;
    }
    printCoordinator_.print(*tab->session(), tab->title());
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
