// SPDX-License-Identifier: MPL-2.0
#include "app/ShellController.hpp"

#include "core/Error.hpp"
#include "core/Log.hpp"

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

    auto sidebar = std::make_unique<ui::PageThumbnailList>();
    sidebar_ = sidebar.get();
    root_->addChild(std::move(sidebar));

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

    const double top = kTabStripHeight;
    const double middleHeight = std::max(0.0, height - top - kToolbarHeight - kStatusBarHeight);
    tabStrip_->setFrame(core::Rect{0.0, 0.0, width, kTabStripHeight});
    toolbar_->setFrame(core::Rect{0.0, top, width, kToolbarHeight});
    sidebar_->setFrame(core::Rect{0.0, top + kToolbarHeight, kSidebarWidth, middleHeight});
    viewport_->setFrame(core::Rect{kSidebarWidth, top + kToolbarHeight,
                                   std::max(0.0, width - kSidebarWidth), middleHeight});
    overlayLabel_->setFrame(viewport_->frame());
    statusBar_->setFrame(core::Rect{0.0, height - kStatusBarHeight, width, kStatusBarHeight});
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
        overlayLabel_->setText(std::format("Loading {}…", tab->title()));
        setStatus(std::format("Loading {}…", tab->title()));
        updatePageIndicator();
        updateWindowTitle();
        return;
    }
    if (tab->state() == DocumentTab::State::Error) {
        viewport_->clearDocument();
        sidebar_->clearDocument();
        overlayLabel_->setText(std::format("Could not open {} — {}", tab->title(), tab->errorText()));
        setStatus(std::format("Failed to open {}: {}", tab->title(), tab->errorText()));
        updatePageIndicator();
        updateWindowTitle();
        return;
    }

    editor::DocumentSession* session = tab->session();
    viewport_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                           [session] { return session->revision(); }, &tab->viewState());
    sidebar_->setDocument(session->id(), &session->layout(), &session->renderSource(),
                          [session] { return session->revision(); });

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

void ShellController::setFocus(ui::Widget* widget) {
    if (focusedWidget_ == widget) return;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(false);
    focusedWidget_ = widget;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(true);
}

bool ShellController::handleKeyEvent(const ui::KeyEvent& event) {
    // 1. The focused widget (a text field) consumes its keys first.
    if (focusedWidget_ != nullptr && focusedWidget_->onKey(event)) return true;

    // Escape blurs the focused widget (search UI closes later, in 2B).
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

std::unique_ptr<ShellController> createShell(const platform::ShellServices& services) {
    return ShellController::create(services);
}

} // namespace rivet::app
