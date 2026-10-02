// SPDX-License-Identifier: MPL-2.0
#include "app/FileController.hpp"

#include <format>
#include <system_error>
#include <utility>

namespace rivet::app {

namespace {

constexpr const char* kUnavailable = "File dialogs are not available on this platform backend";

std::string describeFailure(const core::Error& error) { return core::describe(error); }

} // namespace

FileController::FileController(pdf::PdfEngine& engine, ShellContext& context,
                               core::TaskScheduler& scheduler, StatusSink setStatus,
                               NotifySink notifyDocumentChanged)
    : engine_(engine), context_(context), scheduler_(scheduler), setStatus_(std::move(setStatus)),
      notifyDocumentChanged_(std::move(notifyDocumentChanged)),
      alive_(std::make_shared<std::atomic<bool>>(true)) {}

FileController::~FileController() {
    // Drop undelivered completions, then cancel the in-flight writes and wait
    // until no worker is inside a task of this controller: the tasks use the
    // engine, the context's dispatcher and the cancellation state, which must
    // not be touched once the controller (and then the shell) is gone.
    *alive_ = false;
    if (context_.services.lifecycle != nullptr) {
        context_.services.lifecycle->setCloseRequestHandler(nullptr);
        context_.services.lifecycle->setQuitRequestHandler(nullptr);
    }
    scope_.closeAndWait();
}

FileController::WorkerToken FileController::enterWorkerScope() {
    std::optional<core::AsyncScope::Token> token = scope_.enter();
    if (!token.has_value()) return nullptr;
    return std::make_shared<core::AsyncScope::Token>(std::move(*token));
}

DocumentTab* FileController::liveTab(TabId id) const {
    DocumentTab* tab = context_.workspace.tabById(id);
    return tab != nullptr && tab->session() != nullptr ? tab : nullptr;
}

DocumentTab* FileController::readyTab(TabId id) const {
    DocumentTab* tab = liveTab(id);
    return tab != nullptr && tab->state() == DocumentTab::State::Ready ? tab : nullptr;
}

void FileController::installLifecycleHandlers() {
    if (context_.services.lifecycle == nullptr) return;
    context_.services.lifecycle->setCloseRequestHandler([this] { return confirmCloseWindow(); });
    context_.services.lifecycle->setQuitRequestHandler(
        [this](platform::IAppLifecycle::QuitReply reply) { handleQuitRequest(std::move(reply)); });
}

bool FileController::canPerform(FileCommand command) const {
    const DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr) return false;
    if (command == FileCommand::Extract && selectionProvider_ != nullptr) {
        return !selectionProvider_().empty();
    }
    if (command == FileCommand::Split) return tab->session() != nullptr && tab->session()->pageCount() > 0;
    return true;
}

void FileController::perform(FileCommand command) {
    DocumentTab* tab = context_.readyActiveTab();
    if (tab == nullptr) return;
    switch (command) {
    case FileCommand::Save: save(*tab); break;
    case FileCommand::SaveAs: saveAs(*tab); break;
    case FileCommand::ImportBefore: importPages(*tab, tab->currentPage()); break;
    case FileCommand::ImportAfter: importPages(*tab, tab->currentPage() + 1); break;
    case FileCommand::Merge: importPages(*tab, std::nullopt); break;
    case FileCommand::Extract:
        if (selectionProvider_ != nullptr) extract(*tab, selectionProvider_());
        break;
    case FileCommand::Split: split(*tab); break;
    }
}

// --- Save / Save As -----------------------------------------------------------

bool FileController::isSaving(TabId tab) const {
    if (activeSave_.has_value() && activeSave_->tab == tab) return true;
    return std::any_of(queuedSaves_.begin(), queuedSaves_.end(),
                       [&](const SaveRequest& request) { return request.tab == tab; });
}

void FileController::save(DocumentTab& tab) {
    if (tab.session() == nullptr || tab.state() != DocumentTab::State::Ready) return;
    requestSave(tab, tab.session()->path(), false, true);
}

void FileController::saveAs(DocumentTab& tab) {
    if (tab.session() == nullptr || tab.state() != DocumentTab::State::Ready) return;
    if (context_.services.saveDialog == nullptr) {
        setStatus_(kUnavailable);
        return;
    }
    const TabId tabId = tab.id();
    platform::ISaveDialog::Options options;
    options.suggestedName = tab.path().filename().string();
    options.title = "Save As";
    options.prompt = "Save";
    const std::optional<std::filesystem::path> chosen = context_.services.saveDialog->runSavePanel(options);
    if (!chosen.has_value()) return; // cancelled: nothing changes
    // The panel may have run completions (a save closing this very tab).
    DocumentTab* current = readyTab(tabId);
    if (current == nullptr) {
        setStatus_("Save As cancelled: the document was closed");
        return;
    }
    if (*chosen == current->session()->path()) {
        save(*current);
        return;
    }
    // Refuse to clobber a document that is open in another tab.
    const std::size_t existing = context_.workspace.indexOfPath(*chosen);
    if (existing != DocumentWorkspace::kNoTab && context_.workspace.tab(existing)->id() != tabId) {
        setStatus_("A document with this path is already open in another tab");
        return;
    }
    requestSave(*current, *chosen, false, true);
}

void FileController::requestSave(DocumentTab& tab, std::filesystem::path destination,
                                 bool closeTabWhenDone, bool interactive) {
    if (tab.session() == nullptr) return;
    if (activeSave_.has_value()) {
        if (interactive) {
            setStatus_("A save is already in progress");
            return;
        }
        // The quit lifecycle chains behind the running save.
        queuedSaves_.push_back(
            SaveRequest{tab.id(), ++generationCounter_, std::move(destination), closeTabWhenDone});
        return;
    }
    WorkerToken token = enterWorkerScope();
    if (token == nullptr) return; // the controller is being destroyed
    SaveRequest request{tab.id(), ++generationCounter_, std::move(destination), closeTabWhenDone};

    core::Result<editor::DocumentWriteJob> job =
        editor::makeSaveJob(*tab.session(), request.destination);
    if (!job.has_value()) {
        setStatus_("Could not prepare the save: " + describeFailure(job.error()));
        return;
    }
    // The model cannot change under the in-flight save: the snapshot that is
    // written stays the state that gets marked saved and rebased onto.
    tab.session()->setEditingLocked(true, "saving");
    if (request.closeTabWhenDone) closeAfterSave_.push_back(request.tab);
    activeSave_ = request;
    setStatus_("Saving…");
    postSaveWrite(std::move(*job), request, std::move(token));
}

void FileController::postSaveWrite(editor::DocumentWriteJob job, SaveRequest request, WorkerToken token) {
    auto result = std::make_shared<std::optional<editor::DocumentWriteResult>>();
    // The token is declared first: it is released last, after the job (and
    // its snapshot) the worker was using.
    scheduler_.post([this, token = std::move(token), result, captured = std::move(job), request,
                     alive = alive_] mutable {
        editor::DocumentWriteControl control;
        control.cancelled = [t = token.get()] { return t->cancelled(); };
        *result = editor::runDocumentWrite(engine_, captured, control);
        if (context_.services.mainDispatcher != nullptr) {
            context_.services.mainDispatcher->post([this, result, request, alive] {
                if (!*alive) return;
                handleSaveCompleted(result, request);
            });
        } else {
            // Tests without a dispatcher: deliver inline on the "worker".
            if (*alive) handleSaveCompleted(result, request);
        }
    });
}

void FileController::startNextSave() {
    if (activeSave_.has_value() || queuedSaves_.empty()) return;
    WorkerToken token = enterWorkerScope();
    if (token == nullptr) return; // the controller is being destroyed
    SaveRequest request = std::move(queuedSaves_.front());
    queuedSaves_.pop_front();
    DocumentTab* tab = context_.workspace.tabById(request.tab);
    if (tab == nullptr || tab->session() == nullptr || tab->state() != DocumentTab::State::Ready) {
        // Gone meanwhile: nothing left to lose. Still present but unusable: not saved.
        saveSettled(request.tab, tab == nullptr);
        return;
    }
    core::Result<editor::DocumentWriteJob> job =
        editor::makeSaveJob(*tab->session(), request.destination);
    if (!job.has_value()) {
        tab->session()->setEditingLocked(false);
        setStatus_("Could not prepare the save: " + describeFailure(job.error()));
        saveSettled(request.tab, false);
        return;
    }
    tab->session()->setEditingLocked(true, "saving");
    if (request.closeTabWhenDone) closeAfterSave_.push_back(request.tab);
    activeSave_ = request;
    setStatus_("Saving…");
    postSaveWrite(std::move(*job), request, std::move(token));
}

void FileController::handleSaveCompleted(
    std::shared_ptr<std::optional<editor::DocumentWriteResult>> result, SaveRequest request) {
    activeSave_.reset();
    DocumentTab* tab = context_.workspace.tabById(request.tab);
    // The tab may be gone (closed while saving): the file was still written
    // atomically; there is nothing to rebase and nothing to report.
    if (tab == nullptr || tab->session() == nullptr || tab->state() != DocumentTab::State::Ready ||
        !result->has_value()) {
        saveSettled(request.tab, tab == nullptr);
        return;
    }
    editor::DocumentSession& session = *tab->session();
    editor::DocumentWriteResult& write = **result;
    if (!write.written.has_value()) {
        // The destination is intact (atomic replace): stay dirty, stay editable.
        session.setEditingLocked(false);
        setStatus_("Save failed: " + describeFailure(write.written.error()));
        saveSettled(request.tab, false);
        return;
    }
    const bool pathChanged = request.destination != session.path();
    if (write.rebase.has_value() && write.rebase->has_value()) {
        const core::Status rebased =
            session.rebaseOnto(std::move(**write.rebase),
                               pathChanged ? std::optional<std::filesystem::path>{request.destination}
                                           : std::nullopt);
        if (!rebased.has_value()) {
            setStatus_("Saved, but the document could not be reloaded: " + describeFailure(rebased.error()));
        }
    } else if (write.rebase.has_value()) {
        // Written but not reloadable: the save itself succeeded.
        setStatus_("Saved (the written file could not be reopened for reloading)");
    }
    if (pathChanged) {
        // The file now lives at the destination whatever happened to the
        // reload: the session must follow, or the next Save would silently
        // overwrite the previous path (rebaseOnto already did it on success).
        if (session.path() != request.destination) session.setPath(request.destination);
        context_.workspace.retitleTab(tab->id(), request.destination);
        if (notifyDocumentChanged_ != nullptr) notifyDocumentChanged_();
    }
    session.markSaved();
    session.setEditingLocked(false);
    setStatus_(std::format("Saved {}", session.path().filename().string()));
    saveSettled(request.tab, true);
}

void FileController::saveSettled(TabId tab, bool saved) {
    // The user chose to close this tab behind its save. A failed save must
    // never discard the edits: the tab stays open (and dirty).
    if (std::erase(closeAfterSave_, tab) > 0 && saved) {
        const std::size_t index = context_.workspace.indexOfTab(tab);
        if (index != DocumentWorkspace::kNoTab) context_.workspace.closeTab(index);
    }
    if (pendingQuit_.has_value()) {
        std::erase(pendingQuit_->outstanding, tab);
        if (!saved) {
            // Quit was waiting on this save: abort it, the app keeps running.
            auto quit = std::move(*pendingQuit_);
            pendingQuit_.reset();
            quit.reply(false);
        } else if (pendingQuit_->outstanding.empty()) {
            auto quit = std::move(*pendingQuit_);
            pendingQuit_.reset();
            quit.reply(true);
        }
    }
    startNextSave();
}

// --- Extract ----------------------------------------------------------------

void FileController::extract(DocumentTab& tab, std::span<const core::PageId> pages) {
    if (tab.session() == nullptr || tab.state() != DocumentTab::State::Ready) return;
    if (context_.services.saveDialog == nullptr) {
        setStatus_(kUnavailable);
        return;
    }
    if (pages.empty()) {
        setStatus_("No pages selected to extract");
        return;
    }
    const TabId tabId = tab.id();
    platform::ISaveDialog::Options options;
    options.suggestedName = tab.path().filename().string();
    options.title = "Export Selected Pages";
    options.prompt = "Export";
    const std::optional<std::filesystem::path> chosen = context_.services.saveDialog->runSavePanel(options);
    if (!chosen.has_value()) return;
    // The panel may have run completions that closed the tab.
    DocumentTab* current = readyTab(tabId);
    if (current == nullptr) {
        setStatus_("Export cancelled: the document was closed");
        return;
    }

    core::Result<editor::DocumentWriteJob> job = editor::makeExtractJob(*current->session(), pages, *chosen);
    if (!job.has_value()) {
        setStatus_("Could not prepare the export: " + describeFailure(job.error()));
        return;
    }
    WorkerToken token = enterWorkerScope();
    if (token == nullptr) return; // the controller is being destroyed
    setStatus_("Exporting…");
    auto result = std::make_shared<std::optional<editor::DocumentWriteResult>>();
    editor::DocumentWriteJob captured = std::move(*job);
    scheduler_.post([this, token = std::move(token), result, captured = std::move(captured),
                     alive = alive_] mutable {
        editor::DocumentWriteControl control;
        control.cancelled = [t = token.get()] { return t->cancelled(); };
        *result = editor::runDocumentWrite(engine_, captured, control);
        if (context_.services.mainDispatcher != nullptr) {
            context_.services.mainDispatcher->post([this, result, alive] {
                if (!*alive) return;
                editor::DocumentWriteResult& write = **result;
                if (write.written.has_value()) {
                    setStatus_("Exported");
                } else {
                    setStatus_("Export failed: " + describeFailure(write.written.error()));
                }
            });
        }
    });
}

// --- Split ------------------------------------------------------------------

void FileController::split(DocumentTab& tab) {
    if (tab.session() == nullptr || tab.state() != DocumentTab::State::Ready) return;
    if (context_.services.alerts == nullptr) {
        setStatus_("Text prompts are not available on this platform backend");
        return;
    }
    const TabId tabId = tab.id();
    const std::optional<std::string> text = context_.services.alerts->promptForText(
        "Split PDF by Ranges",
        std::format("Enter the page ranges to export, each to its own file (the document has {} pages), "
                    "for example 1-3, 4-7, 8-10.",
                    tab.session()->pageCount()),
        "");
    if (!text.has_value()) return; // cancelled: nothing changes
    // The prompt may have run completions that closed the tab.
    DocumentTab* current = readyTab(tabId);
    if (current == nullptr) {
        setStatus_("Split cancelled: the document was closed");
        return;
    }
    splitByRanges(*current, *text);
}

void FileController::splitByRanges(DocumentTab& tab, std::string_view rangeText) {
    if (tab.session() == nullptr || tab.state() != DocumentTab::State::Ready) return;
    const auto fail = [&](const std::string& message) {
        setStatus_(message);
        if (context_.services.alerts != nullptr) context_.services.alerts->showError("Split PDF", message);
    };
    const TabId tabId = tab.id();
    const core::Result<std::vector<editor::PageRange>> checkedRanges =
        editor::parsePageRanges(rangeText, tab.session()->pageCount());
    if (!checkedRanges.has_value()) {
        fail("Invalid page ranges: " + checkedRanges.error().message);
        return;
    }
    if (context_.services.saveDialog == nullptr) {
        setStatus_(kUnavailable);
        return;
    }
    platform::ISaveDialog::Options options;
    options.suggestedName = tab.path().filename().string();
    options.title = "Split PDF";
    options.prompt = "Split";
    const std::optional<std::filesystem::path> chosen = context_.services.saveDialog->runSavePanel(options);
    if (!chosen.has_value()) return;

    // The panel may have run completions (the tab closed, an import changed
    // the page count): re-resolve the tab and validate the ranges again
    // against the model that will actually be captured.
    DocumentTab* current = readyTab(tabId);
    if (current == nullptr) {
        setStatus_("Split cancelled: the document was closed");
        return;
    }
    editor::DocumentSession& session = *current->session();
    const core::Result<std::vector<editor::PageRange>> ranges =
        editor::parsePageRanges(rangeText, session.pageCount());
    if (!ranges.has_value()) {
        fail("Invalid page ranges: " + ranges.error().message);
        return;
    }

    // Never overwrite: every output must be new, checked before any write.
    const std::vector<std::filesystem::path> outputs = editor::splitOutputPaths(*chosen, *ranges);
    for (const std::filesystem::path& output : outputs) {
        std::error_code ec;
        if (std::filesystem::exists(output, ec)) {
            fail(std::format("Split cancelled: {} already exists. Nothing was written.",
                             output.filename().string()));
            return;
        }
    }

    // One consistent snapshot: every job is captured now, on the main thread.
    std::vector<editor::DocumentWriteJob> jobs;
    jobs.reserve(ranges->size());
    for (std::size_t i = 0; i < ranges->size(); ++i) {
        std::vector<core::PageId> pages;
        pages.reserve((*ranges)[i].size());
        for (std::size_t page = (*ranges)[i].first; page <= (*ranges)[i].last; ++page) {
            pages.push_back(session.pageId(page - 1));
        }
        core::Result<editor::DocumentWriteJob> job = editor::makeExtractJob(session, pages, outputs[i]);
        if (!job.has_value()) {
            fail("Could not prepare the split: " + describeFailure(job.error()));
            return;
        }
        // The existence check above is only advisory (it can race with
        // another writer): the atomic commit itself refuses to replace.
        job->overwriteExisting = false;
        jobs.push_back(std::move(*job));
    }

    struct SplitOutcome {
        std::size_t written = 0;
        std::optional<core::Error> failure;
        std::size_t failedIndex = 0;
    };
    WorkerToken token = enterWorkerScope();
    if (token == nullptr) return; // the controller is being destroyed
    auto outcome = std::make_shared<SplitOutcome>();
    const std::size_t total = jobs.size();
    setStatus_(std::format("Splitting into {} {}…", total, total == 1 ? "file" : "files"));
    core::IMainThreadDispatcher* dispatcher = context_.services.mainDispatcher;
    platform::IAlertService* alerts = context_.services.alerts;
    pdf::PdfEngine& engine = engine_;
    scheduler_.post([this, &engine, token = std::move(token), outcome, jobs = std::move(jobs), outputs,
                     total, dispatcher, alerts, alive = alive_]() mutable {
        // Worker: touches only the engine, the jobs (which own their
        // snapshots) and the scope token. Controller destruction cancels it
        // and waits for it.
        editor::DocumentWriteControl control;
        control.cancelled = [t = token.get()] { return t->cancelled(); };
        for (std::size_t i = 0; i < jobs.size(); ++i) {
            if (token->cancelled()) break; // nothing further is started
            editor::DocumentWriteResult write = editor::runDocumentWrite(engine, jobs[i], control);
            if (!write.written.has_value()) {
                outcome->failure = write.written.error();
                outcome->failedIndex = i;
                break;
            }
            ++outcome->written;
        }
        // The jobs (and their PDF snapshots) are released on this worker.
        jobs.clear();
        if (dispatcher == nullptr) return;
        dispatcher->post([this, outcome, outputs, total, alerts, alive] {
            if (!*alive) return;
            if (!outcome->failure.has_value()) {
                setStatus_(std::format("Split into {} {}", total, total == 1 ? "file" : "files"));
                return;
            }
            const std::string message = std::format(
                "Split failed at {}: {} ({} of {} files written)",
                outputs[outcome->failedIndex].filename().string(),
                describeFailure(*outcome->failure), outcome->written, total);
            setStatus_(message);
            if (alerts != nullptr) alerts->showError("Split PDF", message);
        });
    });
}

// --- Import / merge -----------------------------------------------------------

void FileController::importPages(DocumentTab& tab, std::optional<std::size_t> beforeIndex) {
    if (tab.session() == nullptr || tab.state() != DocumentTab::State::Ready) return;
    if (context_.services.fileDialog == nullptr) {
        setStatus_(kUnavailable);
        return;
    }
    const TabId tabId = tab.id();
    const core::Result<std::filesystem::path> chosen = context_.services.fileDialog->openPdf();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            setStatus_("Could not choose a file: " + describeFailure(chosen.error()));
        }
        return;
    }
    // The panel may have run completions that closed the tab.
    DocumentTab* current = readyTab(tabId);
    if (current == nullptr) {
        setStatus_("Import cancelled: the document was closed");
        return;
    }
    startImport(*current, beforeIndex, *chosen);
}

void FileController::startImport(DocumentTab& tab, std::optional<std::size_t> beforeIndex,
                                 std::filesystem::path sourcePath) {
    WorkerToken token = enterWorkerScope();
    if (token == nullptr) return; // the controller is being destroyed
    const std::uint64_t generation = ++generationCounter_;
    liveImports_.push_back(generation);
    const TabId tabId = tab.id();
    setStatus_(std::format("Importing {}…", sourcePath.filename().string()));

    auto result = std::make_shared<core::Result<std::vector<editor::PageSource>>>(
        std::unexpected(core::Error{core::ErrorCode::NotAvailable, "pending", "import"}));
    scheduler_.post([this, token = std::move(token), result, sourcePath = std::move(sourcePath), tabId,
                     generation, beforeIndex, alive = alive_] mutable {
        // Worker: open the source and read its page metadata (PDFium calls
        // under the adapter's gate). The document stays alive through the
        // returned sources' shared pointers. The token keeps the controller
        // (and through it the engine) alive until this task is done.
        if (token->cancelled()) return;
        auto document = engine_.openDocument(sourcePath, {});
        if (!document.has_value()) {
            *result = std::unexpected(std::move(document).error());
        } else {
            std::shared_ptr<pdf::PdfDocument> source{std::move(document.value())};
            *result = editor::PageModel::describeAllPages(source);
        }
        if (context_.services.mainDispatcher != nullptr) {
            context_.services.mainDispatcher->post(
                [this, result, tabId, generation, beforeIndex, alive] {
                    if (!*alive) return;
                    handleImportCompleted(result, tabId, generation, beforeIndex);
                });
        } else {
            if (*alive) handleImportCompleted(result, tabId, generation, beforeIndex);
        }
    });
}

void FileController::handleImportCompleted(
    std::shared_ptr<core::Result<std::vector<editor::PageSource>>> result, TabId tab,
    std::uint64_t generation, std::optional<std::size_t> beforeIndex) {
    std::erase(liveImports_, generation); // stale completions never apply
    DocumentTab* target = context_.workspace.tabById(tab);
    if (target == nullptr || target->session() == nullptr ||
        target->state() != DocumentTab::State::Ready) {
        return;
    }
    if (!result->has_value()) {
        const core::Error& error = result->error();
        if (error.code == core::ErrorCode::PasswordRequired) {
            setStatus_("Import failed: the PDF is password protected — encrypted imports are not supported");
        } else {
            setStatus_("Import failed: " + describeFailure(error));
        }
        return;
    }
    const std::size_t imported = result->value().size();
    std::size_t index = target->session()->pageCount();
    if (beforeIndex.has_value()) index = std::min(*beforeIndex, index);
    auto command = std::make_unique<editor::InsertPagesCommand>(target->session()->pageModel(),
                                                               std::move(result->value()), index);
    const core::Status status = target->session()->execute(std::move(command));
    if (!status.has_value()) {
        setStatus_("Import failed: " + describeFailure(status.error()));
        return;
    }
    setStatus_(std::format("Imported {} page{}", imported, imported == 1 ? "" : "s"));
}

// --- Close / quit lifecycle -----------------------------------------------------

std::vector<TabId> FileController::dirtyTabs() const {
    std::vector<TabId> dirty;
    for (std::size_t i = 0; i < context_.workspace.tabCount(); ++i) {
        DocumentTab* tab = context_.workspace.tab(i);
        if (tab != nullptr && tab->session() != nullptr && tab->session()->isDirty()) {
            dirty.push_back(tab->id());
        }
    }
    return dirty;
}

void FileController::discardAndClose(TabId id) {
    const std::size_t index = context_.workspace.indexOfTab(id);
    if (index != DocumentWorkspace::kNoTab) context_.workspace.closeTab(index);
}

bool FileController::confirmCloseTab(DocumentTab& tab) {
    if (tab.session() == nullptr || !tab.session()->isDirty()) return true;

    // A prompt that cannot be asked (no alert service) must never discard
    // data silently: treat it as Cancel.
    if (context_.services.alerts == nullptr) return false;

    // The prompt is modal and may run completions that close this tab: keep
    // the id and a copy of the title (the view must outlive the tab), never
    // the pointer.
    const TabId tabId = tab.id();
    const std::string title = tab.title();
    const platform::SaveChangesChoice choice = context_.services.alerts->askSaveChanges(title);
    switch (choice) {
    case platform::SaveChangesChoice::Cancel:
        return false;
    case platform::SaveChangesChoice::DontSave:
        discardAndClose(tabId);
        return true;
    case platform::SaveChangesChoice::Save: {
        DocumentTab* current = liveTab(tabId);
        // Gone or already clean (its save settled during the prompt): there
        // is nothing left to save and the tab may close now.
        if (current == nullptr || !current->session()->isDirty()) return true;
        if (isSaving(tabId)) {
            // Keep the tab open; it closes when its save settles.
            if (std::find(closeAfterSave_.begin(), closeAfterSave_.end(), tabId) == closeAfterSave_.end()) {
                closeAfterSave_.push_back(tabId);
            }
            return false;
        }
        requestSave(*current, current->session()->path(), true, true);
        return false;
    }
    }
    return false;
}

bool FileController::confirmCloseWindow() {
    const std::vector<TabId> dirty = dirtyTabs();
    if (dirty.empty()) return true;
    if (context_.services.alerts == nullptr) return false;

    if (dirty.size() == 1) {
        DocumentTab* tab = liveTab(dirty.front());
        return tab == nullptr || confirmCloseTab(*tab);
    }

    const platform::ReviewChangesChoice choice =
        context_.services.alerts->askReviewUnsavedChanges(dirty.size());
    switch (choice) {
    case platform::ReviewChangesChoice::Cancel:
        return false;
    case platform::ReviewChangesChoice::DiscardAll:
        for (const TabId id : dirty) discardAndClose(id);
        return true;
    case platform::ReviewChangesChoice::Review:
        // Sequential per-document prompts (app-modal). A tab answered "Save"
        // defers its close to its save completion; the close proceeds only
        // when every prompt was answered without cancelling. Every prompt may
        // run completions, so each tab is re-resolved right before its turn.
        for (const TabId id : dirty) {
            DocumentTab* tab = liveTab(id);
            if (tab == nullptr || !tab->session()->isDirty()) continue;
            if (!confirmCloseTab(*tab)) return false;
        }
        return closeAfterSave_.empty();
    }
    return false;
}

void FileController::handleQuitRequest(platform::IAppLifecycle::QuitReply reply) {
    const std::vector<TabId> dirty = dirtyTabs();
    if (dirty.empty()) {
        reply(true);
        return;
    }
    if (context_.services.alerts == nullptr) {
        reply(false); // never discard data silently
        return;
    }

    std::vector<TabId> outstanding;
    if (dirty.size() == 1) {
        DocumentTab* tab = liveTab(dirty.front());
        if (tab == nullptr) {
            reply(true);
            return;
        }
        const TabId tabId = tab->id();
        const std::string title = tab->title(); // copy: the prompt is modal
        const platform::SaveChangesChoice choice = context_.services.alerts->askSaveChanges(title);
        switch (choice) {
        case platform::SaveChangesChoice::Cancel:
            reply(false);
            return;
        case platform::SaveChangesChoice::DontSave:
            discardAndClose(tabId);
            reply(true);
            return;
        case platform::SaveChangesChoice::Save:
            outstanding.push_back(tabId);
            break;
        }
    } else {
        const platform::ReviewChangesChoice choice =
            context_.services.alerts->askReviewUnsavedChanges(dirty.size());
        switch (choice) {
        case platform::ReviewChangesChoice::Cancel:
            reply(false);
            return;
        case platform::ReviewChangesChoice::DiscardAll:
            for (const TabId id : dirty) discardAndClose(id);
            reply(true);
            return;
        case platform::ReviewChangesChoice::Review:
            for (const TabId id : dirty) {
                // Re-resolved before every prompt: earlier prompts may have
                // run completions that closed or saved this tab.
                DocumentTab* tab = liveTab(id);
                if (tab == nullptr || !tab->session()->isDirty()) continue;
                const std::string title = tab->title(); // copy: the prompt is modal
                const platform::SaveChangesChoice perTab = context_.services.alerts->askSaveChanges(title);
                if (perTab == platform::SaveChangesChoice::Cancel) {
                    reply(false);
                    return;
                }
                if (perTab == platform::SaveChangesChoice::DontSave) {
                    discardAndClose(id);
                    continue;
                }
                outstanding.push_back(id);
            }
            if (outstanding.empty()) {
                reply(true);
                return;
            }
            break;
        }
    }

    // Defer the quit until every accepted save finished. Tabs already saving
    // (or queued) just wait; the others start saving now (chained when busy).
    // A tab that vanished or was saved during the prompts has nothing left to
    // wait for.
    pendingQuit_ = PendingQuit{std::move(reply), outstanding};
    for (const TabId tabId : outstanding) {
        DocumentTab* tab = liveTab(tabId);
        if (tab == nullptr) {
            saveSettled(tabId, true); // vanished meanwhile: re-check the quit state
            continue;
        }
        if (isSaving(tabId)) continue;
        if (!tab->session()->isDirty()) {
            saveSettled(tabId, true); // saved meanwhile: nothing to write
            continue;
        }
        requestSave(*tab, tab->session()->path(), false, false);
    }
}

} // namespace rivet::app
