// SPDX-License-Identifier: MPL-2.0
#include "app/PrintCoordinator.hpp"

#include "core/Error.hpp"
#include "editor/DocumentSaver.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <random>
#include <system_error>
#include <utility>
#include <vector>

namespace rivet::app {

platform::PrintSpoolDescription toPrintSpoolDescription(const editor::PrintSpool& spool) {
    platform::PrintSpoolDescription description;
    description.pages.reserve(spool.pages().size());
    for (const editor::SpooledPage& page : spool.pages()) {
        platform::PrintSpoolPage out;
        out.pageIndex = page.pageIndex;
        out.displaySizePoints = page.displaySizePoints;
        out.bands.reserve(page.bands.size());
        for (const editor::SpooledBand& band : page.bands) {
            out.bands.push_back(platform::PrintSpoolBand{band.file, band.pixelWidth, band.pixelHeight,
                                                         band.stride, band.rectPoints});
        }
        description.pages.push_back(std::move(out));
    }
    return description;
}

namespace {

core::Error printError(core::ErrorCode code, std::string message) {
    return core::makeError(code, std::move(message), "app");
}

bool hasAnnotationEdits(const editor::PageModelSnapshot& snapshot) {
    for (const editor::PageEntry& entry : snapshot.entries()) {
        if (entry.annotations != nullptr &&
            (!entry.annotations->suppressed.empty() || !entry.annotations->overlay.empty())) {
            return true;
        }
    }
    return false;
}

} // namespace

// The edited document a print job renders from: the page-model snapshot
// assembled (all pages, model order) into a private temp file and reopened.
// Page `position` of the reopened document is model position `position`.
//
// Used only from the spool worker, one call at a time, except cancel() (any
// thread) and the destructor (after the worker is done with the job).
class PrintCoordinator::Assembly {
public:
    Assembly(pdf::PdfEngine& engine, editor::PageSnapshotPtr snapshot, pdf::PdfAssemblyRequest request)
        : engine_(engine), snapshot_(std::move(snapshot)), request_(std::move(request)) {}

    ~Assembly() {
        document_.reset(); // close before the file goes away
        if (!directory_.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
        }
    }

    Assembly(const Assembly&) = delete;
    Assembly& operator=(const Assembly&) = delete;

    void cancel() { cancelled_.store(true); }

    core::Result<core::Bitmap> render(std::size_t position, const core::Rect& bandRectPoints,
                                      double devicePixelsPerPoint) {
        if (cancelled_.load()) return std::unexpected(printError(core::ErrorCode::Cancelled, "print cancelled"));
        std::lock_guard<std::mutex> lock(mutex_);
        if (!attempted_) {
            attempted_ = true;
            openError_ = assembleAndOpen();
        }
        if (openError_.has_value()) return std::unexpected(*openError_);
        auto info = document_->pageInfo(position);
        if (!info.has_value()) return std::unexpected(std::move(info).error());
        return document_->renderPage(position, info->view, bandRectPoints, devicePixelsPerPoint);
    }

private:
    std::optional<core::Error> assembleAndOpen() {
        std::error_code error;
        const std::filesystem::path parent = std::filesystem::temp_directory_path(error);
        if (error) return printError(core::ErrorCode::Io, "no temporary directory for printing: " + error.message());
        std::random_device device;
        for (int attempt = 0; attempt < 16 && directory_.empty(); ++attempt) {
            const std::uint64_t token = (std::uint64_t{device()} << 32) ^ device();
            const std::filesystem::path candidate = parent / std::format("rivet-print-doc-{:016x}", token);
            if (std::filesystem::create_directory(candidate, error)) {
                directory_ = candidate;
                std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace, error);
            } else if (error) {
                return printError(core::ErrorCode::Io,
                                  "cannot create the print working directory: " + error.message());
            }
        }
        if (directory_.empty()) return printError(core::ErrorCode::Io, "cannot find a unique print directory name");

        editor::DocumentWriteJob job;
        job.kind = editor::DocumentWriteJob::Kind::Extract;
        job.snapshot = snapshot_;
        job.request = std::move(request_);
        job.destination = directory_ / "document.pdf";
        editor::DocumentWriteControl control;
        control.cancelled = [this] { return cancelled_.load(); };
        const editor::DocumentWriteResult written = editor::runDocumentWrite(engine_, job, control);
        if (!written.written.has_value()) return written.written.error();
        if (cancelled_.load()) return printError(core::ErrorCode::Cancelled, "print cancelled");

        auto reopened = engine_.openDocument(job.destination);
        if (!reopened.has_value()) return std::move(reopened).error();
        document_ = std::move(*reopened);
        return std::nullopt;
    }

    pdf::PdfEngine& engine_;
    editor::PageSnapshotPtr snapshot_; // keeps the request's document pointers alive
    pdf::PdfAssemblyRequest request_;
    std::atomic<bool> cancelled_{false};

    std::mutex mutex_;
    bool attempted_ = false;
    std::optional<core::Error> openError_;
    std::filesystem::path directory_;
    std::unique_ptr<pdf::PdfDocument> document_;
};

PrintCoordinator::PrintCoordinator(DocumentWorkspace& workspace, pdf::PdfEngine& engine,
                                   core::TaskScheduler& scheduler, core::IMainThreadDispatcher* dispatcher,
                                   platform::IPrintService* printService, StatusSink setStatus)
    : workspace_(workspace),
      engine_(engine),
      dispatcher_(dispatcher),
      printService_(printService),
      setStatus_(std::move(setStatus)),
      spooler_(scheduler, dispatcher) {}

PrintCoordinator::~PrintCoordinator() { cancel(); }

void PrintCoordinator::print(TabId tabId) {
    if (spooler_.isActive()) {
        setStatus_("Already preparing a print job (Esc to cancel)");
        return;
    }
    if (printService_ == nullptr || dispatcher_ == nullptr) {
        setStatus_("No print service available on this platform backend");
        return;
    }
    // Only values leave this block: the panel below is modal and pumps the
    // main queue, so the tab (and its session) may be destroyed meanwhile.
    std::string title;
    std::size_t pageCount = 0;
    core::Size firstPageSize;
    core::DocumentId document;
    {
        const DocumentTab* tab = workspace_.tabById(tabId);
        if (tab == nullptr || tab->session() == nullptr || tab->state() != DocumentTab::State::Ready) {
            setStatus_("Nothing to print — open a document first");
            return;
        }
        pageCount = tab->session()->pageCount();
        if (pageCount == 0) {
            setStatus_("Nothing to print — the document has no pages");
            return;
        }
        title = tab->title();
        firstPageSize = tab->session()->pageSizePoints(0);
        document = tab->session()->id();
    }

    // Step 1: the native panel (renders nothing).
    auto settings = printService_->choosePrintSettings(platform::PrintSetup{title, pageCount, firstPageSize});
    if (!settings.has_value()) {
        if (settings.error().code != core::ErrorCode::Cancelled) {
            setStatus_("Print failed: " + core::describe(settings.error()));
        }
        return;
    }

    // Re-resolve: the tab may have been closed (or its document changed
    // shape) while the panel was up.
    DocumentTab* tab = workspace_.tabById(tabId);
    if (tab == nullptr || tab->session() == nullptr || tab->state() != DocumentTab::State::Ready ||
        tab->session()->id() != document) {
        setStatus_("Print cancelled — the document was closed");
        return;
    }
    editor::DocumentSession& session = *tab->session();
    if (session.pageCount() != pageCount) {
        setStatus_("Print cancelled — the document changed while the print panel was open");
        return;
    }
    if (spooler_.isActive()) {
        setStatus_("Already preparing a print job (Esc to cancel)");
        return;
    }

    // Step 2: spool the chosen pages on the worker. The render functions
    // capture the page-model snapshot's entries (source document kept alive
    // by the entry, page presented through its view), so printing reflects
    // the edited page order/rotation/crop and never reads session state.
    // A snapshot with annotation edits renders from its assembled document.
    const editor::PageSnapshotPtr snapshot = session.pageSnapshot();
    std::shared_ptr<Assembly> assembly;
    if (hasAnnotationEdits(*snapshot)) {
        std::vector<core::PageId> all;
        all.reserve(snapshot->size());
        for (const editor::PageEntry& entry : snapshot->entries()) all.push_back(entry.id);
        auto request = snapshot->toAssemblyRequest(editor::PageModelSnapshot::AssemblyMode::Extract, all);
        if (!request.has_value()) {
            setStatus_("Print failed: " + core::describe(request.error()));
            return;
        }
        assembly = std::make_shared<Assembly>(engine_, snapshot, std::move(*request));
    }

    std::vector<editor::PrintPageSource> pages;
    pages.reserve(pageCount);
    for (std::size_t index = 0; index < pageCount; ++index) {
        const editor::PageEntry& entry = snapshot->at(index);
        if (assembly != nullptr) {
            pages.push_back(editor::PrintPageSource{
                pdf::displaySize(entry.view),
                [assembly, index](const core::Rect& bandRectPoints, double devicePixelsPerPoint) {
                    return assembly->render(index, bandRectPoints, devicePixelsPerPoint);
                }});
            continue;
        }
        pages.push_back(editor::PrintPageSource{
            pdf::displaySize(entry.view),
            [source = entry.source, sourceIndex = entry.sourcePageIndex, view = entry.view](
                const core::Rect& bandRectPoints, double devicePixelsPerPoint) {
                return source->renderPage(sourceIndex, view, bandRectPoints, devicePixelsPerPoint);
            }});
    }
    editor::PrintSpoolOptions options;
    options.firstPage = settings->firstPage;
    options.lastPage = settings->lastPage;
    const std::size_t selected = settings->lastPage - settings->firstPage + 1;

    jobDocument_ = document;
    jobSettings_ = std::move(*settings);
    jobAbandoned_ = false;
    jobAssembly_ = std::move(assembly);
    setStatus_(std::format("Preparing to print… 0/{} (Esc to cancel)", selected));
    const core::Status started = spooler_.start(
        std::move(pages), std::move(options),
        [this](std::size_t done, std::size_t total) {
            setStatus_(std::format("Preparing to print… {}/{} (Esc to cancel)", done, total));
        },
        [this](core::Result<editor::PrintSpool> spool) { handleSpooled(std::move(spool)); });
    if (!started.has_value()) {
        jobDocument_.reset();
        jobSettings_ = {};
        jobAssembly_.reset();
        setStatus_("Print failed: " + core::describe(started.error()));
    }
}

// Step 3 (main thread): hand the finished spool to the platform. The spool
// (and its directory) dies when this returns.
void PrintCoordinator::handleSpooled(core::Result<editor::PrintSpool> spool) {
    const platform::PrintSettings settings = std::exchange(jobSettings_, {});
    const bool abandoned = std::exchange(jobAbandoned_, false);
    jobDocument_.reset();
    // The spooler drops its job (and the render functions' reference) after
    // this callback; releasing ours makes the temp file go with it.
    jobAssembly_.reset();

    if (!spool.has_value()) {
        if (spool.error().code == core::ErrorCode::Cancelled) {
            setStatus_("Print cancelled");
        } else {
            setStatus_("Print failed: " + core::describe(spool.error()));
        }
        return;
    }
    if (abandoned) {
        // Finished just before the cancel: the user no longer wants it.
        setStatus_("Print cancelled");
        return;
    }
    setStatus_("Printing…");
    const core::Status printed = printService_->printSpool(settings, toPrintSpoolDescription(*spool));
    if (!printed.has_value()) {
        if (printed.error().code == core::ErrorCode::Cancelled) {
            setStatus_("Print cancelled");
        } else {
            setStatus_("Print failed: " + core::describe(printed.error()));
        }
        return;
    }
    setStatus_(std::format("Printed {}", settings.jobTitle));
}

bool PrintCoordinator::requestCancel() {
    if (!spooler_.isActive()) return false;
    jobAbandoned_ = true;
    if (jobAssembly_ != nullptr) jobAssembly_->cancel();
    spooler_.cancel();
    setStatus_("Cancelling print…");
    return true;
}

void PrintCoordinator::cancel() {
    if (!spooler_.isActive()) return;
    jobAbandoned_ = true;
    if (jobAssembly_ != nullptr) jobAssembly_->cancel();
    spooler_.cancel();
    // At most one in-flight band render; the Cancelled completion still
    // arrives through the dispatcher and updates the status.
    spooler_.waitForWorker();
}

void PrintCoordinator::cancelIfDocument(core::DocumentId document) {
    if (jobDocument_.has_value() && *jobDocument_ == document) cancel();
}

} // namespace rivet::app
