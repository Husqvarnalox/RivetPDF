// SPDX-License-Identifier: MPL-2.0
#include "app/PrintCoordinator.hpp"

#include "core/Error.hpp"

#include <format>
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

PrintCoordinator::PrintCoordinator(core::TaskScheduler& scheduler,
                                   core::IMainThreadDispatcher* dispatcher,
                                   platform::IPrintService* printService, StatusSink setStatus)
    : dispatcher_(dispatcher),
      printService_(printService),
      setStatus_(std::move(setStatus)),
      spooler_(scheduler, dispatcher) {}

PrintCoordinator::~PrintCoordinator() { cancel(); }

void PrintCoordinator::print(editor::DocumentSession& session, const std::string& title) {
    if (spooler_.isActive()) {
        setStatus_("Already preparing a print job (Esc to cancel)");
        return;
    }
    if (printService_ == nullptr || dispatcher_ == nullptr) {
        setStatus_("No print service available on this platform backend");
        return;
    }
    const std::size_t pageCount = session.pageCount();
    if (pageCount == 0) {
        setStatus_("Nothing to print — the document has no pages");
        return;
    }

    // Step 1: the native panel. App-modal (the tab cannot be closed while
    // it is up, so `session` stays valid); renders nothing.
    auto settings = printService_->choosePrintSettings(
        platform::PrintSetup{title, pageCount, session.pageSizePoints(0)});
    if (!settings.has_value()) {
        if (settings.error().code != core::ErrorCode::Cancelled) {
            setStatus_("Print failed: " + core::describe(settings.error()));
        }
        return;
    }

    // Step 2: spool the chosen pages on the worker. The render functions
    // borrow the session's document; cancelIfDocument() keeps them from
    // outliving it.
    std::vector<editor::PrintPageSource> pages;
    pages.reserve(pageCount);
    pdf::PdfDocument* document = &session.document();
    for (std::size_t index = 0; index < pageCount; ++index) {
        pages.push_back(editor::PrintPageSource{
            session.pageSizePoints(index),
            [document, index](const core::Rect& bandRectPoints, double devicePixelsPerPoint) {
                return document->renderPage(index, bandRectPoints, devicePixelsPerPoint);
            }});
    }
    editor::PrintSpoolOptions options;
    options.firstPage = settings->firstPage;
    options.lastPage = settings->lastPage;
    const std::size_t selected = settings->lastPage - settings->firstPage + 1;

    jobDocument_ = session.id();
    jobSettings_ = std::move(*settings);
    jobAbandoned_ = false;
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
        setStatus_("Print failed: " + core::describe(started.error()));
    }
}

// Step 3 (main thread): hand the finished spool to the platform. The spool
// (and its directory) dies when this returns.
void PrintCoordinator::handleSpooled(core::Result<editor::PrintSpool> spool) {
    const platform::PrintSettings settings = std::exchange(jobSettings_, {});
    const bool abandoned = std::exchange(jobAbandoned_, false);
    jobDocument_.reset();

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
    spooler_.cancel();
    setStatus_("Cancelling print…");
    return true;
}

void PrintCoordinator::cancel() {
    if (!spooler_.isActive()) return;
    jobAbandoned_ = true;
    spooler_.cancel();
    // At most one in-flight band render; the Cancelled completion still
    // arrives through the dispatcher and updates the status.
    spooler_.waitForWorker();
}

void PrintCoordinator::cancelIfDocument(core::DocumentId document) {
    if (jobDocument_.has_value() && *jobDocument_ == document) cancel();
}

} // namespace rivet::app
