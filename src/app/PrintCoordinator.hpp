// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PrintSpooler.hpp"
#include "platform/Print.hpp"

#include <functional>
#include <optional>
#include <string>

namespace rivet::app {

// Converts a finished editor spool into the platform's spool description
// (rivet_platform stays independent of rivet_editor).
platform::PrintSpoolDescription toPrintSpoolDescription(const editor::PrintSpool& spool);

// The application print flow for one shell:
//
//   print(session)   -> platform print panel (modal, no rendering)
//                    -> PrintSpooler renders the chosen pages on a worker
//                       through the session's PdfDocument, status bar shows
//                       "Preparing to print… n/m (Esc to cancel)"
//                    -> completion (main thread): platform printSpool()
//                    -> "Printed …" / "Print failed: …" / "Print cancelled"
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
    // unavailable).
    PrintCoordinator(core::TaskScheduler& scheduler, core::IMainThreadDispatcher* dispatcher,
                     platform::IPrintService* printService, StatusSink setStatus);
    ~PrintCoordinator();

    PrintCoordinator(const PrintCoordinator&) = delete;
    PrintCoordinator& operator=(const PrintCoordinator&) = delete;

    // Starts printing `session` (title = the tab title).
    void print(editor::DocumentSession& session, const std::string& title);

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

    core::IMainThreadDispatcher* dispatcher_ = nullptr;
    platform::IPrintService* printService_ = nullptr;
    StatusSink setStatus_;

    // The active job (main thread).
    std::optional<core::DocumentId> jobDocument_;
    platform::PrintSettings jobSettings_;
    bool jobAbandoned_ = false; // its document went away

    // Declared last: destroyed first (cancels and waits for its worker).
    editor::PrintSpooler spooler_;
};

} // namespace rivet::app
