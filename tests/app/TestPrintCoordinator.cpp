// SPDX-License-Identifier: MPL-2.0
// PrintCoordinator at the application layer:
//   - the tab may be closed while the (modal) print panel is up: print()
//     re-resolves the tab by id afterwards instead of using a dangling
//     session;
//   - a document with annotation edits is printed from ONE assembled
//     document (assembled on the worker, reopened, rendered from, temp file
//     removed on success, failure and cancel); a document without edits is
//     rendered from its sources and never assembled.
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"
#include "fakes/FakePageDocument.hpp"
#include "fakes/FakeWritableEngine.hpp"

#include "app/DocumentWorkspace.hpp"
#include "app/PrintCoordinator.hpp"
#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/AnnotationCommands.hpp"
#include "pdf/PdfEngine.hpp"
#include "platform/Print.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace rivet;
using rivet::test::FakeFileDocument;
using rivet::test::FakePageDocument;
using rivet::test::FakeWritableEngine;
using rivet::test::QueueDispatcher;
using rivet::test::settle;

// Number of print working directories currently in the temp directory.
std::size_t countAssemblyDirectories() {
    std::error_code ignored;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(fs::temp_directory_path(), ignored)) {
        if (entry.path().filename().string().rfind("rivet-print-doc-", 0) == 0) ++count;
    }
    return count;
}

// What the reopened (assembled) document saw.
struct ReopenedStats {
    std::atomic<int> renders{0};
    std::mutex mutex;
    std::vector<std::size_t> pages;
    std::vector<fs::path> paths;
};

// Wraps a document read back from a fake-written file: counts renders and
// returns a bitmap of the requested size (the fake file document returns 1x1).
class CountingFileDocument final : public pdf::PdfDocument {
public:
    CountingFileDocument(std::unique_ptr<pdf::PdfDocument> inner, ReopenedStats& stats)
        : inner_(std::move(inner)), stats_(stats) {}

    const pdf::PdfDocumentInfo& info() const override { return inner_->info(); }
    core::Result<pdf::PdfPageInfo> pageInfo(std::size_t index) const override { return inner_->pageInfo(index); }
    core::Result<core::Bitmap> renderPage(std::size_t index, const core::Rect& rect, double scale) override {
        return renderPageInView(index, pdf::PdfPageView{}, {}, rect, scale);
    }

protected:
    core::Result<core::Bitmap> renderPageInView(std::size_t index, const pdf::PdfPageView&,
                                                std::span<const std::uint32_t>, const core::Rect& rect,
                                                double scale) override {
        ++stats_.renders;
        {
            std::lock_guard<std::mutex> lock(stats_.mutex);
            stats_.pages.push_back(index);
        }
        return core::Bitmap::create(static_cast<std::uint32_t>(std::ceil(rect.size.width * scale)),
                                    static_cast<std::uint32_t>(std::ceil(rect.size.height * scale)));
    }

private:
    std::unique_ptr<pdf::PdfDocument> inner_;
    ReopenedStats& stats_;
};

// FakeWritableEngine whose documents opened from a fake-WRITTEN file (the
// assembled print document) are wrapped in CountingFileDocument.
class PrintEngine final : public pdf::PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "print-test"; }

    core::Result<std::unique_ptr<pdf::PdfDocument>> openDocument(const fs::path& path,
                                                                 std::string_view password) override {
        auto opened = inner.openDocument(path, password);
        if (!opened.has_value()) return opened;
        if (dynamic_cast<FakeFileDocument*>(opened->get()) == nullptr) return opened;
        {
            std::lock_guard<std::mutex> lock(stats.mutex);
            stats.paths.push_back(path);
        }
        return std::unique_ptr<pdf::PdfDocument>(
            std::make_unique<CountingFileDocument>(std::move(*opened), stats));
    }

    core::Status assembleDocument(const pdf::PdfAssemblyRequest& request, pdf::IPdfByteSink& sink,
                                  std::vector<pdf::PdfAssembledPageAnnotations>* report = nullptr) override {
        return inner.assembleDocument(request, sink, report);
    }

    FakeWritableEngine inner;
    ReopenedStats stats;
};

class FakePrintService final : public platform::IPrintService {
public:
    core::Result<platform::PrintSettings> choosePrintSettings(const platform::PrintSetup& setup) override {
        ++panels;
        if (duringPanel) {
            auto hook = std::move(duringPanel);
            duringPanel = nullptr;
            hook();
        }
        platform::PrintSettings settings;
        settings.jobTitle = setup.jobTitle;
        settings.firstPage = 0;
        settings.lastPage = setup.pageCount - 1;
        return settings;
    }

    core::Status printSpool(const platform::PrintSettings&, const platform::PrintSpoolDescription& spool) override {
        ++printed;
        printedPages = spool.pages.size();
        return core::ok();
    }

    std::function<void()> duringPanel; // runs inside the "modal"
    std::atomic<int> panels{0};
    std::atomic<int> printed{0};
    std::size_t printedPages = 0;
};

struct PrintFixture {
    PrintFixture() {
        path = fs::temp_directory_path() / "rivet-print-test-doc.pdf";
        engine.inner.pageCounts[path.filename().string()] = 3;
        workspace.openDocument(path);
        CHECK(settle(dispatcher, [&] {
            const app::DocumentTab* active = workspace.activeTab();
            return active != nullptr && active->state() == app::DocumentTab::State::Ready;
        }));
        tab = workspace.activeTab();
        CHECK(tab != nullptr);
        if (tab != nullptr) id = tab->id();
    }

    ~PrintFixture() { coordinator.cancel(); }

    app::DocumentTab* activeTab() { return workspace.tabById(id); }
    FakePageDocument* base() { return dynamic_cast<FakePageDocument*>(tab->session()->documentPtr().get()); }

    void addAnnotation(std::size_t page) {
        editor::AnnotationDraft draft;
        draft.page = tab->session()->pageId(page);
        draft.kind = pdf::PdfAnnotationKind::Square;
        draft.rect = core::Rect{40, 50, 80, 40};
        draft.style.borderWidth = 2.0F;
        auto edit = editor::createAnnotations(*tab->session(), {draft});
        CHECK(edit.has_value());
        if (edit.has_value()) CHECK(tab->session()->execute(std::move(edit->command)).has_value());
    }

    bool printedOrFailed() {
        return service.printed.load() > 0 || lastStatusStartsWith("Print failed") ||
               lastStatusStartsWith("Print cancelled");
    }
    bool lastStatusStartsWith(const std::string& prefix) const {
        return !status.empty() && status.back().rfind(prefix, 0) == 0;
    }
    fs::path assemblyDirectory() {
        std::lock_guard<std::mutex> lock(engine.stats.mutex);
        return engine.stats.paths.empty() ? fs::path{} : engine.stats.paths.front().parent_path();
    }

    PrintEngine engine;
    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    app::DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    FakePrintService service;
    std::vector<std::string> status;
    app::PrintCoordinator coordinator{workspace, engine, scheduler, &dispatcher, &service,
                                      [this](std::string text) { status.push_back(std::move(text)); }};
    fs::path path;
    app::DocumentTab* tab = nullptr;
    app::TabId id;
};

} // namespace

RIVET_TEST(printSurvivesTheTabClosingWhileThePanelIsUp) {
    PrintFixture f;
    f.service.duringPanel = [&] { f.workspace.closeTab(0); };
    f.tab = nullptr; // dangling after the close; the coordinator must not rely on it
    f.coordinator.print(f.id);
    CHECK_EQ(f.service.panels.load(), 1);
    CHECK_EQ(f.workspace.tabCount(), std::size_t{0});
    CHECK(!f.coordinator.isSpooling());
    f.dispatcher.pump();
    CHECK_EQ(f.service.printed.load(), 0);
    CHECK(f.lastStatusStartsWith("Print cancelled"));
}

RIVET_TEST(printOfAnUnknownTabReportsNothingToPrint) {
    PrintFixture f;
    f.workspace.closeTab(0);
    f.coordinator.print(f.id);
    CHECK_EQ(f.service.panels.load(), 0);
    CHECK(f.lastStatusStartsWith("Nothing to print"));
}

RIVET_TEST(printWithoutAnnotationEditsRendersTheSourceAndAssemblesNothing) {
    PrintFixture f;
    FakePageDocument* base = f.base();
    CHECK(base != nullptr);
    f.coordinator.print(f.id);
    CHECK(settle(f.dispatcher, [&] { return f.printedOrFailed(); }));
    CHECK_EQ(f.service.printed.load(), 1);
    CHECK_EQ(f.service.printedPages, std::size_t{3});
    CHECK_EQ(f.engine.inner.assemblies.load(), 0);
    CHECK_EQ(f.engine.stats.renders.load(), 0);
    CHECK_EQ(base->renders.load(), 3);
}

RIVET_TEST(printWithAnnotationEditsRendersFromOneAssembledDocument) {
    PrintFixture f;
    FakePageDocument* base = f.base();
    CHECK(base != nullptr);
    f.addAnnotation(1);
    f.coordinator.print(f.id);
    CHECK(settle(f.dispatcher, [&] { return f.printedOrFailed(); }));
    CHECK_EQ(f.service.printed.load(), 1);
    CHECK_EQ(f.service.printedPages, std::size_t{3});

    // Assembled exactly once, with the edit on the second page.
    CHECK_EQ(f.engine.inner.assemblies.load(), 1);
    CHECK_EQ(f.engine.inner.lastEdits.size(), std::size_t{3});
    CHECK(f.engine.inner.lastEdits[0].create.empty());
    CHECK_EQ(f.engine.inner.lastEdits[1].create.size(), std::size_t{1});
    // All three pages were rendered from the reopened document, in order,
    // and none from the unedited source.
    CHECK_EQ(f.engine.stats.renders.load(), 3);
    CHECK_EQ(base->renders.load(), 0);
    {
        std::lock_guard<std::mutex> lock(f.engine.stats.mutex);
        CHECK_EQ(f.engine.stats.pages.size(), std::size_t{3});
        CHECK_EQ(f.engine.stats.paths.size(), std::size_t{1});
        CHECK(f.engine.stats.pages == (std::vector<std::size_t>{0, 1, 2}));
    }
    // The temp document is gone once the job ended.
    const fs::path directory = f.assemblyDirectory();
    CHECK(!directory.empty());
    CHECK(settle(f.dispatcher, [&] { return !fs::exists(directory); }));
}

RIVET_TEST(printCleansUpWhenTheAssemblyFails) {
    const std::size_t baseline = countAssemblyDirectories();
    PrintFixture f;
    f.addAnnotation(0);
    f.engine.inner.failAssembly = true;
    f.coordinator.print(f.id);
    CHECK(settle(f.dispatcher, [&] { return f.printedOrFailed(); }));
    CHECK_EQ(f.service.printed.load(), 0);
    CHECK(f.lastStatusStartsWith("Print failed"));
    CHECK_EQ(f.engine.inner.assemblies.load(), 1);
    CHECK_EQ(f.engine.stats.renders.load(), 0);
    // The private working directory (no document inside it) is removed.
    CHECK_EQ(countAssemblyDirectories(), baseline);
}

RIVET_TEST(printCancelledDuringTheAssemblyCleansUp) {
    const std::size_t baseline = countAssemblyDirectories();
    PrintFixture f;
    f.addAnnotation(0);
    f.engine.inner.closeGate();
    f.coordinator.print(f.id);
    CHECK(f.engine.inner.waitParked(1)); // the worker is inside the assembly
    CHECK(f.coordinator.requestCancel());
    f.engine.inner.release();
    CHECK(settle(f.dispatcher, [&] { return !f.coordinator.isSpooling(); }));
    CHECK_EQ(f.service.printed.load(), 0);
    CHECK(f.lastStatusStartsWith("Print cancelled"));
    CHECK_EQ(f.engine.stats.renders.load(), 0);
    CHECK_EQ(countAssemblyDirectories(), baseline);
}
