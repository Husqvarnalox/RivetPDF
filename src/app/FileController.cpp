// SPDX-License-Identifier: MPL-2.0
#include "app/FileController.hpp"

#include <format>
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
    // Drop undelivered completions; in-flight workers touch only their job
    // (which owns the page snapshot) and the shell-owned engine, both of
    // which outlive this controller.
    *alive_ = false;
    if (context_.services.lifecycle != nullptr) {
        context_.services.lifecycle->setCloseRequestHandler(nullptr);
        context_.services.lifecycle->setQuitRequestHandler(nullptr);
    }
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
    platform::ISaveDialog::Options options;
    options.suggestedName = tab.path().filename().string();
    options.title = "Save As";
    options.prompt = "Save";
    const std::optional<std::filesystem::path> chosen = context_.services.saveDialog->runSavePanel(options);
    if (!chosen.has_value()) return; // cancelled: nothing changes
    if (*chosen == tab.session()->path()) {
        save(tab);
        return;
    }
    // Refuse to clobber a document that is open in another tab.
    const std::size_t existing = context_.workspace.indexOfPath(*chosen);
    if (existing != DocumentWorkspace::kNoTab && context_.workspace.tab(existing)->id() != tab.id()) {
        setStatus_("A document with this path is already open in another tab");
        return;
    }
    requestSave(tab, *chosen, false, true);
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

    auto result = std::make_shared<std::optional<editor::DocumentWriteResult>>();
    editor::DocumentWriteJob captured = std::move(*job);
    scheduler_.post([this, result, captured = std::move(captured), request, alive = alive_] mutable {
        *result = editor::runDocumentWrite(engine_, captured);
        if (context_.services.mainDispatcher != nullptr) {
            context_.services.mainDispatcher->post(
                [this, result, request, alive] {
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
    auto result = std::make_shared<std::optional<editor::DocumentWriteResult>>();
    editor::DocumentWriteJob captured = std::move(*job);
    scheduler_.post([this, result, captured = std::move(captured), request, alive = alive_] mutable {
        *result = editor::runDocumentWrite(engine_, captured);
        if (context_.services.mainDispatcher != nullptr) {
            context_.services.mainDispatcher->post([this, result, request, alive] {
                if (!*alive) return;
                handleSaveCompleted(result, request);
            });
        } else {
            if (*alive) handleSaveCompleted(result, request);
        }
    });
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
    const editor::DocumentWriteResult& write = **result;
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
    platform::ISaveDialog::Options options;
    options.suggestedName = tab.path().filename().string();
    options.title = "Export Selected Pages";
    options.prompt = "Export";
    const std::optional<std::filesystem::path> chosen = context_.services.saveDialog->runSavePanel(options);
    if (!chosen.has_value()) return;

    core::Result<editor::DocumentWriteJob> job = editor::makeExtractJob(*tab.session(), pages, *chosen);
    if (!job.has_value()) {
        setStatus_("Could not prepare the export: " + describeFailure(job.error()));
        return;
    }
    setStatus_("Exporting…");
    auto result = std::make_shared<std::optional<editor::DocumentWriteResult>>();
    editor::DocumentWriteJob captured = std::move(*job);
    scheduler_.post([this, result, captured = std::move(captured), alive = alive_] mutable {
        *result = editor::runDocumentWrite(engine_, captured);
        if (context_.services.mainDispatcher != nullptr) {
            context_.services.mainDispatcher->post([this, result, alive] {
                if (!*alive) return;
                const editor::DocumentWriteResult& write = **result;
                if (write.written.has_value()) {
                    setStatus_("Exported");
                } else {
                    setStatus_("Export failed: " + describeFailure(write.written.error()));
                }
            });
        }
    });
}

// --- Import / merge -----------------------------------------------------------

void FileController::importPages(DocumentTab& tab, std::optional<std::size_t> beforeIndex) {
    if (tab.session() == nullptr || tab.state() != DocumentTab::State::Ready) return;
    if (context_.services.fileDialog == nullptr) {
        setStatus_(kUnavailable);
        return;
    }
    const core::Result<std::filesystem::path> chosen = context_.services.fileDialog->openPdf();
    if (!chosen.has_value()) {
        if (chosen.error().code != core::ErrorCode::Cancelled) {
            setStatus_("Could not choose a file: " + describeFailure(chosen.error()));
        }
        return;
    }
    startImport(tab, beforeIndex, *chosen);
}

void FileController::startImport(DocumentTab& tab, std::optional<std::size_t> beforeIndex,
                                 std::filesystem::path sourcePath) {
    const std::uint64_t generation = ++generationCounter_;
    liveImports_.push_back(generation);
    const TabId tabId = tab.id();
    setStatus_(std::format("Importing {}…", sourcePath.filename().string()));

    auto result = std::make_shared<core::Result<std::vector<editor::PageSource>>>(
        std::unexpected(core::Error{core::ErrorCode::NotAvailable, "pending", "import"}));
    scheduler_.post([this, result, sourcePath = std::move(sourcePath), tabId, generation, beforeIndex,
                     alive = alive_] mutable {
        // Worker: open the source and read its page metadata (PDFium calls
        // under the adapter's gate). The document stays alive through the
        // returned sources' shared pointers.
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

std::vector<DocumentTab*> FileController::dirtyTabs() const {
    std::vector<DocumentTab*> dirty;
    for (std::size_t i = 0; i < context_.workspace.tabCount(); ++i) {
        DocumentTab* tab = context_.workspace.tab(i);
        if (tab != nullptr && tab->session() != nullptr && tab->session()->isDirty()) {
            dirty.push_back(tab);
        }
    }
    return dirty;
}

void FileController::discardAndClose(DocumentTab* tab) {
    const std::size_t index = context_.workspace.indexOfTab(tab->id());
    if (index != DocumentWorkspace::kNoTab) context_.workspace.closeTab(index);
}

bool FileController::confirmCloseTab(DocumentTab& tab) {
    if (tab.session() == nullptr || !tab.session()->isDirty()) return true;

    // A prompt that cannot be asked (no alert service) must never discard
    // data silently: treat it as Cancel.
    if (context_.services.alerts == nullptr) return false;

    const platform::SaveChangesChoice choice = context_.services.alerts->askSaveChanges(tab.title());
    switch (choice) {
    case platform::SaveChangesChoice::Cancel:
        return false;
    case platform::SaveChangesChoice::DontSave:
        discardAndClose(&tab);
        return true;
    case platform::SaveChangesChoice::Save:
        if (isSaving(tab.id())) {
            // Keep the tab open; it closes when its save settles.
            if (std::find(closeAfterSave_.begin(), closeAfterSave_.end(), tab.id()) ==
                closeAfterSave_.end()) {
                closeAfterSave_.push_back(tab.id());
            }
            return false;
        }
        requestSave(tab, tab.session()->path(), true, true);
        return false;
    }
    return false;
}

bool FileController::confirmCloseWindow() {
    const std::vector<DocumentTab*> dirty = dirtyTabs();
    if (dirty.empty()) return true;
    if (context_.services.alerts == nullptr) return false;

    if (dirty.size() == 1) return confirmCloseTab(*dirty.front());

    const platform::ReviewChangesChoice choice =
        context_.services.alerts->askReviewUnsavedChanges(dirty.size());
    switch (choice) {
    case platform::ReviewChangesChoice::Cancel:
        return false;
    case platform::ReviewChangesChoice::DiscardAll:
        for (DocumentTab* tab : dirty) discardAndClose(tab);
        return true;
    case platform::ReviewChangesChoice::Review:
        // Sequential per-document prompts (app-modal). A tab answered "Save"
        // defers its close to its save completion; the close proceeds only
        // when every prompt was answered without cancelling.
        for (DocumentTab* tab : dirty) {
            if (tab->session() == nullptr || !tab->session()->isDirty()) continue;
            if (!confirmCloseTab(*tab)) return false;
        }
        return closeAfterSave_.empty();
    }
    return false;
}

void FileController::handleQuitRequest(platform::IAppLifecycle::QuitReply reply) {
    const std::vector<DocumentTab*> dirty = dirtyTabs();
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
        DocumentTab* tab = dirty.front();
        const platform::SaveChangesChoice choice =
            context_.services.alerts->askSaveChanges(tab->title());
        switch (choice) {
        case platform::SaveChangesChoice::Cancel:
            reply(false);
            return;
        case platform::SaveChangesChoice::DontSave:
            discardAndClose(tab);
            reply(true);
            return;
        case platform::SaveChangesChoice::Save:
            outstanding.push_back(tab->id());
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
            for (DocumentTab* tab : dirty) discardAndClose(tab);
            reply(true);
            return;
        case platform::ReviewChangesChoice::Review:
            for (DocumentTab* tab : dirty) {
                if (tab->session() == nullptr || !tab->session()->isDirty()) continue;
                const platform::SaveChangesChoice perTab =
                    context_.services.alerts->askSaveChanges(tab->title());
                if (perTab == platform::SaveChangesChoice::Cancel) {
                    reply(false);
                    return;
                }
                if (perTab == platform::SaveChangesChoice::DontSave) {
                    discardAndClose(tab);
                    continue;
                }
                outstanding.push_back(tab->id());
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
    pendingQuit_ = PendingQuit{std::move(reply), outstanding};
    for (const TabId tabId : outstanding) {
        DocumentTab* tab = context_.workspace.tabById(tabId);
        if (tab == nullptr || tab->session() == nullptr) {
            saveSettled(tabId, true); // vanished meanwhile: re-check the quit state
            continue;
        }
        if (isSaving(tabId)) continue;
        requestSave(*tab, tab->session()->path(), false, false);
    }
}

} // namespace rivet::app
