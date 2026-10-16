// SPDX-License-Identifier: MPL-2.0
// Document comparison tab (ADR-0029): the menu command, the tab payload and
// its async lifecycle, the host view (highlights, change navigation, scroll
// sync) and File > Compare With... through the shell. The failure-path and
// menu bodies run everywhere; the bodies that compare two generated PDFs run
// only when PDFium is built in (early return otherwise).
#include "RivetTest.h"

#include "app/ComparisonHostView.hpp"
#include "app/ComparisonTabState.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/MenuModel.hpp"
#include "app/ShellController.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfBuild.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"
#include "platform/PlatformKit.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef RIVET_APP_PDF_FIXTURE_DIR
#define RIVET_APP_PDF_FIXTURE_DIR "tests/pdf/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
using namespace rivet;
using rivet::app::ComparisonHostView;
using rivet::app::ComparisonTabState;
using rivet::app::DocumentTab;
using rivet::app::DocumentWorkspace;
using rivet::app::FileCommand;

class WaitDispatcher final : public core::IMainThreadDispatcher {
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
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

class FakeOpenDialog final : public platform::IFileDialog {
public:
    core::Result<fs::path> openPdf() override { return next; }
    fs::path next;
};

class StringSink final : public pdf::IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        bytes.append(static_cast<const char*>(data), size);
        return core::ok();
    }
    std::string bytes;
};

pdf::PdfBuildPage textPage(const std::vector<std::string>& lines) {
    pdf::PdfBuildPage page;
    double y = 700.0;
    for (const std::string& line : lines) {
        pdf::PdfBuildText text;
        text.fontSize = 14.0;
        text.origin = pdf::PdfPoint{72.0, y};
        text.utf8 = line;
        page.items.push_back(text);
        y -= 40.0;
    }
    return page;
}

fs::path writePdf(pdf::PdfEngine& engine, const std::string& name,
                  const std::vector<std::vector<std::string>>& pages) {
    pdf::PdfBuildDocument document;
    for (const auto& lines : pages) document.pages.push_back(textPage(lines));
    StringSink sink;
    CHECK(engine.buildDocument(document, sink).has_value());
    const fs::path path = fs::temp_directory_path() / name;
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    CHECK(file != nullptr);
    if (file != nullptr) {
        std::fwrite(sink.bytes.data(), 1, sink.bytes.size(), file);
        std::fclose(file);
    }
    return path;
}

// Two generated PDFs: one word differs on page 1, the left one has a third page.
struct Pair {
    std::unique_ptr<pdf::PdfEngine> builder;
    fs::path left;
    fs::path right;
};

bool makePair(Pair& pair) {
    pair.builder = pdf::createEngine();
    if (pair.builder == nullptr || !pair.builder->isAvailable()) return false;
    // The first open initializes the library (building text needs it).
    (void)pair.builder->openDocument(fs::path(RIVET_APP_PDF_FIXTURE_DIR) / "alpha.pdf");
    pair.left = writePdf(*pair.builder, "rivet-cmp-left.pdf",
                         {{"The quick brown fox", "jumps over"}, {"unchanged page"}, {"only left"}});
    pair.right = writePdf(*pair.builder, "rivet-cmp-right.pdf",
                          {{"The quick red fox", "jumps over"}, {"unchanged page"}});
    return true;
}

} // namespace

RIVET_TEST(compareWithIsInTheFileMenu) {
    bool found = false;
    for (const app::Menu& menu : app::buildMenuBar()) {
        if (menu.title != "File") continue;
        for (const app::MenuItem& entry : menu.items) {
            if (entry.command == app::AppCommand::of(FileCommand::Compare)) found = true;
        }
    }
    CHECK(found);
}

RIVET_TEST(comparisonOfMissingFilesFailsCleanlyAndTabLifecycleHolds) {
    std::unique_ptr<pdf::PdfEngine> engine = pdf::createEngine();
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    DocumentWorkspace workspace(*engine, scheduler, &dispatcher);

    auto state = ComparisonTabState::start({engine.get(), &scheduler, &dispatcher},
                                           "/nonexistent/rivet-left.pdf", "/nonexistent/rivet-right.pdf");
    ComparisonTabState* raw = state.get();
    DocumentTab* tab = workspace.openComparisonTab("a vs b", std::move(state));
    CHECK(tab != nullptr);
    CHECK(tab->isComparison());
    CHECK(tab->kind() == app::DocumentKind::Comparison);
    CHECK(tab->state() == DocumentTab::State::Ready);
    CHECK(tab->comparison() == raw);
    CHECK(tab->session() == nullptr);
    CHECK(!tab->canSave());
    CHECK(!tab->isDirty());
    CHECK_EQ(workspace.indexOfPath("/nonexistent/rivet-left.pdf"), DocumentWorkspace::kNoTab);

    ComparisonHostView host({});
    host.setFrame(core::Rect{0, 0, 1000, 600});
    raw->setOnChanged([&host] { host.refresh(); });
    host.bind(raw);
    CHECK(dispatcher.waitUntil([raw] { return raw->phase() == ComparisonTabState::Phase::Failed; }));
    CHECK(!raw->errorText().empty());
    CHECK(raw->result() == nullptr);
    CHECK(host.overlaysFor(true, 0).empty());
    host.nextChange(); // no result: no-op
    CHECK(!host.selectedChange().has_value());

    host.bind(nullptr);
    workspace.closeTab(0);
    CHECK(workspace.isEmpty());
}

RIVET_TEST(comparisonHighlightsNavigationAndScrollSyncOverGeneratedPdfs) {
    Pair pair;
    if (!makePair(pair)) return;

    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    auto state = ComparisonTabState::start({pair.builder.get(), &scheduler, &dispatcher}, pair.left, pair.right);
    ComparisonHostView host({});
    host.setFrame(core::Rect{0, 0, 1200, 800});
    state->setOnChanged([&host] { host.refresh(); });
    host.bind(state.get());
    CHECK(dispatcher.waitUntil([&] { return state->phase() == ComparisonTabState::Phase::Ready; }));
    if (state->phase() != ComparisonTabState::Phase::Ready) return;

    const editor::ComparisonResult* result = state->result();
    CHECK(result != nullptr);
    if (result == nullptr) return;
    CHECK_EQ(result->changes.size(), std::size_t{2});
    CHECK_EQ(host.changeList().rows().size(), std::size_t{2});

    // Word highlights: one deleted box on the left, one inserted box on the
    // right of page 1; the extra left page is tinted as a whole; the common
    // unchanged page has nothing.
    const auto left0 = host.overlaysFor(true, 0);
    const auto right0 = host.overlaysFor(false, 0);
    CHECK_EQ(left0.size(), std::size_t{1});
    CHECK_EQ(right0.size(), std::size_t{1});
    CHECK_GT(left0[0].color.r, left0[0].color.g); // red
    CHECK_GT(right0[0].color.g, right0[0].color.r); // green
    CHECK(left0[0].color.a < 1.0);
    CHECK_EQ(host.overlaysFor(true, 1).size(), std::size_t{0});
    CHECK_EQ(host.overlaysFor(false, 1).size(), std::size_t{0});
    CHECK_EQ(host.overlaysFor(true, 2).size(), std::size_t{1});

    // Navigation wraps and strengthens the selected highlight.
    host.nextChange();
    CHECK(host.selectedChange() == std::optional<std::size_t>{0});
    CHECK_EQ(host.overlaysFor(true, 0).size(), std::size_t{2});
    host.nextChange();
    CHECK(host.selectedChange() == std::optional<std::size_t>{1});
    CHECK_EQ(host.overlaysFor(true, 2).size(), std::size_t{2});
    host.nextChange();
    CHECK(host.selectedChange() == std::optional<std::size_t>{0});
    host.previousChange();
    CHECK(host.selectedChange() == std::optional<std::size_t>{1});

    // Scrolling one side moves the other to the same page position.
    host.leftViewport().setScrollOffsetPoints(core::Point{0.0, 300.0});
    CHECK_NEAR(host.rightViewport().scrollOffsetPoints().y, host.leftViewport().scrollOffsetPoints().y, 1.0);
    host.rightViewport().setScrollOffsetPoints(core::Point{0.0, 60.0});
    CHECK_NEAR(host.leftViewport().scrollOffsetPoints().y, host.rightViewport().scrollOffsetPoints().y, 1.0);

    host.bind(nullptr);
}

RIVET_TEST(compareWithOpensAComparisonTabAndClosingReleasesIt) {
    Pair pair;
    if (!makePair(pair)) return;

    WaitDispatcher dispatcher;
    FakeOpenDialog dialog;
    platform::ShellServices services;
    services.mainDispatcher = &dispatcher;
    services.fileDialog = &dialog;
    auto shell = app::ShellController::createWithEngine(services, pdf::createEngine());
    shell->rootWidget().setFrame(core::Rect{0, 0, 1200, 800});

    CHECK(!shell->canPerformFile(FileCommand::Compare)); // nothing open
    shell->openDocument(pair.left);
    CHECK(dispatcher.waitUntil([&] {
        DocumentTab* tab = shell->workspace().activeTab();
        return tab != nullptr && tab->state() == DocumentTab::State::Ready;
    }));
    CHECK(shell->canPerformFile(FileCommand::Compare));

    dialog.next = pair.right;
    shell->performFile(FileCommand::Compare);
    CHECK_EQ(shell->workspace().tabCount(), std::size_t{2});
    DocumentTab* tab = shell->workspace().activeTab();
    CHECK(tab != nullptr && tab->isComparison());
    if (tab == nullptr || !tab->isComparison()) return;
    CHECK(shell->comparisonChromeActive());
    CHECK(shell->pdfViewport().frame().isEmpty());
    CHECK(!shell->comparisonHost().frame().isEmpty());
    CHECK(!shell->canPerformFile(FileCommand::Compare)); // PDF tabs only
    CHECK(!shell->canPerformFile(FileCommand::Save));
    CHECK(dispatcher.waitUntil([&] { return tab->comparison()->phase() == ComparisonTabState::Phase::Ready; }));
    CHECK(shell->comparisonHost().state() == tab->comparison());
    CHECK_EQ(shell->comparisonHost().changeList().rows().size(), std::size_t{2});

    // Switching away and back keeps the view consistent.
    shell->workspace().activateTab(0);
    dispatcher.pump();
    CHECK(!shell->comparisonChromeActive());
    CHECK(shell->comparisonHost().state() == nullptr);
    CHECK(!shell->pdfViewport().frame().isEmpty());
    CHECK(shell->canPerformFile(FileCommand::Compare));
    shell->workspace().activateTab(1);
    dispatcher.pump();
    CHECK(shell->comparisonChromeActive());
    CHECK(shell->comparisonHost().state() != nullptr);

    // Closing the tab destroys the state (both documents released).
    shell->workspace().closeTab(1);
    dispatcher.pump();
    CHECK_EQ(shell->workspace().tabCount(), std::size_t{1});
    CHECK(shell->comparisonHost().state() == nullptr);
    CHECK(!shell->comparisonChromeActive());
}

RIVET_TEST(closingAComparisonTabWhileItIsStillOpeningIsSafe) {
    Pair pair;
    if (!makePair(pair)) return;
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    for (int round = 0; round < 5; ++round) {
        auto state =
            ComparisonTabState::start({pair.builder.get(), &scheduler, &dispatcher}, pair.left, pair.right);
        state.reset(); // cancel + wait + release, whatever stage it reached
        dispatcher.pump();
    }
    CHECK(true);
}
