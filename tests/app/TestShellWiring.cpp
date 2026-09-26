// SPDX-License-Identifier: MPL-2.0
// Shell feature controllers (SearchBarController, StatusBarController,
// PasswordPromptController, TextInteractionController, SidebarController)
// wired exactly like ShellController (same context, controllers and bind
// sequence) over a real DocumentWorkspace with a text/outline-capable fake
// engine. Deterministic: the test thread is the main thread and only blocks
// on the dispatcher queue (condition variable, no sleeps) while background
// work completes.
#include "RivetTest.h"

#include "app/DocumentWorkspace.hpp"
#include "app/PasswordPromptController.hpp"
#include "app/SearchBarController.hpp"
#include "app/ShellContext.hpp"
#include "app/SidebarController.hpp"
#include "app/StatusBarController.hpp"
#include "app/TextInteractionController.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfText.hpp"
#include "pdf/PdfTypes.hpp"
#include "platform/Clipboard.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextField.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using rivet::app::DocumentTab;
using rivet::app::DocumentWorkspace;
using rivet::app::FlattenedOutline;
using rivet::app::OutlinePath;
using rivet::app::PasswordPromptController;
using rivet::app::SearchBarController;
using rivet::app::ShellContext;
using rivet::app::SidebarController;
using rivet::app::StatusBarController;
using rivet::app::TextInteractionController;
using rivet::core::Bitmap;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::Status;
using rivet::core::TaskScheduler;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfOutlineNode;
using rivet::pdf::PdfPageInfo;
using rivet::pdf::PdfTextPage;
using rivet::pdf::TextChar;
using rivet::ui::Key;
using rivet::ui::KeyEvent;

namespace {

std::shared_ptr<const PdfTextPage> makePage(const std::u32string& text) {
    std::vector<TextChar> chars;
    for (std::size_t i = 0; i < text.size(); ++i) {
        TextChar ch;
        ch.unicode = text[i];
        ch.index = static_cast<std::uint32_t>(i);
        ch.bounds = Rect{static_cast<double>(i) * 8.0, 100.0, 7.0, 12.0};
        ch.fontSize = 12.0;
        chars.push_back(ch);
    }
    return std::make_shared<const PdfTextPage>(std::move(chars));
}

PdfOutlineNode outlineNode(std::string title, std::size_t page, std::vector<PdfOutlineNode> children = {}) {
    PdfOutlineNode node;
    node.title = std::move(title);
    node.destination = rivet::pdf::PdfDestination{};
    node.destination->pageIndex = page;
    node.children = std::move(children);
    return node;
}

// Three text pages with a two-level outline.
class TextDocument final : public PdfDocument {
public:
    TextDocument() { info_.pageCount = pages_.size(); }
    const PdfDocumentInfo& info() const override { return info_; }
    Result<PdfPageInfo> pageInfo(std::size_t index) const override {
        if (index >= pages_.size()) return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        return PdfPageInfo{index, Size{612.0, 792.0}, rivet::core::PageRotation::None};
    }
    Result<Bitmap> renderPage(std::size_t, const Rect&, double) override { return Bitmap::create(2, 2); }
    Result<std::shared_ptr<const PdfTextPage>> textPage(std::size_t index) const override {
        if (index >= pages_.size()) return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        return makePage(pages_[index]);
    }
    Result<std::optional<PdfOutlineNode>> outline() const override {
        PdfOutlineNode root;
        root.children.push_back(outlineNode("Chapter 1", 0, {outlineNode("Section 1.1", 1)}));
        root.children.push_back(outlineNode("Chapter 2", 2));
        return std::optional<PdfOutlineNode>{std::move(root)};
    }

private:
    std::vector<std::u32string> pages_{U"alpha beta", U"beta gamma", U"delta beta"};
    PdfDocumentInfo info_;
};

class Engine final : public PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "wiring"; }
    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path&,
                                                      std::string_view password) override {
        if (passwordRequired && password != "secret") {
            return std::unexpected(Error{ErrorCode::PasswordRequired, "locked", "test"});
        }
        return std::unique_ptr<PdfDocument>(std::make_unique<TextDocument>());
    }
    bool passwordRequired = false;
};

// Main-thread queue; the test thread pumps it. waitUntil() blocks on the
// queue (never sleeps) until the predicate holds after a pump.
class WaitDispatcher final : public rivet::core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    void pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
    }
    bool waitUntil(const std::function<bool()>& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            pump();
            if (predicate()) return true;
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_until(lock, deadline, [this] { return !queue_.empty(); })) return predicate();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

class FakeClipboard final : public rivet::platform::IClipboard {
public:
    Status setText(const std::string& text) override {
        text_ = text;
        ++writes;
        return {};
    }
    std::string text() const override { return text_; }
    int writes = 0;

private:
    std::string text_;
};

std::filesystem::path tempPdf(const char* name) {
    auto path = std::filesystem::temp_directory_path() /
                std::filesystem::path{std::string{"rivet-wiring-"} + name + ".pdf"};
    if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
        std::fputs("%PDF-1.4\n", file);
        std::fclose(file);
    }
    return path;
}

// A miniature shell: the same context, controllers and bind sequence as
// ShellController, over a fake engine. Member order mirrors the shell's
// destruction contract (controllers die before the tree and the workspace).
struct Shell {
    Engine engine;
    TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    FakeClipboard clipboard;
    rivet::platform::ShellServices services;

    std::unique_ptr<rivet::ui::Container> root = std::make_unique<rivet::ui::Container>();
    rivet::ui::PdfViewport* viewport = nullptr;
    rivet::ui::Widget* focused = nullptr;
    std::vector<std::string> statusLog;
    int relayouts = 0;

    std::unique_ptr<ShellContext> context;
    std::unique_ptr<TextInteractionController> text;
    std::unique_ptr<SidebarController> sidebar;
    std::unique_ptr<SearchBarController> search;
    std::unique_ptr<PasswordPromptController> password;
    std::unique_ptr<StatusBarController> status;

    Shell() {
        services.mainDispatcher = &dispatcher;
        services.clipboard = &clipboard;
        auto vp = std::make_unique<rivet::ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(Rect{220.0, 68.0, 800.0, 600.0});
        context = std::make_unique<ShellContext>(ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) {
                statusLog.push_back(message);
                if (status != nullptr) status->setStatus(std::move(message));
            },
            [this](rivet::ui::Widget* widget) {
                if (focused == widget) return;
                if (focused != nullptr) focused->setFocused(false);
                focused = widget;
                if (focused != nullptr) focused->setFocused(true);
            },
            [this] { ++relayouts; },
        });
        sidebar = std::make_unique<SidebarController>(*context, *root);
        root->addChild(std::move(vp));
        search = std::make_unique<SearchBarController>(*context, *root);
        password = std::make_unique<PasswordPromptController>(*context, *root);
        status = std::make_unique<StatusBarController>(*context, *root, "Ready");
        text = std::make_unique<TextInteractionController>(*context);
        viewport->setTextBridge(text.get());
        sidebar->layout(Rect{0.0, 68.0, SidebarController::kWidth, 600.0});
        status->layout(Rect{0.0, 668.0, 1020.0, StatusBarController::kHeight});
        password->layout(viewport->frame());
        workspace.setOnActiveTabChanged([this] { bind(); });
    }

    ~Shell() {
        viewport->clearDocument();
        viewport->setTextBridge(nullptr);
        workspace.setOnActiveTabChanged({});
        text.reset();
        status.reset();
        password.reset();
        search.reset();
        sidebar.reset();
    }

    // ShellController::bindActiveTab, minus overlay/title/zoom.
    void bind() {
        DocumentTab* tab = workspace.activeTab();
        password->bindTab(tab);
        if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
            viewport->clearDocument();
            sidebar->bindTab(nullptr);
            status->updatePageIndicator();
            return;
        }
        rivet::editor::DocumentSession* session = tab->session();
        viewport->setDocument(session->id(), &session->layout(), &session->renderSource(),
                              [session] { return session->revision(); }, &tab->viewState());
        sidebar->bindTab(tab);
        status->updatePageIndicator();
        text->bindTab(*tab);
        search->bindTab(*tab);
    }

    DocumentTab* open(const char* name) {
        workspace.openDocument(tempPdf(name));
        DocumentTab* tab = workspace.activeTab();
        CHECK(tab != nullptr);
        CHECK(dispatcher.waitUntil([tab] { return tab->state() != DocumentTab::State::Loading; }));
        return tab;
    }

    void typeInto(rivet::ui::TextField& field, const std::string& chars) {
        context->setFocus(&field);
        for (const char c : chars) {
            KeyEvent event;
            event.key = Key::Character;
            event.text = std::string(1, c);
            field.onKey(event);
        }
    }
    static void press(rivet::ui::Widget& widget, Key key) {
        KeyEvent event;
        event.key = key;
        widget.onKey(event);
    }
};

} // namespace

RIVET_TEST(wiringSearchBarQueryStartsSearchAndStepsMatches) {
    Shell s;
    DocumentTab* tab = s.open("search");
    CHECK(tab->state() == DocumentTab::State::Ready);
    CHECK(tab->search() != nullptr);

    // Cmd+F toggles the bar open and focuses the field.
    s.search->toggle();
    CHECK(s.search->visible());
    CHECK(s.focused == &s.search->field());
    CHECK_GE(s.relayouts, 1);

    // Typing drives the ACTIVE tab's search.
    s.typeInto(s.search->field(), "beta");
    CHECK_EQ(tab->search()->query(), std::string("beta"));
    CHECK(s.dispatcher.waitUntil([tab] { return !tab->search()->searching(); }));
    CHECK_EQ(tab->search()->matchCount(), std::size_t{3});
    s.search->updateSearchUi();
    CHECK(s.search->countText().ends_with("/ 3"));

    // Enter / next / previous step the active match and update the counter.
    Shell::press(s.search->field(), Key::Enter);
    CHECK(tab->search()->currentIndex() == std::optional<std::size_t>{0});
    CHECK_EQ(s.search->countText(), std::string("1 / 3"));
    Shell::press(s.search->field(), Key::Enter);
    CHECK_EQ(s.search->countText(), std::string("2 / 3"));
    tab->search()->previous();
    CHECK_EQ(s.search->countText(), std::string("1 / 3"));

    // Escape closes: focus drops, the bar hides, the search is cancelled.
    Shell::press(s.search->field(), Key::Escape);
    CHECK(!s.search->visible());
    CHECK(s.focused == nullptr);
    CHECK(!tab->search()->searching());
    CHECK(!s.search->handleEscape()); // nothing left to close
}

RIVET_TEST(wiringSearchBarClosesOnTabSwitchAndIgnoresBackgroundTabs) {
    Shell s;
    DocumentTab* first = s.open("search-a");
    s.search->toggle();
    s.typeInto(s.search->field(), "gamma");
    CHECK(s.dispatcher.waitUntil([first] { return !first->search()->searching(); }));

    DocumentTab* second = s.open("search-b");
    CHECK(second != first);
    CHECK(!s.search->visible()); // a different tab closes the bar
    s.search->updateSearchUi();
    const std::string before = s.search->countText();

    // The background tab's results never drive the bar.
    first->search()->start("alpha");
    CHECK(s.dispatcher.waitUntil([first] { return !first->search()->searching(); }));
    CHECK_EQ(s.search->countText(), before);
    CHECK_EQ(second->search()->query(), std::string(""));

    // Closing the bound active tab rebinds the neighbor; the closed session
    // outlives the rebind (the views cancel on it before it dies).
    s.workspace.closeActiveTab();
    CHECK(s.workspace.activeTab() == first);
    CHECK_EQ(s.status->pageCountText(), std::string("/ 3"));
}

RIVET_TEST(wiringStatusBarShowsPageIndicatorAndZoomText) {
    Shell s;
    s.status->updatePageIndicator();
    CHECK_EQ(s.status->pageCountText(), std::string("/ 0"));
    CHECK_EQ(s.status->pageField().text(), std::string(""));
    CHECK_EQ(s.status->statusText(), std::string("Ready"));

    DocumentTab* tab = s.open("status");
    CHECK_EQ(s.status->pageCountText(), std::string("/ 3"));
    CHECK_EQ(s.status->pageField().text(), std::string("1"));
    tab->setCurrentPage(2);
    s.status->updatePageIndicator();
    CHECK_EQ(s.status->pageField().text(), std::string("3"));

    // A focused field is not overwritten (the user is editing it).
    s.context->setFocus(&s.status->pageField());
    s.status->pageField().setText("2");
    tab->setCurrentPage(0);
    s.status->updatePageIndicator();
    CHECK_EQ(s.status->pageField().text(), std::string("2"));
    // Enter commits and returns focus to the viewport.
    Shell::press(s.status->pageField(), Key::Enter);
    CHECK(s.focused == nullptr);

    s.context->setStatus("Hello");
    CHECK_EQ(s.status->statusText(), std::string("Hello"));

    CHECK(StatusBarController::parsePageNumber("1", 3) == std::optional<std::size_t>{0});
    CHECK(StatusBarController::parsePageNumber("3", 3) == std::optional<std::size_t>{2});
    CHECK(!StatusBarController::parsePageNumber("0", 3).has_value());
    CHECK(!StatusBarController::parsePageNumber("4", 3).has_value());
    CHECK(!StatusBarController::parsePageNumber("-1", 3).has_value());
    CHECK(!StatusBarController::parsePageNumber(" 2", 3).has_value());
    CHECK(!StatusBarController::parsePageNumber("99999999999999999999999", 3).has_value());

    CHECK_EQ(rivet::app::zoomPercentText(1.0), std::string("100%"));
    CHECK_EQ(rivet::app::zoomPercentText(1.254), std::string("125%"));
    CHECK_EQ(rivet::app::zoomPercentText(0.5), std::string("50%"));
}

RIVET_TEST(wiringPasswordPromptRoutesSubmitAndCancel) {
    Shell s;
    s.engine.passwordRequired = true;
    DocumentTab* tab = s.open("locked");
    CHECK(tab->state() == DocumentTab::State::NeedsPassword);
    CHECK(s.password->visible());
    CHECK(s.focused == &s.password->field());

    // Escape only drops focus; the prompt stays for the locked tab.
    Shell::press(s.password->field(), Key::Escape);
    CHECK(s.focused == nullptr);
    CHECK(s.password->visible());
    CHECK(tab->state() == DocumentTab::State::NeedsPassword);

    // A wrong password: cleared before the retry, the prompt returns.
    s.typeInto(s.password->field(), "nope");
    Shell::press(s.password->field(), Key::Enter);
    CHECK_EQ(s.password->field().text(), std::string(""));
    CHECK(s.dispatcher.waitUntil([tab] { return tab->state() != DocumentTab::State::Loading; }));
    CHECK(tab->state() == DocumentTab::State::NeedsPassword);
    CHECK(s.password->visible());

    // The right password unlocks; the prompt hides and releases focus.
    s.typeInto(s.password->field(), "secret");
    s.password->submit();
    CHECK_EQ(s.password->field().text(), std::string(""));
    CHECK(s.dispatcher.waitUntil([tab] { return tab->state() != DocumentTab::State::Loading; }));
    CHECK(tab->state() == DocumentTab::State::Ready);
    CHECK(!s.password->visible());
    CHECK(s.focused != &s.password->field());

    // Submitting for a Ready tab is a no-op.
    s.password->field().setText("secret");
    s.password->submit();
    CHECK(tab->state() == DocumentTab::State::Ready);
    CHECK_EQ(s.password->field().text(), std::string("secret"));
}

RIVET_TEST(wiringTextInteractionIsNoOpWithoutReadyTab) {
    Shell s;
    // No tab at all.
    s.text->warmPage(0);
    CHECK(!s.text->charIndexAtPoint(0, rivet::core::Point{1.0, 1.0}).has_value());
    CHECK(s.text->overlayRects(0).empty());
    CHECK(!s.text->linkAtPoint(0, rivet::core::Point{1.0, 1.0}).has_value());
    CHECK(s.text->linkRects(0).empty());
    s.text->selectionDragBegan(0, 1, false);
    s.text->selectionDragMoved(0, 3);
    s.text->selectionCleared();
    s.text->copySelection();
    s.text->navigateInternalDestination(0, rivet::core::Point{}, false);
    CHECK_EQ(s.clipboard.writes, 0);
    CHECK(s.statusLog.empty());

    // A NeedsPassword tab is not Ready: still a no-op.
    s.engine.passwordRequired = true;
    DocumentTab* locked = s.open("text-locked");
    CHECK(locked->state() == DocumentTab::State::NeedsPassword);
    s.text->selectionDragBegan(0, 0, false);
    s.text->selectionDragMoved(0, 4);
    CHECK(locked->selection().empty());
    s.text->copySelection();
    CHECK_EQ(s.clipboard.writes, 0);
}

RIVET_TEST(wiringTextInteractionSelectsHighlightsAndCopiesAsync) {
    Shell s;
    DocumentTab* tab = s.open("text-ready");
    s.text->warmPage(0);
    CHECK(s.dispatcher.waitUntil([&] {
        return tab->session()->textService().cachedTextPage(tab->session()->pageId(0)) != nullptr;
    }));
    CHECK(s.text->charIndexAtPoint(0, rivet::core::Point{9.0, 105.0}) == std::optional<std::uint32_t>{1});

    s.text->selectionDragBegan(0, 0, false);
    s.text->selectionDragMoved(0, 5);
    CHECK(!tab->selection().empty());
    CHECK(!s.text->overlayRects(0).empty());
    CHECK(s.text->overlayRects(1).empty());

    // Copy is asynchronous: nothing reaches the clipboard until delivery.
    s.text->copySelection();
    CHECK(s.dispatcher.waitUntil([&] { return s.clipboard.writes > 0; }));
    CHECK_EQ(s.clipboard.text(), std::string("alpha"));
    CHECK_EQ(s.statusLog.back(), std::string("Copied 5 characters"));

    s.text->selectionCleared();
    CHECK(tab->selection().empty());
    CHECK(s.text->overlayRects(0).empty());
}

RIVET_TEST(wiringSidebarSwitchesModesAndLoadsOutlineAsync) {
    Shell s;
    CHECK(s.sidebar->mode() == SidebarController::Mode::Pages);
    // The sidebar container is root's first child: [Pages, Outline, ...].
    auto* container = dynamic_cast<rivet::ui::Container*>(s.root->children().front().get());
    CHECK(container != nullptr);
    auto* pagesButton = dynamic_cast<rivet::ui::Button*>(container->children()[0].get());
    auto* outlineButton = dynamic_cast<rivet::ui::Button*>(container->children()[1].get());
    CHECK(pagesButton != nullptr && outlineButton != nullptr);
    CHECK_EQ(pagesButton->label(), std::string("Pages •"));
    CHECK_EQ(outlineButton->label(), std::string("Outline"));

    s.sidebar->setMode(SidebarController::Mode::Outline);
    CHECK(s.sidebar->mode() == SidebarController::Mode::Outline);
    CHECK_EQ(pagesButton->label(), std::string("Pages"));
    CHECK_EQ(outlineButton->label(), std::string("Outline •"));
    CHECK(s.sidebar->outlineRows().empty());

    // The outline arrives asynchronously; the top level starts expanded.
    s.open("outline");
    CHECK(s.dispatcher.waitUntil([&] { return !s.sidebar->outlineRows().empty(); }));
    CHECK_EQ(s.sidebar->outlineRows().size(), std::size_t{3});
    CHECK_EQ(s.sidebar->outlineRows()[1].title, std::string("Section 1.1"));

    // Collapsing a top-level node sticks (it is not re-expanded).
    s.sidebar->setRowExpanded(0, false);
    CHECK_EQ(s.sidebar->outlineRows().size(), std::size_t{2});
    s.sidebar->setRowExpanded(0, true);
    CHECK_EQ(s.sidebar->outlineRows().size(), std::size_t{3});
    s.sidebar->activateRow(99); // out of range: ignored

    s.sidebar->setMode(SidebarController::Mode::Pages);
    CHECK(s.sidebar->mode() == SidebarController::Mode::Pages);

    // Closing the only (bound) tab clears the outline.
    s.workspace.closeActiveTab();
    CHECK(s.sidebar->outlineRows().empty());
}

RIVET_TEST(wiringFlattenOutlineHonorsExpansionAndClampsDestinations) {
    PdfOutlineNode root;
    root.children.push_back(outlineNode("A", 0, {outlineNode("A.1", 7), outlineNode("A.2", 1)}));
    root.children.push_back(outlineNode("B", 1));

    const FlattenedOutline collapsed = rivet::app::flattenOutline(root, {}, 2);
    CHECK_EQ(collapsed.rows.size(), std::size_t{2});
    CHECK(collapsed.rows[0].hasChildren);
    CHECK(!collapsed.rows[0].expanded);

    const FlattenedOutline expanded = rivet::app::flattenOutline(root, {OutlinePath{0}}, 2);
    CHECK_EQ(expanded.rows.size(), std::size_t{4});
    CHECK_EQ(expanded.rows[1].depth, 2);
    CHECK(expanded.paths[2] == (OutlinePath{0, 1}));
    CHECK_EQ(expanded.destinations[1], std::size_t{0}); // page 7 >= 2 pages
    CHECK_EQ(expanded.destinations[2], std::size_t{1});
}
