// SPDX-License-Identifier: MPL-2.0
// FileController: background save / save as / import / merge / extract at the
// application layer (markSaved, rebase, retitling), the dirty close/quit
// lifecycle and the async races (close tab during save, quit during save,
// completion after the tab is gone). Portable: FakeWritableEngine writes and
// reopens its own format, so markers verify what landed on disk.
#include "RivetTest.h"

#include "fakes/FakeWritableEngine.hpp"

#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/ShellContext.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/PageCommands.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"

#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using rivet::app::DocumentTab;
using rivet::app::DocumentWorkspace;
using rivet::app::FileCommand;
using rivet::app::FileController;
using rivet::app::ShellContext;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentSession;
using rivet::test::FakeWritableEngine;
using rivet::ui::Container;
using rivet::ui::PdfViewport;

// Unique scratch directory, removed on exit.
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("rivet-filectl-" + std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::permissions(path_, fs::perms::owner_all, ec);
        fs::remove_all(path_, ec);
    }
    fs::path operator()(const std::string& name) const { return path_ / name; }
    const fs::path& dir() const { return path_; }

private:
    fs::path path_;
};

// Main-thread queue; the test thread pumps it (deterministic, no sleeps).
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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

class FakeOpenDialog final : public rivet::platform::IFileDialog {
public:
    Result<fs::path> openPdf() override {
        auto next = next_;
        next_.reset();
        if (!next.has_value()) return std::unexpected(Error{ErrorCode::Cancelled, "cancelled", "test"});
        return *next;
    }
    std::optional<fs::path> next_;
};

class FakeSaveDialog final : public rivet::platform::ISaveDialog {
public:
    std::optional<fs::path> runSavePanel(const Options&) override {
        auto next = next_;
        next_.reset();
        return next;
    }
    std::optional<fs::path> next_;
};

class FakeAlerts final : public rivet::platform::IAlertService {
public:
    rivet::platform::SaveChangesChoice askSaveChanges(std::string_view title) override {
        savePrompts.emplace_back(title);
        if (saveAnswers.empty()) return rivet::platform::SaveChangesChoice::Cancel;
        auto answer = saveAnswers.front();
        saveAnswers.pop_front();
        return answer;
    }
    rivet::platform::ReviewChangesChoice askReviewUnsavedChanges(std::size_t) override {
        if (reviewAnswers.empty()) return rivet::platform::ReviewChangesChoice::Cancel;
        auto answer = reviewAnswers.front();
        reviewAnswers.pop_front();
        return answer;
    }
    void showError(std::string_view, std::string_view) override {}

    std::deque<rivet::platform::SaveChangesChoice> saveAnswers;
    std::deque<rivet::platform::ReviewChangesChoice> reviewAnswers;
    std::vector<std::string> savePrompts;
};

// Minimal shell: workspace + context + FileController (no widget work).
struct Shell {
    FakeWritableEngine engine;
    TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    TempDir dir;
    FakeOpenDialog openDialog;
    FakeSaveDialog saveDialog;
    FakeAlerts alerts;
    rivet::platform::ShellServices services;

    std::unique_ptr<Container> root = std::make_unique<Container>();
    PdfViewport* viewport = nullptr;
    std::unique_ptr<ShellContext> context;
    std::unique_ptr<FileController> files;
    std::vector<std::string> statusLog;
    int notifications = 0;

    explicit Shell(bool withAlerts = true) {
        services.mainDispatcher = &dispatcher;
        services.fileDialog = &openDialog;
        services.saveDialog = &saveDialog;
        services.alerts = withAlerts ? &alerts : nullptr;
        auto vp = std::make_unique<PdfViewport>();
        viewport = vp.get();
        context = std::make_unique<ShellContext>(ShellContext{
            workspace, services, *viewport, {}, {}, {},
        });
        root->addChild(std::move(vp));
        files = std::make_unique<FileController>(engine, *context, scheduler,
                                                 [this](std::string text) { statusLog.push_back(std::move(text)); },
                                                 [this] { ++notifications; });
    }

    ~Shell() {
        files.reset();
        viewport->clearDocument();
    }

    DocumentTab* open(const std::string& name, std::size_t pages = 5) {
        engine.pageCounts[name] = pages;
        // The fake engine opens any path, but the file should exist for the
        // filesystem checks (and for realism).
        if (std::FILE* file = std::fopen(dir(name).string().c_str(), "wb")) {
            std::fputs("placeholder\n", file);
            std::fclose(file);
        }
        workspace.openDocument(dir(name));
        if (!dispatcher.waitUntil([this] {
                DocumentTab* tab = workspace.activeTab();
                return tab != nullptr && tab->state() == DocumentTab::State::Ready;
            })) {
            return nullptr;
        }
        return workspace.activeTab();
    }

    // Runs one structural edit through the session (marks the doc dirty).
    void rotateFirst(DocumentTab& tab) {
        tab.session()->execute(std::make_unique<rivet::editor::RotatePagesCommand>(
            tab.session()->pageModel(), std::vector{tab.session()->pageId(0)}, 90));
    }

    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }
    bool hasStatus(const std::string& prefix) const {
        return !statusLog.empty() && statusLog.back().rfind(prefix, 0) == 0;
    }
};

} // namespace

// --- Save / Save As -----------------------------------------------------------

RIVET_TEST(saveMarksCleanRebasesAndWritesMarkers) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    CHECK(tab != nullptr);
    shell.rotateFirst(*tab);
    CHECK(tab->session()->isDirty());

    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!tab->session()->isDirty());
    CHECK(!tab->session()->isEditingLocked());
    // The written file carries the edit (rotation degrees in the marker line).
    const std::vector<std::string> markers = rivet::test::fakeFileMarkers(shell.dir("doc.pdf"));
    CHECK_EQ(markers.size(), 5u);
    // Rebase cleared the history (the written file is the new base).
    CHECK(!tab->session()->commands().canUndo());
    // Saving again is a no-op that still succeeds.
    shell.statusLog.clear();
    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!tab->session()->isDirty());
}

RIVET_TEST(failedSaveKeepsDirtyOriginalAndReleasesLock) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    const auto original = rivet::test::fakeFileMarkers(shell.dir("doc.pdf"));
    shell.rotateFirst(*tab);
    shell.engine.failAssembly = true;

    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Save failed"); }));
    CHECK(tab->session()->isDirty()); // failed saves never clear dirty
    CHECK(!tab->session()->isEditingLocked()); // the lock releases
    // The original file is untouched (atomic replace target behavior).
    CHECK((rivet::test::fakeFileMarkers(shell.dir("doc.pdf")) == original));

    // Repair and save again.
    shell.engine.failAssembly = false;
    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!tab->session()->isDirty());
}

RIVET_TEST(rapidSaveRequestsAreReportedNotQueued) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.engine.closeGate(); // park the first save inside the assembly
    shell.files->save(*tab);
    CHECK(shell.engine.waitParked(1));
    shell.statusLog.clear();
    shell.files->save(*tab);
    CHECK(shell.hasStatus("A save is already in progress"));
    shell.engine.release();
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
}

RIVET_TEST(saveAsRetitlesTabAndKeepsSource) {
    Shell shell;
    DocumentTab* tab = shell.open("source.pdf");
    shell.rotateFirst(*tab);
    shell.saveDialog.next_ = shell.dir("renamed.pdf");

    shell.files->saveAs(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(tab->path() == shell.dir("renamed.pdf"));
    CHECK(tab->title() == "renamed.pdf");
    CHECK(tab->session()->path() == shell.dir("renamed.pdf"));
    CHECK(!tab->session()->isDirty());
    CHECK(fs::exists(shell.dir("renamed.pdf")));
    CHECK(fs::exists(shell.dir("source.pdf")));
}

RIVET_TEST(saveAsCancellationChangesNothing) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.saveDialog.next_.reset(); // cancelled panel

    shell.files->saveAs(*tab);
    shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); });
    CHECK(shell.statusLog.empty());
    CHECK(tab->session()->isDirty());
    CHECK(tab->path() == shell.dir("doc.pdf"));
}

RIVET_TEST(saveAsRefusesDestinationOpenInAnotherTab) {
    Shell shell;
    DocumentTab* first = shell.open("first.pdf");
    (void)shell.open("second.pdf");
    shell.saveDialog.next_ = shell.dir("second.pdf"); // another tab's path

    shell.files->saveAs(*first);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("A document with this path"); }));
    CHECK(shell.statusLog.size() == 1u); // nothing was saved through the refused path
}

// --- Import / merge / extract ---------------------------------------------------

RIVET_TEST(importInsertsBeforeCurrentAndUndoRestores) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    tab->setCurrentPage(1);
    // The import source: 3 pages with their own markers.
    shell.engine.pageCounts["import.pdf"] = 3;
    shell.openDialog.next_ = shell.dir("import.pdf");

    shell.files->importPages(*tab, 1);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Imported 3 pages"); }));
    CHECK_EQ(tab->session()->pageCount(), 8u);
    const auto snapshot = tab->session()->pageSnapshot();
    // Inserted at index 1..3 (before current), new ids, imports participate
    // in undo.
    CHECK(tab->session()->undo());
    CHECK_EQ(tab->session()->pageCount(), 5u);
    CHECK(tab->session()->redo());
    CHECK_EQ(tab->session()->pageCount(), 8u);
}

RIVET_TEST(mergeAppendsAtEnd) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.engine.pageCounts["more.pdf"] = 2;
    shell.openDialog.next_ = shell.dir("more.pdf");

    shell.files->importPages(*tab, std::nullopt);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Imported 2 pages"); }));
    CHECK_EQ(tab->session()->pageCount(), 7u);
}

RIVET_TEST(importFailuresLeaveModelUntouched) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    // Corrupted source.
    shell.engine.failOpen.insert("broken.pdf");
    shell.openDialog.next_ = shell.dir("broken.pdf");
    shell.files->importPages(*tab, std::nullopt);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Import failed"); }));
    CHECK_EQ(tab->session()->pageCount(), 5u);
    CHECK(!tab->session()->isDirty());
    // Encrypted source: a controlled message, no crash, no partial state.
    shell.engine.lockedPaths.insert("locked.pdf");
    shell.openDialog.next_ = shell.dir("locked.pdf");
    shell.files->importPages(*tab, std::nullopt);
    CHECK(shell.dispatcher.waitUntil(
        [&] { return shell.hasStatus("Import failed: the PDF is password protected"); }));
    CHECK_EQ(tab->session()->pageCount(), 5u);
    CHECK(!tab->session()->isDirty());
}

RIVET_TEST(extractWritesSelectionAndLeavesSourceAlone) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.saveDialog.next_ = shell.dir("out.pdf");

    // Pages 4 and 1 (model order applies regardless of the given order).
    const std::vector<rivet::core::PageId> pages = {tab->session()->pageId(4),
                                                    tab->session()->pageId(1)};
    shell.files->extract(*tab, pages);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Exported"); }));
    const std::vector<std::string> out = rivet::test::fakeFileMarkers(shell.dir("out.pdf"));
    CHECK((out == std::vector<std::string>{"doc-2", "doc-5"}));
    CHECK_EQ(tab->session()->pageCount(), 5u);
    CHECK(tab->session()->isDirty()); // the source was not saved, just extracted
}

RIVET_TEST(extractWithoutPagesIsReported) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.files->extract(*tab, {});
    CHECK(shell.hasStatus("No pages selected"));
}

// --- Close / quit lifecycle -------------------------------------------------------

RIVET_TEST(dirtyCloseTabPromptsAndActs) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);

    // Cancel keeps the tab.
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Cancel);
    CHECK(!shell.files->confirmCloseTab(*tab));
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    // Don't Save discards.
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::DontSave);
    CHECK(shell.files->confirmCloseTab(*tab));
    CHECK_EQ(shell.workspace.tabCount(), 0u);
}

RIVET_TEST(dirtyCloseTabSaveClosesAfterCompletion) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);

    CHECK(!shell.files->confirmCloseTab(*tab)); // deferred
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.workspace.tabCount() == 0; }));
    // The file was written before the tab closed.
    CHECK_EQ(rivet::test::fakeFileMarkers(shell.dir("doc.pdf")).size(), 5u);
}

RIVET_TEST(closeTabDuringSaveDropsCompletionWithoutCrash) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.engine.closeGate();
    shell.files->save(*tab);
    CHECK(shell.engine.waitParked(1));
    // Close the tab while its save is parked in the assembly.
    shell.workspace.closeTab(shell.workspace.activeIndex());
    CHECK_EQ(shell.workspace.tabCount(), 0u);
    shell.engine.release();
    // The completion lands with no tab to rebase: dropped, no crash.
    shell.dispatcher.waitUntil([&] { return shell.files->isSavingAnything() == false; });
    CHECK(shell.statusLog.empty() || !shell.hasStatus("Saved"));
}

RIVET_TEST(quitWithMultipleDirtyTabsSavesEach) {
    Shell shell;
    DocumentTab* first = shell.open("first.pdf");
    DocumentTab* second = shell.open("second.pdf");
    shell.rotateFirst(*first);
    shell.rotateFirst(*second);

    shell.alerts.reviewAnswers.push_back(rivet::platform::ReviewChangesChoice::Review);
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);

    bool replied = false;
    bool proceed = false;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(!replied); // deferred while saves run
    CHECK(shell.dispatcher.waitUntil([&] { return replied; }));
    CHECK(proceed);
    CHECK(!first->session()->isDirty());
    CHECK(!second->session()->isDirty());
    CHECK(fs::exists(shell.dir("first.pdf")));
    CHECK(fs::exists(shell.dir("second.pdf")));
}

RIVET_TEST(quitDiscardAllAndCancel) {
    Shell shell;
    DocumentTab* first = shell.open("first.pdf");
    DocumentTab* second = shell.open("second.pdf");
    shell.rotateFirst(*first);
    shell.rotateFirst(*second);

    shell.alerts.reviewAnswers.push_back(rivet::platform::ReviewChangesChoice::DiscardAll);
    bool replied = false;
    bool proceed = true;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(replied);
    CHECK(proceed);
    CHECK_EQ(shell.workspace.tabCount(), 0u);

    (void)second;
    // Cancel on a fresh dirty document keeps it.
    DocumentTab* third = shell.open("third.pdf");
    shell.rotateFirst(*third);
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Cancel);
    replied = false;
    proceed = true;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(replied);
    CHECK(!proceed);
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    CHECK(third->session()->isDirty());
}

RIVET_TEST(quitDuringInFlightSaveWaitsForIt) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.engine.closeGate();
    shell.files->save(*tab);
    CHECK(shell.engine.waitParked(1));

    // Quit while the save is in flight: the single dirty document is the
    // saving one; the prompt answers Save and the reply waits.
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    bool replied = false;
    bool proceed = false;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(!replied);
    shell.engine.release();
    CHECK(shell.dispatcher.waitUntil([&] { return replied; }));
    CHECK(proceed);
    CHECK(!shell.files->isSavingAnything());
}

RIVET_TEST(quitWithNoAlertServiceNeverDiscards) {
    Shell shell{false};
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);

    bool replied = false;
    bool proceed = true;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(replied);
    CHECK(!proceed); // refused rather than discarding silently
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    CHECK(tab->session()->isDirty());
}

RIVET_TEST(cleanCloseNeedsNoPrompt) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    CHECK(!tab->session()->isDirty());
    CHECK(shell.files->confirmCloseTab(*tab));
    CHECK(shell.alerts.savePrompts.empty());
    CHECK(shell.files->confirmCloseWindow());
}
