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

    const double top = kTabStripHeight;
    const double middleHeight = std::max(0.0, height - top - kToolbarHeight - kStatusBarHeight);
    tabStrip_->setFrame(core::Rect{0.0, 0.0, width, kTabStripHeight});
    toolbar_->setFrame(core::Rect{0.0, top, width, kToolbarHeight});
    sidebar_->setFrame(core::Rect{0.0, top + kToolbarHeight, kSidebarWidth, middleHeight});
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

    // Per-tab text interaction: selection repaints the view, search updates
    // the bar UI and the highlights.
    tab->selection().setCallback([this] { viewport_->invalidate(); });
    if (tab->search() != nullptr) {
        tab->search()->setOnResultsChanged([this] {
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

void ShellController::setFocus(ui::Widget* widget) {
    if (focusedWidget_ == widget) return;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(false);
    focusedWidget_ = widget;
    if (focusedWidget_ != nullptr) focusedWidget_->setFocused(true);
}

bool ShellController::handleKeyEvent(const ui::KeyEvent& event) {
    // 1. The focused widget (a text field) consumes its keys first.
    if (focusedWidget_ != nullptr && focusedWidget_->onKey(event)) return true;

    // Escape closes the search bar (its field consumes Escape while
    // focused); otherwise it blurs the focused widget.
    if (event.key == ui::Key::Escape && searchVisible_) {
        setSearchVisible(false);
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

// UTF-8 encoding of one code point (mirror of the adapter's decoder).
void appendCodePointUtf8(std::string& out, char32_t codePoint) {
    if (codePoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

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
    tab->session()->textService().ensureTextPage(tab->session()->pageId(pageIndex));
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

// Selected text assembly: walk the ordered selection range page by page,
// encoding each character; line-break characters (empty bounds, generated by
// the extractor) become \n so copying across lines keeps sensible breaks.
std::string ShellController::selectedText() const {
    const DocumentTab* tab = workspace_.activeTab();
    if (tab == nullptr || tab->state() != DocumentTab::State::Ready ||
        tab->selection().empty()) {
        return {};
    }
    const editor::DocumentSession* session = tab->session();
    const editor::TextPosition a = tab->selection().anchor();
    const editor::TextPosition b = tab->selection().focus();
    const std::size_t aIndex = session->pageIndexFor(a.page);
    const std::size_t bIndex = session->pageIndexFor(b.page);
    if (aIndex == editor::DocumentSession::kInvalidPage ||
        bIndex == editor::DocumentSession::kInvalidPage) {
        return {};
    }

    std::string out;
    for (std::size_t i = std::min(aIndex, bIndex); i <= std::max(aIndex, bIndex); ++i) {
        const std::shared_ptr<const pdf::PdfTextPage> page =
            session->textService().cachedTextPage(session->pageId(i));
        if (page == nullptr) continue; // not loaded: skip silently
        std::uint32_t begin = 0;
        std::uint32_t end = static_cast<std::uint32_t>(page->charCount());
        if (aIndex == bIndex) {
            begin = std::min(a.characterIndex, b.characterIndex);
            end = std::max(a.characterIndex, b.characterIndex);
        } else if (i == std::min(aIndex, bIndex)) {
            begin = (aIndex == i ? a : b).characterIndex;
        } else if (i == std::max(aIndex, bIndex)) {
            end = (aIndex == i ? a : b).characterIndex;
        }
        const std::vector<pdf::TextChar>& chars = page->chars();
        for (std::uint32_t c = begin; c < end && c < chars.size(); ++c) {
            const pdf::TextChar& ch = chars[c];
            if (ch.bounds.isEmpty() &&
                (ch.unicode == U'\r' || ch.unicode == U'\n')) {
                out.push_back('\n');
                continue;
            }
            if (ch.unicode == 0xFFFDu && ch.bounds.isEmpty()) continue; // no geometry, no text
            appendCodePointUtf8(out, ch.unicode);
        }
        if (i < std::max(aIndex, bIndex)) out.push_back('\n'); // page break
    }
    return out;
}

void ShellController::copySelection() {
    const std::string text = selectedText();
    if (text.empty()) {
        // No selection (or nothing copyable): documented no-op.
        return;
    }
    if (services_.clipboard == nullptr) {
        setStatus("No clipboard available on this platform backend");
        return;
    }
    const core::Status copied = services_.clipboard->setText(text);
    if (!copied.has_value()) {
        setStatus("Copy failed: " + core::describe(copied.error()));
    }
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

std::unique_ptr<ShellController> createShell(const platform::ShellServices& services) {
    return ShellController::create(services);
}

} // namespace rivet::app
