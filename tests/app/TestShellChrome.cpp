// SPDX-License-Identifier: MPL-2.0
// ADR-0033 shell chrome: recent files store, welcome view, tool modes and
// their context bars, sidebar visibility / width, the overflow menu and the
// toolbar's Tab keyboard navigation. Fake engine, deterministic dispatcher.
#include "RivetTest.h"

#include "fakes/FakeWritableEngine.hpp"

#include "app/RecentFiles.hpp"
#include "app/ShellController.hpp"
#include "app/WelcomeView.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "platform/AppDataLocation.hpp"
#include "platform/PlatformKit.hpp"
#include "render/ZoomState.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/PopupMenu.hpp"
#include "ui/Theme.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace rivet::app;

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("rivet-chrome-" + std::to_string(++counter));
        fs::remove_all(path_);
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path operator()(const std::string& name) const { return path_ / name; }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

void writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

class TempAppData final : public rivet::platform::IAppDataLocation {
public:
    explicit TempAppData(fs::path dir) : dir_(std::move(dir)) {}
    rivet::core::Result<fs::path> directory() const override { return dir_; }

private:
    fs::path dir_;
};

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

struct Shell {
    TempDir dir;
    TempAppData appData{dir.path()};
    WaitDispatcher dispatcher;
    rivet::platform::ShellServices services;
    std::unique_ptr<ShellController> shell;

    Shell() {
        services.mainDispatcher = &dispatcher;
        services.appData = &appData;
        shell = ShellController::createWithEngine(services, std::make_unique<rivet::test::FakeWritableEngine>());
        shell->rootWidget().setFrame(rivet::core::Rect{0, 0, 1100, 760});
    }
    bool openPdf(const std::string& name) {
        writeFile(dir(name), "placeholder\n");
        const std::size_t before = shell->workspace().tabCount();
        shell->openDocument(dir(name));
        return dispatcher.waitUntil([&] {
            DocumentTab* tab = shell->workspace().activeTab();
            return shell->workspace().tabCount() == before + 1 && tab != nullptr &&
                   tab->state() == DocumentTab::State::Ready;
        });
    }
};

} // namespace

RIVET_TEST(recentFilesDedupeCapAndPersist) {
    TempDir dir;
    TempAppData appData(dir.path());
    {
        RecentFiles recent(&appData);
        CHECK(recent.entries().empty());
        for (int i = 0; i < 12; ++i) recent.add(dir("f" + std::to_string(i) + ".pdf"));
        CHECK(recent.entries().size() == RecentFiles::kMaxEntries);
        CHECK(recent.entries().front() == dir("f11.pdf"));
        recent.add(dir("f5.pdf")); // moves to the front, no duplicate
        CHECK(recent.entries().front() == dir("f5.pdf"));
        CHECK(recent.entries().size() == RecentFiles::kMaxEntries);
    }
    RecentFiles reloaded(&appData);
    CHECK(reloaded.entries().size() == RecentFiles::kMaxEntries);
    CHECK(reloaded.entries().front() == dir("f5.pdf"));
    reloaded.clear();
    CHECK(RecentFiles(&appData).entries().empty());

    RecentFiles memoryOnly(nullptr);
    memoryOnly.add(dir("a.pdf"));
    CHECK(memoryOnly.entries().size() == 1);
    CHECK(memoryOnly.storePath().empty());
}

RIVET_TEST(welcomeViewGreysMissingAndOpensExistingOnly) {
    TempDir dir;
    writeFile(dir("here.pdf"), "x");
    WelcomeView view;
    view.setFrame(rivet::core::Rect{0, 0, 800, 600});
    std::vector<fs::path> opened;
    view.setOnOpenRecent([&](const fs::path& path) { opened.push_back(path); });
    view.setRecentFiles({dir("here.pdf"), dir("gone.pdf")});
    view.layout();
    CHECK(view.recentCount() == 2);
    CHECK(view.recentExists(0));
    CHECK(!view.recentExists(1));

    const auto click = [&](std::size_t index) {
        const rivet::core::Rect row = view.recentRowRect(index);
        rivet::ui::PointerEvent down;
        down.type = rivet::ui::PointerEventType::Down;
        down.button = 1;
        down.position = rivet::core::Point{row.origin.x + 10.0, row.origin.y + row.size.height / 2.0};
        view.onMouse(down);
        rivet::ui::PointerEvent up = down;
        up.type = rivet::ui::PointerEventType::Up;
        view.onMouse(up);
    };
    click(1);
    CHECK(opened.empty());
    click(0);
    CHECK(opened.size() == 1 && opened.front() == dir("here.pdf"));
}

RIVET_TEST(shellShowsWelcomeUntilATabOpensAndRecordsRecents) {
    Shell s;
    CHECK(!s.shell->welcomeView().frame().isEmpty());
    CHECK(s.shell->pdfViewport().frame().isEmpty());
    CHECK(s.openPdf("doc.pdf"));
    CHECK(s.shell->welcomeView().frame().isEmpty());
    CHECK(!s.shell->pdfViewport().frame().isEmpty());
    CHECK(!s.shell->recentFiles().entries().empty());
    CHECK(s.shell->recentFiles().entries().front() == s.dir("doc.pdf"));
    CHECK(s.shell->welcomeView().recentCount() == 1);
}

RIVET_TEST(shellToolModesSwitchExclusivelyAndShowContextBars) {
    Shell s;
    CHECK(s.openPdf("doc.pdf"));
    CHECK(s.shell->toolMode() == ToolMode::View);
    const double viewTop = s.shell->pdfViewport().frame().origin.y;

    s.shell->setToolMode(ToolMode::Annotate);
    CHECK(s.shell->toolMode() == ToolMode::Annotate);
    CHECK(s.shell->pdfViewport().frame().origin.y > viewTop); // context bar under the toolbar

    s.shell->setToolMode(ToolMode::Edit);
    CHECK(s.shell->toolMode() == ToolMode::Edit);
    CHECK(s.shell->pdfViewport().frame().origin.y > viewTop);

    s.shell->setToolMode(ToolMode::Forms);
    CHECK(s.shell->toolMode() == ToolMode::Forms);
    CHECK(s.shell->pdfViewport().frame().origin.y > viewTop);

    s.shell->setToolMode(ToolMode::View);
    CHECK(s.shell->toolMode() == ToolMode::View);
    CHECK(s.shell->pdfViewport().frame().origin.y == viewTop);
}

RIVET_TEST(shellSidebarToggleAndClampedWidth) {
    Shell s;
    CHECK(s.openPdf("doc.pdf"));
    const double withSidebar = s.shell->pdfViewport().frame().origin.x;
    CHECK(withSidebar == rivet::ui::metrics::kSidebarWidth);
    s.shell->setSidebarWidth(10.0);
    CHECK(s.shell->sidebarWidth() == rivet::ui::metrics::kSidebarMinWidth);
    s.shell->setSidebarWidth(5000.0);
    CHECK(s.shell->sidebarWidth() == rivet::ui::metrics::kSidebarMaxWidth);
    CHECK(s.shell->pdfViewport().frame().origin.x == rivet::ui::metrics::kSidebarMaxWidth);
    s.shell->setSidebarVisible(false);
    CHECK(s.shell->pdfViewport().frame().origin.x == 0.0);
    s.shell->setSidebarVisible(true);
    CHECK(s.shell->pdfViewport().frame().origin.x == rivet::ui::metrics::kSidebarMaxWidth);
}

RIVET_TEST(shellMarkdownTabGetsFormatContextBarAndNoSidebar) {
    Shell s;
    writeFile(s.dir("note.md"), "# Title\n\ntext\n");
    s.shell->openDocument(s.dir("note.md"));
    CHECK(s.dispatcher.waitUntil([&] {
        DocumentTab* tab = s.shell->workspace().activeTab();
        return tab != nullptr && tab->state() == DocumentTab::State::Ready;
    }));
    const rivet::core::Rect host = s.shell->markdownHost().frame();
    using namespace rivet::ui::metrics;
    CHECK(host.origin.x == 0.0);
    CHECK(host.origin.y == kTabStripHeight + kToolbarHeight + kContextBarHeight);
}

RIVET_TEST(newTabOpensFitWidthAndKeepsAnExplicitZoomPerTab) {
    Shell s;
    using FitMode = rivet::render::ZoomState::FitMode;
    CHECK(s.openPdf("a.pdf"));
    CHECK(s.shell->pdfViewport().zoom().fitMode() == FitMode::Width);

    // The user zooms explicitly in tab A; a new tab B opens fit-width.
    CHECK(s.shell->pdfViewport().zoomInStep());
    const double zoomA = s.shell->pdfViewport().zoom().zoom();
    CHECK(s.shell->pdfViewport().zoom().fitMode() == FitMode::None);
    CHECK(s.openPdf("b.pdf"));
    CHECK(s.shell->pdfViewport().zoom().fitMode() == FitMode::Width);

    // Back on tab A the explicit zoom is remembered (no re-fit).
    s.shell->workspace().activateTab(0);
    CHECK(s.shell->pdfViewport().zoom().fitMode() == FitMode::None);
    CHECK(s.shell->pdfViewport().zoom().zoom() == zoomA);
}

RIVET_TEST(viewMenuZoomCommandsFollowTheActiveTab) {
    Shell s;
    using FitMode = rivet::render::ZoomState::FitMode;
    const auto cmd = [](ViewCommand c) { return AppCommand::of(c); };
    // No document: disabled.
    CHECK(!s.shell->canPerform(cmd(ViewCommand::ZoomIn)));
    CHECK(s.openPdf("a.pdf"));
    for (ViewCommand c : {ViewCommand::ZoomIn, ViewCommand::ZoomOut, ViewCommand::ActualSize, ViewCommand::FitWidth,
                          ViewCommand::FitPage}) {
        CHECK(s.shell->canPerform(cmd(c)));
    }
    auto& zoom = s.shell->pdfViewport().zoom();
    s.shell->perform(cmd(ViewCommand::ActualSize));
    CHECK(zoom.zoom() == 1.0 && zoom.fitMode() == FitMode::None);
    s.shell->perform(cmd(ViewCommand::ZoomIn));
    CHECK(zoom.zoom() > 1.0);
    s.shell->perform(cmd(ViewCommand::ZoomOut));
    CHECK(zoom.zoom() == 1.0);
    s.shell->perform(cmd(ViewCommand::FitPage));
    CHECK(zoom.fitMode() == FitMode::Page);
    s.shell->perform(cmd(ViewCommand::FitWidth));
    CHECK(zoom.fitMode() == FitMode::Width);

    // Markdown tabs have no zoom mechanism: disabled.
    writeFile(s.dir("note.md"), "# Title\n");
    s.shell->openDocument(s.dir("note.md"));
    CHECK(s.dispatcher.waitUntil([&] {
        DocumentTab* tab = s.shell->workspace().activeTab();
        return tab != nullptr && tab->isMarkdown() && tab->state() == DocumentTab::State::Ready;
    }));
    CHECK(!s.shell->canPerform(cmd(ViewCommand::ZoomIn)));
}

RIVET_TEST(shellOverflowMenuMirrorsTheMenuBar) {
    Shell s;
    CHECK(s.openPdf("doc.pdf"));
    s.shell->openOverflowMenu();
    rivet::ui::PopupMenu& menu = s.shell->popupMenu();
    CHECK(menu.isOpen());
    CHECK(!menu.items().empty());
    bool foundFile = false;
    for (const auto& item : menu.items()) {
        if (item.title == "File") foundFile = !item.submenu.empty();
    }
    CHECK(foundFile);
    // Escape closes it; keys go to the menu while it is open.
    rivet::ui::KeyEvent escape;
    escape.key = rivet::ui::Key::Escape;
    CHECK(s.shell->handleKeyEvent(escape));
    CHECK(!menu.isOpen());
}
