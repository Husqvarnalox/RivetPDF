// SPDX-License-Identifier: MPL-2.0
#include "app/ShellController.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"
#include "editor/SelectionText.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kTabStripHeight = 28.0;
constexpr double kToolbarHeight = 40.0;
constexpr double kStatusBarHeight = 26.0;
constexpr double kSidebarWidth = 220.0;
constexpr double kPageFieldWidth = 44.0;

// Page indicator text, "12" style; page numbers shown are 1-based.
std::string pageFieldText(std::size_t page) { return std::format("{}", page + 1); }

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
      workspace_(*engine_, scheduler_, services.mainDispatcher) {}

ShellController::~ShellController() {
    // The viewport must drop its render-source/layout/state pointers before
    // the sessions and the widget tree die.
    viewport_->clearDocument();
    if (services_.setWindowTitle) services_.setWindowTitle("Rivet");
}

void ShellController::buildWidgets() {
    root_ = std::make_unique<ShellRoot>();
    root_->onLayout = [this] { layoutShell(); };

    auto tabStrip = std::make_unique<ui::TabStrip>();
    tabStrip_ = tabStrip.get();
    tabStrip_->setOnTabActivated([this](std::size_t index) { workspace_.activateTab(index); });
    tabStrip_->setOnTabCloseRequested([this](std::size_t index) { workspace_.closeTab(index); });
    root_->addChild(std::move(tabStrip));

    auto toolbar = std::make_unique<ui::Toolbar>(kToolbarHeight);
    toolbar_ = toolbar.get();
    root_->addChild(std::move(toolbar));

    // Sidebar: mode header (Pages | Outline) above the active panel.
    auto sidebarContainer = std::make_unique<ui::Container>();
    sidebarContainer_ = sidebarContainer.get();
    sidebarContainer_->setBackgroundColor(ui::Color::rgba(0.93, 0.93, 0.93, 1.0));
    root_->addChild(std::move(sidebarContainer));

    auto pagesButton = std::make_unique<ui::Button>("Pages");
    pagesModeButton_ = pagesButton.get();
    pagesModeButton_->setFrame(core::Rect{8.0, 6.0, 64.0, 24.0});
    pagesModeButton_->setOnClick([this] { switchSidebarMode(0); });
    sidebarContainer_->addChild(std::move(pagesButton));

    auto outlineButton = std::make_unique<ui::Button>("Outline");
    outlineModeButton_ = outlineButton.get();
    outlineModeButton_->setFrame(core::Rect{76.0, 6.0, 72.0, 24.0});
    outlineModeButton_->setOnClick([this] { switchSidebarMode(1); });
    sidebarContainer_->addChild(std::move(outlineButton));

    auto pagesPanel = std::make_unique<ui::PageThumbnailList>();
    sidebar_ = pagesPanel.get();
    sidebarContainer_->addChild(std::move(pagesPanel));

    auto outlinePanel = std::make_unique<ui::OutlinePanel>();
    outlinePanel_ = outlinePanel.get();
    outlinePanel_->setOnRowActivated([this](std::size_t row) { activateOutlineRow(row); });
    outlinePanel_->setOnExpansionToggled([this](std::size_t row, bool expanded) {
        const std::size_t nodePage = row < outlineRowDestinations_.size()
                                         ? outlineRowDestinations_[row]
                                         : 0;
        (void)nodePage;
        if (row < outlineRowPaths_.size()) {
            if (expanded) {
                expandedOutlinePaths_.insert(outlineRowPaths_[row]);
            } else {
                expandedOutlinePaths_.erase(outlineRowPaths_[row]);
            }
        }
        rebuildOutlineRows();
    });
    sidebarContainer_->addChild(std::move(outlinePanel));

    auto viewport = std::make_unique<ui::PdfViewport>();
    viewport_ = viewport.get();
    viewport_->setZoomChangedCallback([this](double zoom) { setZoomDisplay(zoom); });
    viewport_->setOnFocusRequested([this] { setFocus(nullptr); });
    // Current-page funnel: keeps the tab's tracked page, the sidebar
    // selection/reveal and the page field in sync.
    viewport_->setCurrentPageChangedCallback([this](std::size_t page) {
        if (DocumentTab* tab = workspace_.activeTab(); tab != nullptr) tab->setCurrentPage(page);
        sidebar_->setSelectedIndex(page);
        sidebar_->revealPage(page);
        updatePageIndicator();
    });
    root_->addChild(std::move(viewport));

    // Loading / error overlay, drawn above the viewport area.
    auto overlay = std::make_unique<TextLabel>("", ui::Font{14.0, ui::Font::Weight::Semibold},
                                               ui::Color::gray(0.35), ui::TextAlign::Center);
    overlayLabel_ = overlay.get();
    root_->addChild(std::move(overlay));

    // Search bar: hidden overlay at the viewport's top-right.
    auto searchBar = std::make_unique<ui::Container>();
    searchBar_ = searchBar.get();
    searchBar_->setBackgroundColor(ui::Color::rgba(0.97, 0.97, 0.97, 1.0));
    root_->addChild(std::move(searchBar));

    auto searchField = std::make_unique<ui::TextField>("Find");
    searchField_ = searchField.get();
    searchField_->setFrame(core::Rect{8.0, 5.0, 180.0, 24.0});
    searchField_->setOnFocusRequested([this] { setFocus(searchField_); });
    searchField_->setOnTextChanged([this](const std::string& text) {
        if (DocumentTab* tab = readyActiveTab(); tab != nullptr && tab->search() != nullptr) {
            tab->search()->start(text);
        }
    });
    searchField_->setOnEnter([this] {
        if (DocumentTab* tab = readyActiveTab(); tab != nullptr && tab->search() != nullptr) {
            tab->search()->next();
            revealActiveMatch();
        }
    });
    searchField_->setOnEscape([this] { setSearchVisible(false); });
    searchBar_->addChild(std::move(searchField));

    auto searchPrev = std::make_unique<ui::Button>("↑");
    searchPrev->setFrame(core::Rect{196.0, 5.0, 28.0, 24.0});
    searchPrev->setOnClick([this] {
        if (DocumentTab* tab = readyActiveTab(); tab != nullptr && tab->search() != nullptr) {
            tab->search()->previous();
            revealActiveMatch();
        }
    });
    searchBar_->addChild(std::move(searchPrev));

    auto searchNext = std::make_unique<ui::Button>("↓");
    searchNext->setFrame(core::Rect{228.0, 5.0, 28.0, 24.0});
    searchNext->setOnClick([this] {
        if (DocumentTab* tab = readyActiveTab(); tab != nullptr && tab->search() != nullptr) {
            tab->search()->next();
            revealActiveMatch();
        }
    });
    searchBar_->addChild(std::move(searchNext));

    auto searchCount = std::make_unique<TextLabel>("", ui::Font{12.0}, ui::Color::gray(0.35),
                                                   ui::TextAlign::Right);
    searchCount->setFrame(core::Rect{264.0, 5.0, 90.0, 24.0});
    searchCountLabel_ = searchCount.get();
    searchBar_->addChild(std::move(searchCount));
    // Hidden by default. Direct state + frame here: setSearchVisible() runs
    // the full shell layout, which requires the status bar below to exist.
    searchVisible_ = false;
    searchBar_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});

    // Password overlay for protected documents (hidden by default).
    auto passwordPanel = std::make_unique<ui::Container>();
    passwordPanel_ = passwordPanel.get();
    passwordPanel_->setBackgroundColor(ui::Color::rgba(0.97, 0.97, 0.97, 1.0));
    root_->addChild(std::move(passwordPanel));

    auto passwordMessage = std::make_unique<TextLabel>(
        "This document is password protected", ui::Font{13.0, ui::Font::Weight::Semibold},
        ui::Color::gray(0.2), ui::TextAlign::Left);
    passwordMessage->setFrame(core::Rect{12.0, 10.0, 320.0, 20.0});
    passwordMessage_ = passwordMessage.get();
    passwordPanel_->addChild(std::move(passwordMessage));

    auto passwordField = std::make_unique<ui::TextField>("Password");
    passwordField_ = passwordField.get();
    passwordField_->setFrame(core::Rect{12.0, 36.0, 220.0, 24.0});
    passwordField_->setEchoCharacter(U'\u2022');
    passwordField_->setOnFocusRequested([this] { setFocus(passwordField_); });
    passwordField_->setOnEnter([this] {
        if (DocumentTab* tab = workspace_.activeTab();
            tab != nullptr && tab->state() == DocumentTab::State::NeedsPassword) {
            const std::string password = passwordField_->text();
            passwordField_->setText("");
            workspace_.retryWithPassword(workspace_.activeIndex(), password);
        }
        setFocus(nullptr);
    });
    passwordField_->setOnEscape([this] { setFocus(nullptr); });
    passwordPanel_->addChild(std::move(passwordField));

    auto unlockButton = std::make_unique<ui::Button>("Unlock");
    unlockButton->setFrame(core::Rect{244.0, 36.0, 76.0, 24.0});
    unlockButton->setOnClick([this] {
        if (DocumentTab* tab = workspace_.activeTab();
            tab != nullptr && tab->state() == DocumentTab::State::NeedsPassword) {
            const std::string password = passwordField_->text();
            passwordField_->setText("");
            workspace_.retryWithPassword(workspace_.activeIndex(), password);
        }
    });
    passwordPanel_->addChild(std::move(unlockButton));
    passwordPanel_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});

    auto statusBar = std::make_unique<ui::Container>();
    statusBar_ = statusBar.get();
    statusBar_->setBackgroundColor(ui::Color::rgba(0.93, 0.93, 0.93, 1.0));
    root_->addChild(std::move(statusBar));

    // Toolbar items. Fixed frames: the toolbar positions items from their
    // current frame sizes.
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

    auto statusLabel = std::make_unique<TextLabel>(
        engine_->isAvailable() ? "Ready" : "Ready — PDF support is not built into this binary",
        ui::Font{12.0}, ui::Color::gray(0.35), ui::TextAlign::Left);
    statusLabel->setFrame(core::Rect{0.0, 0.0, 100.0, kStatusBarHeight});
    statusLabel_ = statusLabel.get();
    statusBar_->addChild(std::move(statusLabel));

    // Page indicator on the right side of the status bar:
    //   Page [ field ] / 348
    auto pageCaption = std::make_unique<TextLabel>("Page", ui::Font{12.0}, ui::Color::gray(0.35),
                                                   ui::TextAlign::Right);
    pageCaptionLabel_ = pageCaption.get();
    pageCaption->setFrame(core::Rect{0.0, 3.0, 36.0, 20.0});
    statusBar_->addChild(std::move(pageCaption));

    auto pageField = std::make_unique<ui::TextField>("1");
    pageField_ = pageField.get();
    pageField_->setFrame(core::Rect{0.0, 2.0, kPageFieldWidth, 22.0});
    pageField_->setOnFocusRequested([this] { setFocus(pageField_); });
    pageField_->setOnEnter([this] {
        const std::string& text = pageField_->text();
        // Strict digits only: an invalid page number is ignored (never
        // crashes, never creates invalid scroll state).
        if (!text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            const long long page = std::atoll(text.c_str());
            if (DocumentTab* tab = workspace_.activeTab();
                tab != nullptr && tab->session() != nullptr && page >= 1) {
                viewport_->goToPage(static_cast<std::size_t>(page) - 1);
            }
        }
        setFocus(nullptr);
    });
    pageField_->setOnEscape([this] { setFocus(nullptr); });
    statusBar_->addChild(std::move(pageField));

    auto pageCount = std::make_unique<TextLabel>("/ 0", ui::Font{12.0}, ui::Color::gray(0.35),
                                                 ui::TextAlign::Left);
    pageCount->setFrame(core::Rect{0.0, 3.0, 60.0, 20.0});
    pageCountLabel_ = pageCount.get();
    statusBar_->addChild(std::move(pageCount));

    // Text interaction bridge over the active tab.
    textBridge_ = std::make_unique<ShellTextBridge>(*this);
    viewport_->setTextBridge(textBridge_.get());

    // Workspace events.
    workspace_.setOnTabsChanged([this] { refreshTabStrip(); });
    workspace_.setOnActiveTabChanged([this] { bindActiveTab(); });

    layoutShell();
    refreshTabStrip();
    bindActiveTab();
}

void ShellController::layoutShell() {
    const core::Rect bounds = root_->bounds();
    const double width = bounds.size.width;
    const double height = bounds.size.height;

    if (presentationMode_) {
        // Chrome stays hidden (its saved frames are stale while presenting);
        // the viewport fills the window.
        viewport_->setFrame(core::Rect{0.0, 0.0, width, height});
        overlayLabel_->setFrame(viewport_->frame());
        if (searchVisible_) {
            const double barWidth = 360.0;
            const double barX = std::max(0.0, width - barWidth - 12.0);
            searchBar_->setFrame(core::Rect{barX, 8.0, barWidth, 34.0});
        } else {
            searchBar_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
        }
        return;
    }

    const double top = kTabStripHeight;
    const double middleHeight = std::max(0.0, height - top - kToolbarHeight - kStatusBarHeight);
    const core::Rect newTabStrip{0.0, 0.0, width, kTabStripHeight};
    const core::Rect newToolbar{0.0, top, width, kToolbarHeight};
    const core::Rect newSidebar{0.0, top + kToolbarHeight, kSidebarWidth, middleHeight};
    const core::Rect newStatus{0.0, height - kStatusBarHeight, width, kStatusBarHeight};
    tabStripFrame_ = newTabStrip;
    toolbarFrame_ = newToolbar;
    sidebarFrame_ = newSidebar;
    statusBarFrame_ = newStatus;
    tabStrip_->setFrame(newTabStrip);
    toolbar_->setFrame(newToolbar);
    sidebarContainer_->setFrame(newSidebar);
    statusBar_->setFrame(newStatus);
    const core::Rect sidebarFrame{0.0, top + kToolbarHeight, kSidebarWidth, middleHeight};
    sidebarContainer_->setFrame(sidebarFrame);
    const double headerH = 34.0;
    const core::Rect panelFrame{0.0, headerH, kSidebarWidth,
                                std::max(0.0, middleHeight - headerH)};
    sidebar_->setFrame(panelFrame);
    outlinePanel_->setFrame(panelFrame);
    viewport_->setFrame(core::Rect{kSidebarWidth, top + kToolbarHeight,
                                   std::max(0.0, width - kSidebarWidth), middleHeight});
    overlayLabel_->setFrame(viewport_->frame());
    // Search bar floats at the viewport's top-right (hidden when disabled by
    // clipping: a zero-width frame paints nothing and consumes nothing).
    if (searchVisible_) {
        const double barWidth = 360.0;
        const double barX = std::max(viewport_->frame().origin.x,
                                     viewport_->frame().maxX() - barWidth - 12.0);
        searchBar_->setFrame(core::Rect{barX, viewport_->frame().origin.y + 8.0, barWidth, 34.0});
    } else {
        searchBar_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
    }
    statusLabel_->setFrame(core::Rect{12.0, 0.0, std::max(0.0, width - 220.0), kStatusBarHeight});

    // Page indicator cluster, right-aligned: "Page [field] / N". The count
    // label gets a fixed generous width (no PaintContext during layout).
    const double pageCountWidth = 64.0;
    const double pageCountX = std::max(0.0, width - pageCountWidth - 12.0);
    pageCountLabel_->setFrame(core::Rect{pageCountX, 3.0, pageCountWidth, 20.0});
    const double fieldX = std::max(0.0, pageCountX - kPageFieldWidth - 8.0);
    pageField_->setFrame(core::Rect{fieldX, 2.0, kPageFieldWidth, 22.0});
    if (pageCaptionLabel_ != nullptr) {
        pageCaptionLabel_->setFrame(core::Rect{std::max(0.0, fieldX - 44.0), 3.0, 36.0, 20.0});
    }
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

void ShellController::bindActiveTab() {
    DocumentTab* tab = workspace_.activeTab();
    overlayLabel_->setText("");

    if (tab == nullptr) {
        viewport_->clearDocument();
        sidebar_->clearDocument();
        outlinePanel_->setRows({});
        passwordPanel_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
        setStatus("No document open");
        updatePageIndicator();
        setZoomDisplay(viewport_->zoom().zoom());
        updateWindowTitle();
        return;
    }

    // Loading / error states appear inside this tab's view, never replacing
    // other documents.
    if (tab->state() == DocumentTab::State::Loading) {
        viewport_->clearDocument();
        sidebar_->clearDocument();
        outlinePanel_->setRows({});
        passwordPanel_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
        overlayLabel_->setText(std::format("Loading {}…", tab->title()));
        setStatus(std::format("Loading {}…", tab->title()));
        updatePageIndicator();
        updateWindowTitle();
        return;
    }
    if (tab->state() == DocumentTab::State::Error) {
        viewport_->clearDocument();
        sidebar_->clearDocument();
        outlinePanel_->setRows({});
        overlayLabel_->setText(std::format("Could not open {} — {}", tab->title(), tab->errorText()));
        setStatus(std::format("Failed to open {}: {}", tab->title(), tab->errorText()));
        passwordPanel_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
        updatePageIndicator();
        updateWindowTitle();
        return;
    }

    if (tab->state() == DocumentTab::State::NeedsPassword) {
        viewport_->clearDocument();
        sidebar_->clearDocument();
        outlinePanel_->setRows({});
        overlayLabel_->setText(std::format("{} is password protected", tab->title()));
        setStatus(std::format("{} requires a password", tab->title()));
        // Center the prompt in the viewport area; focus the field.
        const core::Rect vp = viewport_->frame();
        const double pw = 340.0;
        const double ph = 74.0;
        passwordPanel_->setFrame(core::Rect{std::max(vp.origin.x, vp.center().x - pw / 2.0),
                                            std::max(vp.origin.y, vp.center().y - ph / 2.0),
                                            pw, ph});
        passwordMessage_->setText(std::format("{} is password protected — enter the password:",
                                              tab->title()));
        setFocus(passwordField_);
        updatePageIndicator();
        updateWindowTitle();
        return;
    }

    passwordPanel_->setFrame(core::Rect{-1.0, -1.0, 0.0, 0.0});
    editor::DocumentSession* session = tab->session();
    viewport_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                           [session] { return session->revision(); }, &tab->viewState());
    sidebar_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                          [session] { return session->revision(); });
    sidebar_->setPageLabels(session->pageLabels());
    rebuildOutlineRows();

    // First bind of a fresh tab: open fit-to-width (Phase 1 behavior). The
    // mode stays active (resizes recompute the zoom) until the user zooms.
    if (!tab->viewStateInitialized()) {
        tab->markViewStateInitialized();
        viewport_->setFitMode(render::ZoomState::FitMode::Width);
    }

    // Restore the per-tab current page and keep the sidebar in sync.
    sidebar_->setSelectedIndex(tab->currentPage());
    updatePageIndicator();
    setZoomDisplay(viewport_->zoom().zoom());
    setStatus(std::format("{} — {} page{}", tab->title(), session->pageCount(),
                          session->pageCount() == 1 ? "" : "s"));
    updateWindowTitle();

    // Per-tab text interaction: selection repaints the view, search updates
    // the bar UI and the highlights.
    tab->selection().setCallback([this] { viewport_->invalidate(); });
    if (tab->search() != nullptr) {
        // Delivered on the main thread (TextSearchController marshals worker
        // progress through the dispatcher). A background tab's search may
        // still notify: only the active tab drives the bar and the view.
        tab->search()->setOnResultsChanged([this, tab] {
            if (workspace_.activeTab() != tab) return;
            updateSearchUi();
            revealActiveMatch();
            viewport_->invalidate();
        });
    }

    // A different tab has a different search; close the bar on switches.
    setSearchVisible(false);
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
    // Chrome collapses to zero-size (hidden) or restores its layout slot;
    // layoutShell() positions everything else.
    toolbar_->setFrame(enabled ? core::Rect{-1.0, -1.0, 0.0, 0.0} : toolbarFrame_);
    sidebarContainer_->setFrame(enabled ? core::Rect{-1.0, -1.0, 0.0, 0.0} : sidebarFrame_);
    tabStrip_->setFrame(enabled ? core::Rect{-1.0, -1.0, 0.0, 0.0} : tabStripFrame_);
    statusBar_->setFrame(enabled ? core::Rect{-1.0, -1.0, 0.0, 0.0} : statusBarFrame_);
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
    // 1. The focused widget (a text field) consumes its keys first.
    if (focusedWidget_ != nullptr && focusedWidget_->onKey(event)) return true;

    // Escape closes the search bar / exits presentation mode; otherwise it
    // blurs the focused widget.
    if (event.key == ui::Key::Escape && searchVisible_) {
        setSearchVisible(false);
        return true;
    }
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
                workspace_.closeActiveTab();
                return true;
            }
            if (event.text == "f") {
                setSearchVisible(!searchVisible_);
                if (searchVisible_) setFocus(searchField_);
                return true;
            }
            if (event.text == "c") {
                copySelection();
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
    if (statusLabel_ != nullptr) statusLabel_->setText(std::move(text));
}

void ShellController::setZoomDisplay(double zoom) {
    if (zoomLabel_ != nullptr) zoomLabel_->setText(std::format("{:.0f}%", zoom * 100.0));
}

void ShellController::updatePageIndicator() {
    if (DocumentTab* tab = workspace_.activeTab(); tab != nullptr && tab->session() != nullptr) {
        if (pageCountLabel_ != nullptr) {
            pageCountLabel_->setText(std::format("/ {}", tab->session()->pageCount()));
        }
        // Do not fight a focused field: the user is editing the page number.
        if (pageField_ != nullptr && focusedWidget_ != pageField_) {
            pageField_->setText(pageFieldText(tab->currentPage()));
        }
        return;
    }
    if (pageCountLabel_ != nullptr) pageCountLabel_->setText("/ 0");
    if (pageField_ != nullptr) pageField_->setText("");
}

void ShellController::updateWindowTitle() {
    if (services_.setWindowTitle == nullptr) return;
    const DocumentTab* tab = workspace_.activeTab();
    services_.setWindowTitle(tab != nullptr ? std::format("{} — Rivet", tab->title()) : "Rivet");
}


// ---------- Text interaction (ShellTextBridge + helpers) ----------

namespace {

// Overlay colors.
constexpr ui::Color kSelectionHighlight = ui::Color::rgba(0.30, 0.55, 1.0, 0.35);
constexpr ui::Color kSearchMatch = ui::Color::rgba(1.0, 0.85, 0.25, 0.45);
constexpr ui::Color kActiveSearchMatch = ui::Color::rgba(1.0, 0.60, 0.05, 0.65);

} // namespace

DocumentTab* ShellController::readyActiveTab() {
    DocumentTab* tab = workspace_.activeTab();
    return (tab != nullptr && tab->state() == DocumentTab::State::Ready) ? tab : nullptr;
}

void ShellController::ShellTextBridge::warmPage(std::size_t pageIndex) {
    DocumentTab* tab = shell_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    const core::PageId pageId = tab->session()->pageId(pageIndex);
    tab->session()->textService().ensureTextPage(pageId);
    tab->session()->linkService().ensurePageLinks(pageId);
}

std::optional<std::uint32_t> ShellController::ShellTextBridge::charIndexAtPoint(
    std::size_t pageIndex, const core::Point& pagePoint) {
    DocumentTab* tab = shell_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return std::nullopt;
    const std::shared_ptr<const pdf::PdfTextPage> page =
        tab->session()->textService().cachedTextPage(tab->session()->pageId(pageIndex));
    if (page == nullptr) return std::nullopt;
    return page->charIndexAtPoint(pagePoint);
}

std::vector<ui::OverlayRect> ShellController::ShellTextBridge::overlayRects(std::size_t pageIndex) {
    return shell_.overlayRectsForActiveTab(pageIndex);
}

void ShellController::ShellTextBridge::selectionDragBegan(std::size_t pageIndex,
                                                          std::uint32_t charIndex, bool shiftHeld) {
    DocumentTab* tab = shell_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    const editor::TextPosition position{tab->session()->pageId(pageIndex), charIndex};
    if (shiftHeld) {
        tab->selection().extendTo(position);
    } else {
        tab->selection().start(position);
    }
}

void ShellController::ShellTextBridge::selectionDragMoved(std::size_t pageIndex,
                                                          std::uint32_t charIndex) {
    DocumentTab* tab = shell_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return;
    tab->selection().setFocus(editor::TextPosition{tab->session()->pageId(pageIndex), charIndex});
}

void ShellController::ShellTextBridge::selectionDragEnded() {}

void ShellController::ShellTextBridge::selectionCleared() {
    if (DocumentTab* tab = shell_.readyActiveTab(); tab != nullptr) tab->selection().clear();
}

std::optional<ui::ViewerLinkHit> ShellController::ShellTextBridge::linkAtPoint(
    std::size_t pageIndex, const core::Point& pagePoint) {
    DocumentTab* tab = shell_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return std::nullopt;
    // Cached links only: the page's links load asynchronously (warmed on
    // page tracking); a cold page reports no links rather than blocking.
    const std::vector<pdf::PdfPageLink> links =
        tab->session()->linkService().cachedLinks(tab->session()->pageId(pageIndex));
    for (const pdf::PdfPageLink& link : links) {
        for (const core::Rect& rect : link.rects) {
            if (rect.contains(pagePoint)) {
                ui::ViewerLinkHit hit;
                hit.kind = link.kind == pdf::PdfPageLink::Kind::Internal
                               ? ui::ViewerLinkHit::Kind::Internal
                               : ui::ViewerLinkHit::Kind::External;
                if (link.kind == pdf::PdfPageLink::Kind::Internal) {
                    hit.pageIndex = link.destination.pageIndex;
                    hit.hasPoint = link.destination.hasPoint;
                    hit.point = link.destination.point;
                }
                hit.url = link.url;
                return hit;
            }
        }
    }
    return std::nullopt;
}

std::vector<core::Rect> ShellController::ShellTextBridge::linkRects(std::size_t pageIndex) {
    std::vector<core::Rect> rects;
    DocumentTab* tab = shell_.readyActiveTab();
    if (tab == nullptr || pageIndex >= tab->session()->pageCount()) return rects;
    const std::vector<pdf::PdfPageLink> links =
        tab->session()->linkService().cachedLinks(tab->session()->pageId(pageIndex));
    for (const pdf::PdfPageLink& link : links) {
        rects.insert(rects.end(), link.rects.begin(), link.rects.end());
    }
    return rects;
}

void ShellController::ShellTextBridge::linkActivated(const ui::ViewerLinkHit& hit) {
    if (hit.kind == ui::ViewerLinkHit::Kind::Internal) {
        shell_.navigateInternalDestination(hit.pageIndex, hit.point, hit.hasPoint);
        return;
    }
    shell_.openExternalUrl(hit.url);
}

// Selection + search highlights for one page, in page display space. The
// selection spans a page range resolved through the session's page ordering
// (PageId alone has no order); per page it clips to the selected character
// range. Requires the page text to be cached - pages under a selection are
// warm by construction (the viewport warms them on page tracking).
std::vector<ui::OverlayRect> ShellController::overlayRectsForActiveTab(std::size_t pageIndex) const {
    std::vector<ui::OverlayRect> rects;
    const DocumentTab* tab = workspace_.activeTab();
    if (tab == nullptr || tab->state() != DocumentTab::State::Ready) return rects;
    const editor::DocumentSession* session = tab->session();

    // Search matches on this page (active match stronger).
    if (const editor::TextSearchController* search = tab->search(); search != nullptr) {
        const std::optional<std::size_t> active = search->currentIndex();
        const std::vector<editor::TextSearchController::Match> matches = search->matches();
        for (std::size_t m = 0; m < matches.size(); ++m) {
            if (session->pageIndexFor(matches[m].page) != pageIndex) continue;
            const std::shared_ptr<const pdf::PdfTextPage> page =
                session->textService().cachedTextPage(matches[m].page);
            if (page == nullptr) continue;
            for (const core::Rect& rect : page->rectsForRange(matches[m].startIndex, matches[m].count)) {
                rects.push_back(ui::OverlayRect{rect, (active && *active == m) ? kActiveSearchMatch
                                                                               : kSearchMatch});
            }
        }
    }

    // Selection highlight.
    if (!tab->selection().empty()) {
        const editor::TextPosition a = tab->selection().anchor();
        const editor::TextPosition b = tab->selection().focus();
        const std::size_t aIndex = session->pageIndexFor(a.page);
        const std::size_t bIndex = session->pageIndexFor(b.page);
        if (aIndex != editor::DocumentSession::kInvalidPage &&
            bIndex != editor::DocumentSession::kInvalidPage && pageIndex >= std::min(aIndex, bIndex) &&
            pageIndex <= std::max(aIndex, bIndex)) {
            const std::size_t firstIdx = std::min(aIndex, bIndex);
            const std::size_t lastIdx = std::max(aIndex, bIndex);
            std::uint32_t begin = 0;
            std::uint32_t end = 0;
            if (aIndex == bIndex) {
                begin = std::min(a.characterIndex, b.characterIndex);
                end = std::max(a.characterIndex, b.characterIndex);
            } else if (pageIndex == firstIdx) {
                begin = (aIndex == firstIdx ? a : b).characterIndex;
                end = static_cast<std::uint32_t>(0xFFFFFFFFu); // clamped below
            } else if (pageIndex == lastIdx) {
                begin = 0;
                end = (aIndex == lastIdx ? a : b).characterIndex;
            }
            const std::shared_ptr<const pdf::PdfTextPage> page =
                session->textService().cachedTextPage(session->pageId(pageIndex));
            if (page != nullptr) {
                end = static_cast<std::uint32_t>(
                    std::min<std::size_t>(end == 0xFFFFFFFFu ? page->charCount() : end,
                                          page->charCount()));
                if (begin < end) {
                    for (const core::Rect& rect : page->rectsForRange(begin, end - begin)) {
                        rects.push_back(ui::OverlayRect{rect, kSelectionHighlight});
                    }
                }
            }
        }
    }
    return rects;
}

// Copy: the selection is snapshotted into per-page ranges in reading order
// and handed to the text service, which extracts EVERY page (cache or worker
// extraction - never a silent skip) and assembles the exact UTF-8 text off
// the main thread. The clipboard is written on the main thread when the
// result arrives; a failure is reported, never a partial copy. The delivery
// dies with the session's text service (tab closed -> no callback).
void ShellController::copySelection() {
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->selection().empty()) return; // documented no-op
    if (services_.clipboard == nullptr) {
        setStatus("No clipboard available on this platform backend");
        return;
    }
    editor::DocumentSession* session = tab->session();
    std::vector<editor::TextRange> ranges = editor::orderedSelectionRanges(
        tab->selection().selection(),
        [session](core::PageId id) { return session->pageIndexFor(id); },
        [session](std::size_t index) { return session->pageId(index); },
        editor::DocumentSession::kInvalidPage);
    if (ranges.empty()) return;
    const std::size_t pages = ranges.size();
    if (pages > 1) setStatus(std::format("Copying text from {} pages…", pages));
    session->textService().requestRangesText(
        std::move(ranges), [this](core::Result<std::string> text) {
            if (!text.has_value()) {
                setStatus("Copy failed: " + core::describe(text.error()));
                return;
            }
            if (text->empty()) return;
            const core::Status copied = services_.clipboard->setText(*text);
            setStatus(copied.has_value() ? std::format("Copied {} characters", text->size())
                                         : "Copy failed: " + core::describe(copied.error()));
        });
}

void ShellController::setSearchVisible(bool visible) {
    searchVisible_ = visible;
    if (!visible) {
        if (focusedWidget_ == searchField_) setFocus(nullptr);
        if (DocumentTab* tab = readyActiveTab(); tab != nullptr && tab->search() != nullptr) {
            tab->search()->cancel();
        }
    }
    layoutShell();
    viewport_->invalidate();
}

void ShellController::updateSearchUi() {
    if (searchCountLabel_ == nullptr) return;
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->search() == nullptr) return;
    const editor::TextSearchController* search = tab->search();
    if (search->query().empty()) {
        searchCountLabel_->setText("");
        return;
    }
    const std::size_t total = search->matches().size();
    const std::optional<std::size_t> active = search->currentIndex();
    if (total == 0) {
        searchCountLabel_->setText(search->searching() ? "Searching…" : "No matches");
    } else {
        searchCountLabel_->setText(std::format("{} / {}", active ? *active + 1 : 0, total));
    }
}

void ShellController::revealActiveMatch() {
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->search() == nullptr) return;
    const editor::TextSearchController* search = tab->search();
    const std::optional<std::size_t> active = search->currentIndex();
    if (!active.has_value()) return;
    const std::vector<editor::TextSearchController::Match> matches = search->matches();
    if (*active >= matches.size()) return;
    const editor::DocumentSession* session = tab->session();
    const std::size_t pageIndex = session->pageIndexFor(matches[*active].page);
    if (pageIndex == editor::DocumentSession::kInvalidPage) return;
    const std::shared_ptr<const pdf::PdfTextPage> page =
        session->textService().cachedTextPage(matches[*active].page);
    if (page == nullptr) return;
    const std::vector<core::Rect> rects =
        page->rectsForRange(matches[*active].startIndex, matches[*active].count);
    if (!rects.empty()) viewport_->revealContentRect(pageIndex, rects.front());
}


// ---------- Sidebar outline mode ----------

// Depth-first flatten of the document outline into visible rows. Paths
// (ancestor indexes) key the expansion state so a rebuild keeps it stable;
// nodes whose path is not in expandedOutlinePaths_ collapse their subtree.
void ShellController::rebuildOutlineRows() {
    outlineRows_.clear();
    outlineRowPaths_.clear();
    outlineRowDestinations_.clear();

    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr) {
        outlinePanel_->setRows({});
        return;
    }
    // The outline loads on the session's navigation stream (a PDFium walk
    // never runs on the main thread); rebuild once it arrives, if this
    // session is still the active one. The delivery dies with the session.
    editor::DocumentSession* session = tab->session();
    const editor::LinkService::Outline outline = session->linkService().cachedOutline();
    if (outline == nullptr) {
        outlinePanel_->setRows({});
        session->linkService().requestOutline([this, session](editor::LinkService::Outline) {
            DocumentTab* active = readyActiveTab();
            if (active != nullptr && active->session() == session) rebuildOutlineRows();
        });
        return;
    }
    if (!outline->has_value()) {
        outlinePanel_->setRows({});
        return;
    }

    // Iterative DFS with an explicit path stack (the tree is depth-bounded
    // by the adapter, but recursion is still avoided here for uniformity).
    // The synthetic root is SKIPPED: its children are the top-level rows,
    // expanded by default so the outline is readable on first open.
    struct Frame {
        const rivet::pdf::PdfOutlineNode* node;
        std::vector<std::size_t> path;
        std::size_t childIndex;
    };
    const rivet::pdf::PdfOutlineNode& rootNode = **outline;
    std::vector<Frame> stack;
    for (std::size_t i = rootNode.children.size(); i > 0; --i) {
        std::vector<std::size_t> path{i - 1};
        expandedOutlinePaths_.insert(path); // top level starts expanded
        stack.push_back(Frame{&rootNode.children[i - 1], std::move(path), 0});
    }

    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.childIndex == 0) {
            ui::OutlineRow row;
            row.title = frame.node->title;
            row.depth = static_cast<int>(frame.path.size());
            row.hasChildren = !frame.node->children.empty();
            row.expanded =
                row.hasChildren && expandedOutlinePaths_.count(frame.path) > 0;
            // Outline rows navigate to the node's destination page.
            outlineRowDestinations_.push_back(
                frame.node->destination.has_value() &&
                        frame.node->destination->pageIndex < tab->session()->pageCount()
                    ? frame.node->destination->pageIndex
                    : 0);
            outlineRowPaths_.push_back(frame.path);
            outlineRows_.push_back(std::move(row));
        }
        const bool expanded =
            !frame.node->children.empty() &&
            expandedOutlinePaths_.count(frame.path) > 0;
        if (expanded && frame.childIndex < frame.node->children.size()) {
            const rivet::pdf::PdfOutlineNode* child = &frame.node->children[frame.childIndex];
            std::vector<std::size_t> childPath = frame.path;
            childPath.push_back(frame.childIndex);
            ++frame.childIndex;
            stack.push_back(Frame{child, std::move(childPath), 0});
        } else {
            stack.pop_back();
        }
    }
    outlinePanel_->setRows(outlineRows_);
}

void ShellController::activateOutlineRow(std::size_t rowIndex) {
    if (rowIndex >= outlineRowDestinations_.size()) return;
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr) return;
    const std::size_t pageIndex = outlineRowDestinations_[rowIndex];
    if (pageIndex >= tab->session()->pageCount()) return;
    viewport_->goToPage(pageIndex);
}

void ShellController::switchSidebarMode(int mode) {
    sidebarMode_ = mode;
    const bool pages = mode == 0;
    // Both panels share the same slot; capture it before reassigning frames.
    const core::Rect panelFrame = sidebar_->frame();
    const core::Rect hidden{-1.0, -1.0, 0.0, 0.0};
    sidebar_->setFrame(pages ? panelFrame : hidden);
    outlinePanel_->setFrame(pages ? hidden : panelFrame);
    if (pagesModeButton_ != nullptr) {
        pagesModeButton_->setLabel(pages ? "Pages •" : "Pages");
    }
    if (outlineModeButton_ != nullptr) {
        outlineModeButton_->setLabel(!pages ? "Outline •" : "Outline");
    }
    root_->invalidate();
}

// ---------- Link navigation ----------

void ShellController::navigateInternalDestination(std::size_t pageIndex,
                                                  const core::Point& targetPoint,
                                                  bool hasPoint) {
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr || pageIndex >= tab->session()->pageCount()) {
        return;
    }
    if (hasPoint) {
        // Center a small region around the target point, keeping the zoom.
        viewport_->revealContentRect(pageIndex,
                                     core::Rect{targetPoint.x - 50.0, targetPoint.y - 50.0,
                                                100.0, 100.0});
    } else {
        viewport_->goToPage(pageIndex);
    }
}

// Printing: builds the request from the ACTIVE tab; page content comes from
// the session's Rivet render path (whole pages at the platform's capped
// density). The callback runs during the print operation on the main thread;
// the global PDFium gate serializes it with any background rendering.
void ShellController::handlePrintRequest() {
    DocumentTab* tab = readyActiveTab();
    if (tab == nullptr || tab->session() == nullptr) {
        setStatus("Nothing to print — open a document first");
        return;
    }
    if (services_.printService == nullptr) {
        setStatus("No print service available on this platform backend");
        return;
    }
    editor::DocumentSession* session = tab->session();

    platform::PrintRequest request;
    request.jobTitle = tab->title();
    request.pageSizesPoints.reserve(session->pageCount());
    for (std::size_t i = 0; i < session->pageCount(); ++i) {
        request.pageSizesPoints.push_back(session->pageSizePoints(i));
    }
    // Render at the print density; the whole page in one bitmap (bounded by
    // the platform's density cap). Runs on the main thread inside the print
    // operation; the gate serializes with worker renders.
    request.renderPage = [session](std::size_t pageIndex, double devicePixelsPerPoint)
        -> core::Result<core::Bitmap> {
        if (pageIndex >= session->pageCount()) {
            return std::unexpected(core::Error{core::ErrorCode::InvalidArgument,
                                               "page index out of range", "app"});
        }
        const core::Size size = session->pageSizePoints(pageIndex);
        return session->document().renderPage(
            pageIndex, core::Rect{core::Point{0.0, 0.0}, size}, devicePixelsPerPoint);
    };

    const core::Status printed = services_.printService->printDocument(request);
    if (!printed.has_value()) {
        if (printed.error().code != core::ErrorCode::Cancelled) {
            setStatus("Print failed: " + core::describe(printed.error()));
        }
        return;
    }
    setStatus(std::format("Printed {} ", tab->title()));
}

// External URL policy: explicit user click (the viewport fires linkActivated
// only for real clicks) + the deliberate scheme allow-list in UrlPolicy.hpp.
void ShellController::openExternalUrl(const std::string& url) {
    if (!isAllowedExternalUrlScheme(url)) {
        core::log::warning("blocked external link with a disallowed scheme");
        setStatus("Blocked external link (unsupported scheme)");
        return;
    }
    if (services_.urlOpener == nullptr) {
        setStatus("No URL opener available on this platform backend");
        return;
    }
    const core::Status opened = services_.urlOpener->openUrl(url);
    if (!opened.has_value()) {
        setStatus("Could not open link: " + core::describe(opened.error()));
    }
}

std::unique_ptr<ShellController> createShell(const platform::ShellServices& services) {
    return ShellController::create(services);
}

} // namespace rivet::app
