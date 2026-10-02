// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PrintSpooler.hpp"
#include "pdf/PdfEngine.hpp"
#include "platform/Print.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace rivet::app {

// Converts a finished editor spool into the platform's spool description
// (rivet_platform stays independent of rivet_editor).
platform::PrintSpoolDescription toPrintSpoolDescription(const editor::PrintSpool& spool);

// The application print flow for one shell:
//
//   print(tab)       -> platform print panel (modal, no rendering)
//                    -> PrintSpooler renders the chosen pages on a worker
//                       through the session's PdfDocument, status bar shows
//                       "Preparing to print… n/m (Esc to cancel)"
//                    -> completion (main thread): platform printSpool()
//                    -> "Printed …" / "Print failed: …" / "Print cancelled"
//
// The print panel is modal and pumps the main queue, so a completion (a save
// that closes its tab, ...) can destroy the tab while it is up: print()
// captures only values and the tab's id before the panel and re-resolves the
// tab through the workspace afterwards.
//
// Annotation edits: the page-model snapshot's source documents do not carry
// the session's pending annotation edits (overlay annotations, deletions), so
// a snapshot with any edited page is printed from an assembled document
// instead: on the spool worker, lazily and once, the whole snapshot is written
// (the same assembly as Save / Extract) to a private temp file, reopened, and
// the pages are rendered from that. The temp file is removed when the job
// ends, whether it finished, failed or was cancelled. Without edits the
// source documents are rendered directly and nothing is assembled.
//
// Lifetime: the spool job borrows the session's PdfDocument. The owner must
// call cancelIfDocument() before a session is destroyed (tab close) and
// cancel() before the shell tears down; both cancel AND wait for the worker,
// so no render call can outlive the session.
//
// Main-thread only.
class PrintCoordinator {
public:
    using StatusSink = std::function<void(std::string)>;

    // All references are shell-owned and must outlive the coordinator.
    // printService/dispatcher may be null (printing then reports that it is
    // unavailable). `engine` assembles and reopens the edited document.
    PrintCoordinator(DocumentWorkspace& workspace, pdf::PdfEngine& engine, core::TaskScheduler& scheduler,
                     core::IMainThreadDispatcher* dispatcher, platform::IPrintService* printService,
                     StatusSink setStatus);
    ~PrintCoordinator();

    PrintCoordinator(const PrintCoordinator&) = delete;
    PrintCoordinator& operator=(const PrintCoordinator&) = delete;

    // Starts printing the tab's document (title = the tab title). A tab that
    // is gone, not ready, or changed page count while the panel was up is
    // reported in the status bar and nothing is printed.
    void print(TabId tab);

    // True while pages are being spooled.
    bool isSpooling() const { return spooler_.isActive(); }

    // Esc: requests cancellation of an active spool without blocking.
    // Returns true when there was one to cancel.
    bool requestCancel();

    // Cancels the active spool and waits for its worker.
    void cancel();

    // cancel() when the active spool renders `document`.
    void cancelIfDocument(core::DocumentId document);

private:
    void handleSpooled(core::Result<editor::PrintSpool> spool);

    class Assembly;

    DocumentWorkspace& workspace_;
    pdf::PdfEngine& engine_;
    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    platform::IPrintService* printService_ = nullptr;
    StatusSink setStatus_;

    // The active job (main thread).
    std::optional<core::DocumentId> jobDocument_;
    platform::PrintSettings jobSettings_;
    bool jobAbandoned_ = false; // its document went away
    std::shared_ptr<Assembly> jobAssembly_; // the edited-document source, if any

    // Declared last: destroyed first (cancels and waits for its worker).
    editor::PrintSpooler spooler_;
};

} // namespace rivet::app
