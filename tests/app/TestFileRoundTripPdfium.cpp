// SPDX-License-Identifier: MPL-2.0
// FileController save against the REAL PDFium backend: open a fixture in a
// tab, edit through page commands, save through the file lifecycle, close,
// reopen the written file and verify what actually persisted. Bodies run
// only when PDFium is built in (early return otherwise).
#include "RivetTest.h"

#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/ShellContext.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/PageCommands.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <vector>

#ifndef RIVET_APP_PDF_FIXTURE_DIR
#define RIVET_APP_PDF_FIXTURE_DIR "tests/pdf/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
using rivet::app::DocumentTab;
using rivet::app::DocumentWorkspace;
using rivet::app::FileController;
using rivet::app::ShellContext;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentSession;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;
using rivet::ui::Container;
using rivet::ui::PdfViewport;

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = rivet::pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return nullptr;
    return engine;
}

fs::path fixture(const char* name) { return fs::path(RIVET_APP_PDF_FIXTURE_DIR) / name; }

// Deterministic main-thread queue.
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

// Text of a page (the fixtures carry one marker word per page).
std::string pageText(PdfDocument& document, std::size_t index) {
    auto text = document.textPage(index);
    if (!text.has_value()) return {};
    return (*text)->text();
}

// The fixture pages carry "PAGE-<n>" somewhere in their text.
std::string marker(PdfDocument& document, std::size_t page) {
    const std::string all = pageText(document, page);
    const auto at = all.find("PAGE-");
    if (at != std::string::npos && at + 5 < all.size()) return all.substr(at, 6);
    return all;
}

std::vector<std::string> markers(PdfDocument& document) {
    std::vector<std::string> out;
    const std::size_t count = document.info().pageCount;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) out.push_back(marker(document, i));
    return out;
}

// Unique scratch directory, removed on exit.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("rivet-roundtrip-" + std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path operator()(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

class FakeOpenDialog final : public rivet::platform::IFileDialog {
public:
    rivet::core::Result<fs::path> openPdf() override { return next_; }
    fs::path next_;
};

class FakeSaveDialog final : public rivet::platform::ISaveDialog {
public:
    std::optional<fs::path> runSavePanel(const Options&) override { return next_; }
    std::optional<fs::path> next_;
};

// "PAGE-n" (base fixture pages) or "IMPORT-n" (import-3.pdf pages).
std::string label(PdfDocument& document, std::size_t page) {
    const std::string all = pageText(document, page);
    if (const auto at = all.find("PAGE-"); at != std::string::npos) return all.substr(at, 6);
    if (const auto at = all.find("IMPORT-"); at != std::string::npos) return all.substr(at, 8);
    return all;
}

std::vector<std::string> labels(PdfDocument& document) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < document.info().pageCount; ++i) out.push_back(label(document, i));
    return out;
}

struct Shell {
    std::unique_ptr<PdfEngine> engine = pdfiumEngine();
    TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    rivet::platform::ShellServices services;
    std::unique_ptr<Container> root = std::make_unique<Container>();
    PdfViewport* viewport = nullptr;
    std::unique_ptr<ShellContext> context;
    std::unique_ptr<FileController> files;
    std::vector<std::string> statusLog;

    Shell() {
        services.mainDispatcher = &dispatcher;
        auto vp = std::make_unique<PdfViewport>();
        viewport = vp.get();
        context = std::make_unique<ShellContext>(ShellContext{workspace, services, *viewport, {}, {}, {}});
        root->addChild(std::move(vp));
        files = std::make_unique<FileController>(*engine, *context, scheduler,
                                                 [this](std::string text) { statusLog.push_back(std::move(text)); },
                                                 [] {});
    }

    ~Shell() {
        files.reset();
        viewport->clearDocument();
    }

    DocumentWorkspace workspace{*engine, scheduler, &dispatcher};

    // Opens a COPY of `path` (never the original: saving over a fixture
    // would destroy it).
    DocumentTab* openCopy(const fs::path& path, const fs::path& destination) {
        std::error_code ec;
        fs::copy_file(path, destination, fs::copy_options::overwrite_existing, ec);
        if (ec) return nullptr;
        return open(destination);
    }

    DocumentTab* open(const fs::path& path) {
        workspace.openDocument(path);
        if (!dispatcher.waitUntil([this] {
                DocumentTab* tab = workspace.activeTab();
                return tab != nullptr && tab->state() == DocumentTab::State::Ready;
            })) {
            return nullptr;
        }
        return workspace.activeTab();
    }
    bool hasStatus(const std::string& prefix) const {
        return !statusLog.empty() && statusLog.back().rfind(prefix, 0) == 0;
    }
};

} // namespace

RIVET_TEST(pdfiumFileControllerSaveRoundTrip) {
    Shell shell;
    if (shell.engine == nullptr) return; // PDFium not built in

    TempDir dir;
    DocumentTab* tab = shell.openCopy(fixture("markers-5.pdf"), dir("work.pdf"));
    CHECK(tab != nullptr);
    DocumentSession& session = *tab->session();
    CHECK_EQ(session.pageCount(), 5u);

    // Edit: delete page 2, rotate page 0, duplicate page 3, crop page 1.
    auto& model = session.pageModel();
    const rivet::core::PageId p0 = session.pageId(0);
    const rivet::core::PageId p1 = session.pageId(1);
    const rivet::core::PageId p2 = session.pageId(2);
    const rivet::core::PageId p3 = session.pageId(3);
    CHECK(session.execute(std::make_unique<rivet::editor::DeletePagesCommand>(model, std::vector{p2})).has_value());
    CHECK(session.execute(std::make_unique<rivet::editor::RotatePagesCommand>(model, std::vector{p0}, 90)).has_value());
    CHECK(session.execute(std::make_unique<rivet::editor::DuplicatePagesCommand>(model, std::vector{p3})).has_value());
    // p1 portrait letter (612x792): crop to a 300x400 box away from the edges.
    const rivet::pdf::PdfBox crop{100.0, 100.0, 400.0, 500.0};
    CHECK(session.execute(std::make_unique<rivet::editor::CropPagesCommand>(model, std::vector{p1}, crop)).has_value());
    // Expected model order: PAGE-1(rot90) PAGE-2(cropped) PAGE-4 PAGE-4' PAGE-5.
    CHECK_EQ(session.pageCount(), 5u);
    CHECK(session.isDirty());

    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!session.isDirty());

    // Close the tab, then reopen what landed on disk.
    const fs::path saved = tab->session()->path();
    shell.workspace.closeTab(shell.workspace.activeIndex());
    shell.workspace.openDocument(saved);
    CHECK(shell.dispatcher.waitUntil([this_ = &shell.workspace] {
        DocumentTab* reopened = this_->activeTab();
        return reopened != nullptr && reopened->state() == DocumentTab::State::Ready;
    }));
    DocumentTab* reopened = shell.workspace.activeTab();
    CHECK(reopened != nullptr);
    DocumentSession& reSession = *reopened->session();

    // Page order survived: 1, 2, 4, 4, 5 (page PAGE-3 was deleted; the
    // duplicate of PAGE-4 carries PAGE-4).
    std::shared_ptr<PdfDocument> document = reSession.documentPtr();
    const std::vector<std::string> out = markers(*document);
    CHECK((out == std::vector<std::string>{"PAGE-1", "PAGE-2", "PAGE-4", "PAGE-4", "PAGE-5"}));

    // The rotation persisted (first page is landscape now).
    auto first = document->pageInfo(0);
    CHECK(first.has_value());
    CHECK(first->rotation == rivet::core::PageRotation::Clockwise90);
    CHECK_NEAR(first->sizePoints.width, 792.0, 0.5);

    // The crop persisted (second page box == the requested crop).
    auto second = document->pageInfo(1);
    CHECK(second.has_value());
    CHECK_NEAR(second->view.cropBox.left, crop.left, 0.01);
    CHECK_NEAR(second->view.cropBox.bottom, crop.bottom, 0.01);
    CHECK_NEAR(second->view.cropBox.right, crop.right, 0.01);
    CHECK_NEAR(second->view.cropBox.top, crop.top, 0.01);

    // Text is still extractable after the round trip (renderable output).
    CHECK(pageText(*document, 4).find("PAGE-5") != std::string::npos);
}

RIVET_TEST(pdfiumFileControllerSaveAsAndExtractRoundTrip) {
    Shell shell;
    if (shell.engine == nullptr) return;

    TempDir dir;
    DocumentTab* tab = shell.openCopy(fixture("markers-5.pdf"), dir("work.pdf"));
    CHECK(tab != nullptr);
    DocumentSession& session = *tab->session();

    // Extract pages 3 and 0; makeExtractJob writes them in MODEL order
    // (document order), so the output is PAGE-1 then PAGE-4.
    const std::vector<rivet::core::PageId> pages = {session.pageId(3), session.pageId(0)};
    auto job = rivet::editor::makeExtractJob(session, pages, dir("out.pdf"));
    CHECK(job.has_value());
    const rivet::editor::DocumentWriteResult write = rivet::editor::runDocumentWrite(*shell.engine, *job);
    CHECK(write.written.has_value());

    // Reopen the extraction: two pages, in model order, source untouched.
    auto out = shell.engine->openDocument(dir("out.pdf"), {});
    CHECK(out.has_value());
    CHECK((markers(**out) == std::vector<std::string>{"PAGE-1", "PAGE-4"}));
    CHECK_EQ(session.pageCount(), 5u);
    CHECK(!session.isDirty());
}

RIVET_TEST(pdfiumFileControllerSplitByRangesRoundTrip) {
    Shell shell;
    if (shell.engine == nullptr) return;
    FakeOpenDialog openDialog;
    FakeSaveDialog saveDialog;
    shell.services.fileDialog = &openDialog;
    shell.services.saveDialog = &saveDialog;

    TempDir dir;
    DocumentTab* tab = shell.openCopy(fixture("markers-5.pdf"), dir("work.pdf"));
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    DocumentSession& session = *tab->session();
    auto& model = session.pageModel();

    // Import 3 pages (IMPORT-1..3) at the end, duplicate pages 0 and 3, move
    // the last page to the front, rotate one page and crop another:
    //   I3 P1 P1' P2(rot90) P3(crop) P4 P4' P5 I1 I2
    openDialog.next_ = fixture("import-3.pdf");
    shell.files->importPages(*tab, std::nullopt);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Imported 3 pages"); }));
    CHECK(session.execute(std::make_unique<rivet::editor::DuplicatePagesCommand>(
                              model, std::vector{session.pageId(0), session.pageId(3)}))
              .has_value());
    CHECK_EQ(session.pageCount(), 10u);
    CHECK(session.execute(std::make_unique<rivet::editor::MovePagesCommand>(model, std::vector{session.pageId(9)}, 0))
              .has_value());
    CHECK(session.execute(std::make_unique<rivet::editor::RotatePagesCommand>(model, std::vector{session.pageId(3)}, 90))
              .has_value());
    const rivet::pdf::PdfBox crop{100.0, 100.0, 400.0, 500.0};
    CHECK(session.execute(std::make_unique<rivet::editor::CropPagesCommand>(model, std::vector{session.pageId(4)}, crop))
              .has_value());
    CHECK(session.isDirty());

    saveDialog.next_ = dir("out.pdf");
    shell.files->splitByRanges(*tab, "1-3, 4-7, 8-10");
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split into 3 files"); }));
    CHECK(session.isDirty()); // export only
    CHECK_EQ(session.pageCount(), 10u);

    auto a = shell.engine->openDocument(dir("out_1-3.pdf"), {});
    auto b = shell.engine->openDocument(dir("out_4-7.pdf"), {});
    auto c = shell.engine->openDocument(dir("out_8-10.pdf"), {});
    CHECK(a.has_value() && b.has_value() && c.has_value());
    if (!a || !b || !c) return;
    CHECK_EQ((*a)->info().pageCount, 3u);
    CHECK_EQ((*b)->info().pageCount, 4u);
    CHECK_EQ((*c)->info().pageCount, 3u);
    CHECK((labels(**a) == std::vector<std::string>{"IMPORT-3", "PAGE-1", "PAGE-1"}));
    CHECK((labels(**b) == std::vector<std::string>{"PAGE-2", "PAGE-3", "PAGE-4", "PAGE-4"}));
    CHECK((labels(**c) == std::vector<std::string>{"PAGE-5", "IMPORT-1", "IMPORT-2"}));

    // Rotation and CropBox survived in the output carrying those pages.
    CHECK((*b)->pageInfo(0)->view.rotation == rivet::core::PageRotation::Clockwise90);
    CHECK((*b)->pageInfo(1)->view.rotation == rivet::core::PageRotation::None);
    const auto cropped = (*b)->pageInfo(1)->view.cropBox;
    CHECK_NEAR(cropped.left, crop.left, 0.01);
    CHECK_NEAR(cropped.bottom, crop.bottom, 0.01);
    CHECK_NEAR(cropped.right, crop.right, 0.01);
    CHECK_NEAR(cropped.top, crop.top, 0.01);
    CHECK((*a)->pageInfo(1)->view.rotation == rivet::core::PageRotation::None);
}
