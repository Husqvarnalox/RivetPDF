// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "app/MarkdownPdfExport.hpp"
#include "app/PrintCoordinator.hpp"
#include "app/ShellContext.hpp"
#include "core/async/AsyncScope.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfEngine.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace rivet::app {

// Export as PDF and printing of Markdown tabs (ADR-0023):
//
//   exportToPdf(tab) -> save panel -> snapshot (source text, decoded images)
//       -> worker: buildMarkdownPdf -> engine.buildDocument into an atomic
//          file writer (temp file + replace, like Save As) -> commit
//       -> completion (main): "Exported N pages to <file>" (+ replaced
//          characters / skipped images) or "Export failed: ..."
//   print(tab)       -> snapshot -> worker: buildMarkdownPdf ->
//          buildDocument into a private temp file -> engine.openDocument
//       -> completion (main): PrintCoordinator::printDocument, which owns
//          the document and the temp file until the job ends
//
// The worker reads only its snapshot: editing stays enabled, the tab's
// dirty state and path are never touched, and closing the tab does not
// stop an export (the file is still written). One export and one print
// preparation at a time per shell (a second request is reported, not
// queued).
//
// Lifetime: every worker task holds a core::AsyncScope token; the
// destructor drops undelivered completions, closes the scope (the workers
// check the token between steps and the sink before every write; an
// abandoned temp file is removed by the writer's RAII / the temp-file
// owner) and waits for the workers.
//
// Main thread only.
class MarkdownExportController {
public:
    using StatusSink = std::function<void(std::string)>;
    // The decoded images of the tab's preview (main thread).
    using ImageSnapshotProvider = std::function<ExportImageMap(const DocumentTab&)>;

    // All references are shell-owned and must outlive the controller.
    MarkdownExportController(pdf::PdfEngine& engine, ShellContext& context, core::TaskScheduler& scheduler,
                             PrintCoordinator& printer, StatusSink setStatus);
    ~MarkdownExportController();

    MarkdownExportController(const MarkdownExportController&) = delete;
    MarkdownExportController& operator=(const MarkdownExportController&) = delete;

    void setImageSnapshotProvider(ImageSnapshotProvider provider) { images_ = std::move(provider); }
    // Page geometry/typography of exports and printouts (default A4).
    void setOptions(MarkdownExportOptions options) { options_ = std::move(options); }

    // A Ready Markdown tab and a PDF backend that can build documents.
    bool canExport(const DocumentTab& tab) const;

    void exportToPdf(DocumentTab& tab);
    void print(DocumentTab& tab);

    bool isExporting() const { return exporting_; }
    bool isPreparingPrint() const { return preparingPrint_; }

    // Esc: cancels a print preparation without blocking. True when there
    // was one.
    bool requestCancelPrint();

private:
    struct PrintSlot;
    using WorkerToken = std::shared_ptr<core::AsyncScope::Token>;
    WorkerToken enterWorkerScope();
    ExportImageMap snapshotImages(const DocumentTab& tab) const;
    // Runs `task` on the main thread (through the dispatcher when there is
    // one) unless the controller is gone by then.
    void deliver(std::function<void()> task);

    pdf::PdfEngine& engine_;
    ShellContext& context_;
    core::TaskScheduler& scheduler_;
    PrintCoordinator& printer_;
    StatusSink setStatus_;
    ImageSnapshotProvider images_;
    MarkdownExportOptions options_;

    bool exporting_ = false;
    bool preparingPrint_ = false;
    // Cancellation of the current print preparation (Esc).
    std::shared_ptr<std::atomic<bool>> printCancel_;
    // The current print preparation's hand-over slot (see the .cpp).
    std::shared_ptr<PrintSlot> printSlot_;

    // Guards dispatcher-posted completions against a destroyed controller.
    std::shared_ptr<std::atomic<bool>> alive_;
    // Declared last: destroyed first, after the destructor closed it.
    core::AsyncScope scope_;
};

} // namespace rivet::app
