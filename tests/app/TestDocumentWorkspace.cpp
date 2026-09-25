// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "app/DocumentWorkspace.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfTypes.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using rivet::app::DocumentTab;
using rivet::app::DocumentWorkspace;
using rivet::core::Bitmap;
using rivet::core::DocumentId;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::PageId;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentSession;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfPageInfo;

namespace {

// Minimal PdfDocument: 2 pages {300, 400}; renderPage makes a tiny bitmap.
class FakePdfDocument final : public PdfDocument {
public:
    FakePdfDocument() {
        info_.pageCount = 2;
        info_.isEncrypted = false;
        info_.title = "fake";
    }

    const PdfDocumentInfo& info() const override { return info_; }

    Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const override {
        if (pageIndex >= 2) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "bad page", "test"});
        }
        return PdfPageInfo{pageIndex, Size{300.0, 400.0}, rivet::core::PageRotation::None};
    }

    Result<Bitmap> renderPage(std::size_t, const Rect&, double) override {
        return Bitmap::create(4, 4);
    }

private:
    PdfDocumentInfo info_;
};

// Scriptable engine: fails when shouldFail is set (recorded error path).
class FakePdfEngine final : public PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "fake"; }

    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path& path,
                                                      std::string_view password) override {
        std::error_code ec;
        // Like a real backend: a missing file is an I/O error.
        if (!std::filesystem::exists(path, ec)) {
            return std::unexpected(Error{ErrorCode::Io, "no such file", "test"});
        }
        if (failOpens) {
            return std::unexpected(Error{ErrorCode::InvalidDocument, "broken pdf", "test"});
        }
        if (passwordRequired && password.empty()) {
            return std::unexpected(Error{ErrorCode::PasswordRequired, "password protected", "test"});
        }
        return std::unique_ptr<PdfDocument>(std::make_unique<FakePdfDocument>());
    }

    bool failOpens = false;
    bool passwordRequired = false;
};

// Main-thread dispatcher: ALWAYS defers into a queue that the test thread
// pumps, modeling the real contract that completions arrive on the main
// thread (running them inline on a worker would be a data race).
class TestDispatcher final : public rivet::core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push_back(std::move(task));
    }

    // Runs every queued task on the calling (test) thread.
    void pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex);
            run.swap(queue);
        }
        for (auto& task : run) task();
    }

    std::mutex mutex;
    std::deque<std::function<void()>> queue;
};

struct Fixture {
    FakePdfEngine engine;
    TaskScheduler scheduler{2};
    TestDispatcher dispatcher;
    DocumentWorkspace workspace;

    Fixture() : workspace(engine, scheduler, &dispatcher) {}
};

// Temporary fixture PDFs; each test uses its own path.
std::filesystem::path tempPdf(const char* name) {
    auto path = std::filesystem::temp_directory_path() /
                std::filesystem::path{std::string{"rivet-ws-"} + name + ".pdf"};
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    std::fputs("%PDF-1.4\n", file);
    std::fclose(file);
    return path;
}

} // namespace

RIVET_TEST(workspaceOpenActivatesLoadingTabThenAttachesSession) {
    Fixture f;
    const std::filesystem::path path = tempPdf("open");

    std::size_t tabsChanged = 0;
    std::size_t activeChanged = 0;
    f.workspace.setOnTabsChanged([&tabsChanged] { ++tabsChanged; });
    f.workspace.setOnActiveTabChanged([&activeChanged] { ++activeChanged; });

    f.workspace.openDocument(path);
    CHECK_EQ(f.workspace.tabCount(), std::size_t{1});
    CHECK_EQ(f.workspace.activeIndex(), std::size_t{0});
    CHECK(f.workspace.activeTab() != nullptr);
    CHECK(f.workspace.activeTab()->state() == DocumentTab::State::Loading);
    CHECK_EQ(f.workspace.activeTab()->session(), nullptr);
    CHECK_EQ(f.workspace.activeTab()->title(), path.filename().string());
    CHECK_GE(tabsChanged, std::size_t{1});
    CHECK_GE(activeChanged, std::size_t{1});

    // Drain the open pipeline: worker task -> queued completion -> attach.
    // The worker may need a moment to post its completion.
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();

    CHECK(f.workspace.activeTab()->state() == DocumentTab::State::Ready);
    CHECK(f.workspace.activeTab()->session() != nullptr);
    CHECK_EQ(f.workspace.activeTab()->session()->pageCount(), std::size_t{2});
}

RIVET_TEST(workspaceOpenFailureEntersErrorState) {
    Fixture f;
    f.engine.failOpens = true;
    f.workspace.openDocument(tempPdf("fail"));
    // Pump until the completion has run (the worker thread needs to finish
    // the background open first).
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();

    CHECK(f.workspace.activeTab()->state() == DocumentTab::State::Error);
    CHECK_EQ(f.workspace.activeTab()->session(), nullptr);
    CHECK(!f.workspace.activeTab()->errorText().empty());
}

RIVET_TEST(workspaceDeduplicatesPathsAndActivatesExistingTab) {
    Fixture f;
    const std::filesystem::path path = tempPdf("dedup");
    f.workspace.openDocument(path);
    // Pump until the completion has run (the worker thread needs to finish
    // the background open first).
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();
    CHECK_EQ(f.workspace.tabCount(), std::size_t{1});

    // Same file through a lexically different path: no second tab.
    const std::filesystem::path doubled = path / std::filesystem::path{".."} / path.filename();
    f.workspace.openDocument(doubled);
    CHECK_EQ(f.workspace.tabCount(), std::size_t{1});
    CHECK_EQ(f.workspace.activeIndex(), std::size_t{0});

    // A different file opens a second tab and becomes active.
    const std::filesystem::path other = tempPdf("other");
    f.workspace.openDocument(other);
    CHECK_EQ(f.workspace.tabCount(), std::size_t{2});
    CHECK_EQ(f.workspace.activeIndex(), std::size_t{1});
}

RIVET_TEST(workspaceSwitchTabsPreservesViewStatePerTab) {
    Fixture f;
    const std::filesystem::path a = tempPdf("tab-a");
    const std::filesystem::path b = tempPdf("tab-b");
    f.workspace.openDocument(a);
    // Pump until the completion has run (the worker thread needs to finish
    // the background open first).
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();
    f.workspace.openDocument(b);
    // Pump until the completion has run (the worker thread needs to finish
    // the background open first).
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();

    // Mutate tab 1's view state.
    f.workspace.activeTab()->viewState().zoom().setZoom(2.0);
    f.workspace.activeTab()->viewState().setScrollOffsetPoints(rivet::core::Point{0.0, 77.0});
    f.workspace.activeTab()->setCurrentPage(1);

    // Switch to tab 0: its state is untouched (fresh defaults).
    f.workspace.activateTab(0);
    CHECK_NEAR(f.workspace.activeTab()->viewState().zoom().zoom(), 1.0, 1e-12);
    CHECK_NEAR(f.workspace.activeTab()->viewState().scrollOffsetPoints().y, 0.0, 1e-12);

    // Switch back: tab 1's state survived the round trip.
    f.workspace.activateTab(1);
    CHECK_NEAR(f.workspace.activeTab()->viewState().zoom().zoom(), 2.0, 1e-12);
    CHECK_NEAR(f.workspace.activeTab()->viewState().scrollOffsetPoints().y, 77.0, 1e-12);
    CHECK_EQ(f.workspace.activeTab()->currentPage(), std::size_t{1});
}

RIVET_TEST(workspaceCloseActiveTabActivatesNeighbor) {
    Fixture f;
    f.workspace.openDocument(tempPdf("c0"));
    f.workspace.openDocument(tempPdf("c1"));
    f.workspace.openDocument(tempPdf("c2"));
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();
    CHECK_EQ(f.workspace.activeIndex(), std::size_t{2});

    // Close the active (last) tab: the previous tab becomes active.
    f.workspace.closeActiveTab();
    CHECK_EQ(f.workspace.tabCount(), std::size_t{2});
    CHECK_EQ(f.workspace.activeIndex(), std::size_t{1});

    // Close a tab BEFORE the active one: active identity preserved.
    f.workspace.closeTab(0);
    CHECK_EQ(f.workspace.tabCount(), std::size_t{1});
    CHECK_EQ(f.workspace.activeIndex(), std::size_t{0});
    CHECK_EQ(f.workspace.activeTab()->title(), tempPdf("c1").filename().string());

    // Close the final tab: empty state, active = kNoTab.
    f.workspace.closeActiveTab();
    CHECK(f.workspace.isEmpty());
    CHECK_EQ(f.workspace.activeIndex(), DocumentWorkspace::kNoTab);
    CHECK_EQ(f.workspace.activeTab(), nullptr);
}

RIVET_TEST(workspaceTabClosedDuringLoadDropsTheSession) {
    Fixture f;
    f.workspace.openDocument(tempPdf("cancel"));

    // Close the loading tab before the completion runs.
    f.workspace.closeTab(0);
    CHECK(f.workspace.isEmpty());

    // Release the pipeline: the completion must not resurrect anything.
    for (int i = 0; i < 500; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(f.workspace.isEmpty());

    // A fresh open still works afterwards.
    f.workspace.openDocument(tempPdf("after"));
    CHECK_EQ(f.workspace.tabCount(), std::size_t{1});
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();
    CHECK(f.workspace.activeTab()->state() == DocumentTab::State::Ready);
}

RIVET_TEST(workspaceEmptyStateTransitionsAndOutsideOpenFailure) {
    Fixture f;
    f.workspace.openDocument("/definitely/not/a/real/file.pdf");
    // Pump until the completion has run (the worker thread needs to finish
    // the background open first).
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();
    CHECK(f.workspace.activeTab()->state() == DocumentTab::State::Error);
    // The view state is still there for the error view.
    (void)f.workspace.activeTab()->viewState();
}

RIVET_TEST(workspacePasswordFlowPromptsAndRetries) {
    Fixture f;
    f.engine.passwordRequired = true;
    const std::filesystem::path path = tempPdf("locked");
    f.workspace.openDocument(path);

    // Wait for the open to complete (fake engine fails with PasswordRequired).
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();
    CHECK(f.workspace.activeTab()->state() == DocumentTab::State::NeedsPassword);
    CHECK_EQ(f.workspace.activeTab()->session(), nullptr);

    // Retry with a password: the fake engine then opens the document.
    f.workspace.retryWithPassword(f.workspace.activeIndex(), "secret");
    for (int i = 0; i < 500 && f.workspace.activeTab()->state() == DocumentTab::State::Loading; ++i) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    f.dispatcher.pump();
    CHECK(f.workspace.activeTab()->state() == DocumentTab::State::Ready);
    CHECK(f.workspace.activeTab()->session() != nullptr);
}
