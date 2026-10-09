// SPDX-License-Identifier: MPL-2.0
// MarkdownExportController over a fake engine (no PDF backend needed):
//   - Export as PDF writes the built document atomically at the chosen path
//     and reports the page count; a cancelled panel exports nothing; engine
//     failures are reported; a second export while one runs is refused;
//     destroying the controller mid-export neither crashes nor leaves files;
//   - printing a Markdown tab builds a private temp PDF, reopens it, spools
//     it through PrintCoordinator::printDocument and removes the temp
//     directory when the job ends;
//   - FileController delegates FileCommand::ExportPdf.
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/MarkdownExportController.hpp"
#include "app/PrintCoordinator.hpp"
#include "app/ShellContext.hpp"
#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfBuild.hpp"
#include "pdf/PdfEngine.hpp"
#include "platform/PlatformKit.hpp"
#include "platform/Print.hpp"
#include "ui/PdfViewport.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace rivet;
using app::DocumentTab;
using app::FileCommand;
using rivet::test::QueueDispatcher;
using rivet::test::settle;

constexpr const char* kMagic = "%FAKE-BUILT-PDF\n";

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("rivet-mdexport-" + std::to_string(++counter));
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

std::string readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t countEntries(const fs::path& dir) {
    std::error_code ignored;
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir, ignored)) ++n;
    return n;
}

std::size_t countPrintDirectories() {
    std::error_code ignored;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(fs::temp_directory_path(), ignored)) {
        if (entry.path().filename().string().rfind("rivet-print-doc-", 0) == 0) ++count;
    }
    return count;
}

// A document reopened from a fake-built file: the built page sizes, renders
// of the requested size.
class BuiltDocument final : public pdf::PdfDocument {
public:
    explicit BuiltDocument(std::vector<core::Size> sizes) : sizes_(std::move(sizes)) { info_.pageCount = sizes_.size(); }

    const pdf::PdfDocumentInfo& info() const override { return info_; }
    core::Result<pdf::PdfPageInfo> pageInfo(std::size_t index) const override {
        if (index >= sizes_.size()) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        return pdf::PdfPageInfo(index, sizes_[index], core::PageRotation::None);
    }
    core::Result<core::Bitmap> renderPage(std::size_t index, const core::Rect& rect, double scale) override {
        return renderPageInView(index, pdf::PdfPageView{}, {}, nullptr, rect, scale);
    }

    std::atomic<int>* renders = nullptr;

protected:
    core::Result<core::Bitmap> renderPageInView(std::size_t, const pdf::PdfPageView&, std::span<const std::uint32_t>,
                                                const pdf::PdfPageContentEditsPtr&, const core::Rect& rect,
                                                double scale) override {
        if (renders != nullptr) ++*renders;
        return core::Bitmap::create(static_cast<std::uint32_t>(std::ceil(rect.size.width * scale)),
                                    static_cast<std::uint32_t>(std::ceil(rect.size.height * scale)));
    }

private:
    std::vector<core::Size> sizes_;
    pdf::PdfDocumentInfo info_;
};

// Builds a fake file (magic + one line per page, in small chunks, or in
// large chunks with pauses so a cancellation can land between them) and
// reopens it as a BuiltDocument.
class ExportEngine final : public pdf::PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "export-test"; }

    core::Result<std::unique_ptr<pdf::PdfDocument>> openDocument(const fs::path& path, std::string_view) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            openedPaths.push_back(path);
        }
        if (readBytes(path).rfind(kMagic, 0) != 0) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument, "not built here", "test"));
        }
        std::vector<core::Size> sizes;
        {
            std::lock_guard<std::mutex> lock(mutex);
            sizes = lastSizes;
        }
        auto document = std::make_unique<BuiltDocument>(std::move(sizes));
        document->renders = &renders;
        return std::unique_ptr<pdf::PdfDocument>(std::move(document));
    }

    core::Status buildDocument(const pdf::PdfBuildDocument& build, pdf::IPdfByteSink& sink) override {
        ++builds;
        started = true;
        {
            std::unique_lock<std::mutex> lock(mutex);
            gateCv.wait(lock, [this] { return !gated; });
            lastSizes.clear();
            for (const auto& page : build.pages) lastSizes.push_back(core::Size{page.width, page.height});
        }
        if (fail) return std::unexpected(core::makeError(core::ErrorCode::Io, "disk on fire", "test"));
        if (core::Status s = sink.write(kMagic, std::char_traits<char>::length(kMagic)); !s) return s;
        if (slowChunks > 0) {
            const std::string chunk(300 * 1024, 'x');
            for (int i = 0; i < slowChunks; ++i) {
                if (core::Status s = sink.write(chunk.data(), chunk.size()); !s) return s;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        for (std::size_t i = 0; i < build.pages.size(); ++i) {
            const std::string line = "page " + std::to_string(i) + "\n";
            if (core::Status s = sink.write(line.data(), line.size()); !s) return s;
        }
        return core::ok();
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            gated = false;
        }
        gateCv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable gateCv;
    bool gated = false;
    std::atomic<bool> fail{false};
    int slowChunks = 0;
    std::atomic<int> builds{0};
    std::atomic<bool> started{false};
    std::atomic<int> renders{0};
    std::vector<core::Size> lastSizes;
    std::vector<fs::path> openedPaths;
};

class FakeSaveDialog final : public platform::ISaveDialog {
public:
    std::optional<fs::path> runSavePanel(const Options& options) override {
        ++panels;
        lastOptions = options;
        auto next = next_;
        next_.reset();
        return next;
    }
    std::optional<fs::path> next_;
    Options lastOptions;
    int panels = 0;
};

class FakePrintService final : public platform::IPrintService {
public:
    core::Result<platform::PrintSettings> choosePrintSettings(const platform::PrintSetup& setup) override {
        ++panels;
        lastTitle = setup.jobTitle;
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
    int panels = 0;
    int printed = 0;
    std::size_t printedPages = 0;
    std::string lastTitle;
};

struct Fixture {
    ExportEngine engine;
    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    app::DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    TempDir docs;
    TempDir out;
    FakeSaveDialog saveDialog;
    FakePrintService printService;
    platform::ShellServices services;
    std::unique_ptr<ui::PdfViewport> viewport = std::make_unique<ui::PdfViewport>();
    std::unique_ptr<app::ShellContext> context;
    std::vector<std::string> statusLog;
    std::unique_ptr<app::PrintCoordinator> printer;
    std::unique_ptr<app::MarkdownExportController> exporter;
    std::unique_ptr<app::FileController> files;

    Fixture() {
        services.mainDispatcher = &dispatcher;
        services.saveDialog = &saveDialog;
        services.printService = &printService;
        context = std::make_unique<app::ShellContext>(app::ShellContext{workspace, services, *viewport, {}, {}, {}});
        auto status = [this](std::string text) { statusLog.push_back(std::move(text)); };
        printer = std::make_unique<app::PrintCoordinator>(workspace, engine, scheduler, &dispatcher, &printService,
                                                          status);
        exporter = std::make_unique<app::MarkdownExportController>(engine, *context, scheduler, *printer, status);
        files = std::make_unique<app::FileController>(engine, *context, scheduler, status, [] {});
        files->setMarkdownExporter(exporter.get());
    }
    ~Fixture() {
        engine.release();
        files.reset();
        exporter.reset();
        printer.reset();
        viewport->clearDocument();
    }

    DocumentTab* openMd(const std::string& name, const std::string& text) {
        {
            std::ofstream file(docs(name), std::ios::binary);
            file << text;
        }
        workspace.openDocument(docs(name));
        settle(dispatcher, [this] {
            DocumentTab* tab = workspace.activeTab();
            return tab != nullptr && tab->state() != DocumentTab::State::Loading;
        });
        return workspace.activeTab();
    }
    bool lastStatusStartsWith(const std::string& prefix) const {
        return !statusLog.empty() && statusLog.back().rfind(prefix, 0) == 0;
    }
};

const char* kDoc = "# Title\n\nSome text.\n";

} // namespace

RIVET_TEST(markdownExportWritesFileAtChosenPath) {
    Fixture f;
    DocumentTab* tab = f.openMd("note.md", kDoc);
    CHECK(tab != nullptr && tab->state() == DocumentTab::State::Ready);
    if (tab == nullptr) return;
    CHECK(f.exporter->canExport(*tab));
    f.saveDialog.next_ = f.out("note-out"); // no extension: .pdf is appended
    f.exporter->exportToPdf(*tab);
    CHECK_EQ(f.saveDialog.lastOptions.suggestedName, std::string("note.pdf"));
    CHECK(f.exporter->isExporting());
    CHECK(settle(f.dispatcher, [&] { return !f.exporter->isExporting(); }));
    const fs::path written = f.out("note-out.pdf");
    CHECK(fs::exists(written));
    CHECK(readBytes(written).rfind(kMagic, 0) == 0);
    CHECK_EQ(countEntries(f.out.path()), std::size_t{1}); // no temp file left
    CHECK(f.lastStatusStartsWith("Exported 1 page to note-out.pdf"));
    CHECK(!tab->markdown()->isDirty());
    CHECK(tab->path() == f.docs("note.md"));
}

RIVET_TEST(markdownExportCancelledPanelDoesNothing) {
    Fixture f;
    DocumentTab* tab = f.openMd("note.md", kDoc);
    if (tab == nullptr) return;
    const std::size_t before = f.statusLog.size();
    f.exporter->exportToPdf(*tab); // next_ empty: the panel is cancelled
    CHECK_EQ(f.saveDialog.panels, 1);
    CHECK(!f.exporter->isExporting());
    CHECK_EQ(f.engine.builds.load(), 0);
    CHECK_EQ(f.statusLog.size(), before);
    CHECK_EQ(countEntries(f.out.path()), std::size_t{0});
}

RIVET_TEST(markdownExportReportsEngineFailure) {
    Fixture f;
    DocumentTab* tab = f.openMd("note.md", kDoc);
    if (tab == nullptr) return;
    f.engine.fail = true;
    f.saveDialog.next_ = f.out("x.pdf");
    f.exporter->exportToPdf(*tab);
    CHECK(settle(f.dispatcher, [&] { return !f.exporter->isExporting(); }));
    CHECK(f.lastStatusStartsWith("Export failed: "));
    CHECK(!fs::exists(f.out("x.pdf")));
    CHECK_EQ(countEntries(f.out.path()), std::size_t{0});
}

RIVET_TEST(markdownExportRefusesSecondExportWhileRunning) {
    Fixture f;
    DocumentTab* tab = f.openMd("note.md", kDoc);
    if (tab == nullptr) return;
    f.engine.gated = true;
    f.saveDialog.next_ = f.out("a.pdf");
    f.exporter->exportToPdf(*tab);
    CHECK(settle(f.dispatcher, [&] { return f.engine.started.load(); }));
    f.saveDialog.next_ = f.out("b.pdf");
    f.exporter->exportToPdf(*tab);
    CHECK(f.lastStatusStartsWith("An export is already running"));
    CHECK_EQ(f.saveDialog.panels, 1);
    f.engine.release();
    CHECK(settle(f.dispatcher, [&] { return !f.exporter->isExporting(); }));
    CHECK(fs::exists(f.out("a.pdf")));
    CHECK(!fs::exists(f.out("b.pdf")));
    CHECK_EQ(f.engine.builds.load(), 1);
}

RIVET_TEST(markdownExportTeardownDuringExportIsSafe) {
    Fixture f;
    DocumentTab* tab = f.openMd("note.md", kDoc);
    if (tab == nullptr) return;
    f.engine.slowChunks = 20;
    f.saveDialog.next_ = f.out("slow.pdf");
    f.exporter->exportToPdf(*tab);
    CHECK(settle(f.dispatcher, [&] { return f.engine.started.load(); }));
    f.files.reset();
    f.exporter.reset(); // cancels the writer between chunks and waits
    f.dispatcher.pump(); // the dropped completion must not run
    CHECK(!fs::exists(f.out("slow.pdf")));
    CHECK_EQ(countEntries(f.out.path()), std::size_t{0});
}

RIVET_TEST(markdownPrintSpoolsTempDocumentAndRemovesIt) {
    Fixture f;
    DocumentTab* tab = f.openMd("print.md", kDoc);
    if (tab == nullptr) return;
    const std::size_t dirsBefore = countPrintDirectories();
    f.exporter->print(*tab);
    CHECK(f.exporter->isPreparingPrint());
    CHECK(settle(f.dispatcher, [&] { return f.printService.printed == 1; }));
    CHECK(settle(f.dispatcher, [&] { return !f.printer->isSpooling(); }));
    CHECK(!f.exporter->isPreparingPrint());
    CHECK_EQ(f.printService.panels, 1);
    CHECK_EQ(f.printService.printedPages, std::size_t{1});
    CHECK_EQ(f.printService.lastTitle, tab->title());
    CHECK_GT(f.engine.renders.load(), 0);
    {
        std::lock_guard<std::mutex> lock(f.engine.mutex);
        CHECK_EQ(f.engine.openedPaths.size(), std::size_t{1});
        if (!f.engine.openedPaths.empty()) {
            const fs::path opened = f.engine.openedPaths.front();
            CHECK_EQ(opened.filename().string(), std::string("document.pdf"));
            CHECK(opened.parent_path().filename().string().rfind("rivet-print-doc-", 0) == 0);
            CHECK(settle(f.dispatcher, [&] { return !fs::exists(opened.parent_path()); }));
        }
    }
    CHECK_EQ(countPrintDirectories(), dirsBefore);
}

RIVET_TEST(markdownPrintCancelRemovesTempDocument) {
    Fixture f;
    DocumentTab* tab = f.openMd("print.md", kDoc);
    if (tab == nullptr) return;
    const std::size_t dirsBefore = countPrintDirectories();
    f.engine.gated = true;
    f.exporter->print(*tab);
    CHECK(settle(f.dispatcher, [&] { return f.engine.started.load(); }));
    CHECK(f.exporter->requestCancelPrint());
    f.engine.release();
    CHECK(settle(f.dispatcher, [&] { return !f.exporter->isPreparingPrint(); }));
    CHECK(f.lastStatusStartsWith("Print cancelled"));
    CHECK_EQ(f.printService.panels, 0);
    CHECK_EQ(countPrintDirectories(), dirsBefore);
}

RIVET_TEST(fileControllerDelegatesExportPdf) {
    Fixture f;
    CHECK(!f.files->canPerform(FileCommand::ExportPdf)); // no tab
    DocumentTab* tab = f.openMd("note.md", kDoc);
    if (tab == nullptr) return;
    CHECK(f.files->canPerform(FileCommand::ExportPdf));
    f.saveDialog.next_ = f.out("via-menu.pdf");
    f.files->perform(FileCommand::ExportPdf);
    CHECK_EQ(f.saveDialog.panels, 1);
    CHECK(settle(f.dispatcher, [&] { return !f.exporter->isExporting(); }));
    CHECK(fs::exists(f.out("via-menu.pdf")));

    f.files->setMarkdownExporter(nullptr);
    CHECK(!f.files->canPerform(FileCommand::ExportPdf));
}
