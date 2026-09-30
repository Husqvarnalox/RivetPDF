// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"
#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageRangeParser.hpp"
#include "platform/AppLifecycle.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::app {

// File commands the shell exposes to shortcuts and platform menus (the
// platform passes them as integers: keep the values stable).
enum class FileCommand : std::uint8_t {
    Save = 0,
    SaveAs = 1,
    ImportBefore = 2, // insert all pages of another PDF before the current page
    ImportAfter = 3,  // ... after the current page
    Merge = 4,        // append all pages of another PDF
    Extract = 5,      // write the selected pages to a new PDF
    Split = 6,        // write page ranges to one new PDF each
};

// The application file lifecycle for one shell, on top of the editor's save
// pipeline (editor::DocumentSaver) and the platform dialogs/alerts:
//
//   Save / Save As -> job capture (main) -> worker: assemble + atomic
//       replace -> completion (main): rebase onto the written file,
//       markSaved; Save As retitles the tab on success
//   Extract        -> save panel -> worker assembly of the given pages into
//       a new PDF; the source document is never modified
//   Split          -> range prompt -> save panel (directory + base name) ->
//       one Extract job per range, captured up front from one model
//       snapshot, written sequentially by ONE worker task; export only
//       (the source is never modified). Partial-success semantics: outputs
//       already written stay, the failing one leaves nothing under its final
//       name (atomic write), later ones are not attempted. Existing output
//       files are never overwritten (checked before anything is written).
//       Like Extract it is not tied to the tab: closing the tab does not
//       stop it (the jobs own their snapshots); destroying the controller
//       (app quit) cancels it cooperatively and drops the completion, and
//       quit does not wait for it.
//   Import / Merge -> open panel -> worker: open the source PDF and read its
//       page metadata -> completion: InsertPagesCommand (atomic on the model)
//
// One save at a time per shell: an interactive Save/Save As while one is in
// flight is reported, not queued; saves started by the quit lifecycle chain
// behind the running one. Extract and import may overlap with a save. The
// session being saved is locked against edits while its save is in flight
// (DocumentSession::setEditingLocked), so the state that is written stays
// exactly the state that is marked saved and rebased onto.
//
// Async identity: completions are addressed by (TabId, generation) - TabIds
// are never reused, every operation mints a fresh generation, and a
// completion only applies while its generation is still registered. A save
// whose tab was closed meanwhile drops its result: the job owns the page
// snapshot (source documents stay alive until the worker is done), the
// destination was atomically replaced or left intact, and there is no
// session left to rebase.
//
// Dirty close/quit lifecycle (platform::IAppLifecycle): closing the window
// or quitting prompts per dirty document (Save / Don't Save / Cancel;
// several dirty documents use the platform review prompt). A per-tab close
// answered "Save" completes when the save settles. Quit defers the platform
// reply until every accepted save finished, so termination never races an
// in-flight save. A window close answered "Save" performs the saves and
// leaves the window open (there is no programmatic window-close service to
// re-trigger the close).
//
// Main thread only.
class FileController {
public:
    using StatusSink = std::function<void(std::string)>;
    // Re-pushed after a document identity change (Save As retitles a tab).
    using NotifySink = std::function<void()>;
    // Resolves the current page selection (Extract targets).
    using SelectionProvider = std::function<std::vector<core::PageId>()>;

    // All references are shell-owned and must outlive the controller.
    FileController(pdf::PdfEngine& engine, ShellContext& context, core::TaskScheduler& scheduler,
                   StatusSink setStatus, NotifySink notifyDocumentChanged);
    ~FileController();

    FileController(const FileController&) = delete;
    FileController& operator=(const FileController&) = delete;

    // Registers the window-close / quit interception (no-op without a
    // lifecycle service). Called by the shell once.
    void installLifecycleHandlers();

    // Where Extract takes its pages from (wired by the shell to the page
    // editing controller).
    void setSelectionProvider(SelectionProvider provider) { selectionProvider_ = std::move(provider); }

    bool canPerform(FileCommand command) const;
    // Runs the command on the active tab (a status message otherwise).
    void perform(FileCommand command);

    // --- Operations (all no-ops without a Ready active tab) ---------------

    // Saves the tab to its current path (worker pipeline; see class comment).
    void save(DocumentTab& tab);
    // Save As: shows the save panel first; cancellation changes nothing.
    void saveAs(DocumentTab& tab);
    // Writes `pages` (model order) to a new PDF chosen in the save panel.
    void extract(DocumentTab& tab, std::span<const core::PageId> pages);
    // Split: prompts for page ranges (IAlertService::promptForText), then
    // calls splitByRanges. Cancelling the prompt changes nothing.
    void split(DocumentTab& tab);
    // Split with the range text already known (see editor::parsePageRanges):
    // validates, shows the save panel for the output directory + base name,
    // then writes `<stem>_<a>-<b>.pdf` per range.
    void splitByRanges(DocumentTab& tab, std::string_view rangeText);
    // Imports every page of a chosen PDF at `beforeIndex` (nullopt = append).
    void importPages(DocumentTab& tab, std::optional<std::size_t> beforeIndex);

    // --- Close / quit lifecycle (see class comment) ------------------------

    // Tab close (tab strip / Cmd+W): prompts for a dirty tab and acts.
    // Returns whether the tab may be closed NOW; "Save" closes it when its
    // save completes.
    bool confirmCloseTab(DocumentTab& tab);
    // Window close: prompts for every dirty tab; "Save" performs the saves
    // and keeps the window open. Returns whether the window may close NOW.
    bool confirmCloseWindow();
    // Application quit: prompts for every dirty tab and defers the platform
    // quit reply until every accepted save completed.
    void handleQuitRequest(platform::IAppLifecycle::QuitReply reply);

    bool isSaving(TabId tab) const;
    bool isSavingAnything() const { return activeSave_.has_value(); }

private:
    // One in-flight or queued save (main thread state; workers only see the
    // captured job).
    struct SaveRequest {
        TabId tab;
        std::uint64_t generation = 0;
        std::filesystem::path destination;
        bool closeTabWhenDone = false;
    };

    // Interactive = reports "already saving" when busy; queued = chains
    // behind the running save (the quit lifecycle).
    void requestSave(DocumentTab& tab, std::filesystem::path destination, bool closeTabWhenDone,
                     bool interactive);
    void startNextSave();
    void handleSaveCompleted(std::shared_ptr<std::optional<editor::DocumentWriteResult>> result,
                             SaveRequest request);
    void startImport(DocumentTab& tab, std::optional<std::size_t> beforeIndex,
                     std::filesystem::path sourcePath);
    void handleImportCompleted(std::shared_ptr<core::Result<std::vector<editor::PageSource>>> result,
                               TabId tab, std::uint64_t generation,
                               std::optional<std::size_t> beforeIndex);
    // The dirty tabs, oldest first (prompt/save order).
    std::vector<DocumentTab*> dirtyTabs() const;
    // Discards the tab's changes and closes it without prompting.
    void discardAndClose(DocumentTab* tab);
    // A save of `tab` settled: runs the deferred close intent and, when
    // quitting, advances the outstanding set (completing the reply on the
    // last one) and starts the next chained save.
    void saveSettled(TabId tab, bool saved);

    pdf::PdfEngine& engine_;
    ShellContext& context_;
    core::TaskScheduler& scheduler_;
    StatusSink setStatus_;
    NotifySink notifyDocumentChanged_;
    SelectionProvider selectionProvider_;

    // Guards dispatcher-posted completions against a destroyed controller
    // (the flag object is heap-owned by every completion copy).
    std::shared_ptr<std::atomic<bool>> alive_;
    std::uint64_t generationCounter_ = 0;
    std::optional<SaveRequest> activeSave_;
    std::deque<SaveRequest> queuedSaves_;
    // Live import operations (a completion applies only while present).
    std::vector<std::uint64_t> liveImports_;

    // Quit lifecycle: the deferred reply and the tabs still to wait for.
    struct PendingQuit {
        platform::IAppLifecycle::QuitReply reply;
        std::vector<TabId> outstanding;
    };
    std::optional<PendingQuit> pendingQuit_;
    // Tabs the user chose to close behind their in-flight save.
    std::vector<TabId> closeAfterSave_;
};

} // namespace rivet::app
