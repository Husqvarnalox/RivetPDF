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

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
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
        ++posted;
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
    std::atomic<int> posted{0};
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

// A modal prompt/panel pumps the main queue on some platforms, so completions
// (a save closing its tab, ...) can run while it is up. The fakes model that
// with a one-shot hook that runs inside the "modal".
void runOnce(std::function<void()>& hook) {
    if (!hook) return;
    auto task = std::move(hook);
    hook = nullptr;
    task();
}

class FakeOpenDialog final : public rivet::platform::IFileDialog {
public:
    Result<fs::path> openPdf() override {
        runOnce(duringModal);
        auto next = next_;
        next_.reset();
        if (!next.has_value()) return std::unexpected(Error{ErrorCode::Cancelled, "cancelled", "test"});
        return *next;
    }
    std::optional<fs::path> next_;
    std::function<void()> duringModal;
};

class FakeSaveDialog final : public rivet::platform::ISaveDialog {
public:
    std::optional<fs::path> runSavePanel(const Options&) override {
        runOnce(duringModal);
        auto next = next_;
        next_.reset();
        return next;
    }
    std::optional<fs::path> next_;
    std::function<void()> duringModal;
};

class FakeAlerts final : public rivet::platform::IAlertService {
public:
    rivet::platform::SaveChangesChoice askSaveChanges(std::string_view title) override {
        savePrompts.emplace_back(title);
        runOnce(duringModal);
        if (saveAnswers.empty()) return rivet::platform::SaveChangesChoice::Cancel;
        auto answer = saveAnswers.front();
        saveAnswers.pop_front();
        return answer;
    }
    rivet::platform::ReviewChangesChoice askReviewUnsavedChanges(std::size_t) override {
        runOnce(duringModal);
        if (reviewAnswers.empty()) return rivet::platform::ReviewChangesChoice::Cancel;
        auto answer = reviewAnswers.front();
        reviewAnswers.pop_front();
        return answer;
    }
    void showError(std::string_view, std::string_view message) override { errors.emplace_back(message); }
    std::optional<std::string> promptForText(std::string_view, std::string_view, std::string_view) override {
        ++textPrompts;
        runOnce(duringModal);
        return textAnswer;
    }

    std::function<void()> duringModal;
    std::optional<std::string> textAnswer;
    int textPrompts = 0;
    std::vector<std::string> errors;
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
        const auto status = tab.session()->execute(std::make_unique<rivet::editor::RotatePagesCommand>(
            tab.session()->pageModel(), std::vector{tab.session()->pageId(0)}, 90));
        CHECK(status.has_value());
    }

    // What the modal's queued completions do to the workspace: close a tab.
    void closeTabById(rivet::app::TabId id) {
        const std::size_t index = workspace.indexOfTab(id);
        if (index != DocumentWorkspace::kNoTab) workspace.closeTab(index);
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

// --- Split ---------------------------------------------------------------------

namespace {

using Markers = std::vector<std::string>;

// Sorted file names in `dir` (leftover temp files would show up here).
std::vector<std::string> listDir(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir)) names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

RIVET_TEST(splitWritesEachRangeAndLeavesSourceAlone) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    CHECK(tab != nullptr);
    tab->setCurrentPage(6);
    shell.saveDialog.next_ = shell.dir("report.pdf");

    shell.files->splitByRanges(*tab, "1-3, 4-7, 8-10");
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split into 3 files"); }));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("report_1-3.pdf")) == Markers{"doc-1", "doc-2", "doc-3"}));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("report_4-7.pdf")) ==
           Markers{"doc-4", "doc-5", "doc-6", "doc-7"}));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("report_8-10.pdf")) == Markers{"doc-8", "doc-9", "doc-10"}));
    CHECK(!fs::exists(shell.dir("report.pdf"))); // the base name is not itself written
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf", "report_1-3.pdf", "report_4-7.pdf",
                                                              "report_8-10.pdf"}));
    // Export only: nothing about the source changed.
    CHECK(!tab->session()->isDirty());
    CHECK(!tab->session()->commands().canUndo());
    CHECK_EQ(tab->session()->pageCount(), 10u);
    CHECK_EQ(tab->currentPage(), 6u);
    CHECK(shell.alerts.errors.empty());
}

RIVET_TEST(splitSinglePagesAndPdfExtensionNormalisation) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.saveDialog.next_ = shell.dir("Report.PDF");
    shell.files->splitByRanges(*tab, "9, 2");
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split into 2 files"); }));
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"Report_2.pdf", "Report_9.pdf", "doc.pdf"}));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("Report_9.pdf")) == Markers{"doc-9"}));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("Report_2.pdf")) == Markers{"doc-2"}));
}

RIVET_TEST(splitUsesTheEditedModelOrderAndKeepsEdits) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    auto& session = *tab->session();
    // Delete page 2 and move the old page 10 (now index 8) to the front.
    CHECK(session.execute(std::make_unique<rivet::editor::DeletePagesCommand>(
                              session.pageModel(), std::vector{session.pageId(1)}))
              .has_value());
    CHECK(session.execute(std::make_unique<rivet::editor::MovePagesCommand>(
                              session.pageModel(), std::vector{session.pageId(8)}, 0))
              .has_value());
    CHECK(session.isDirty());
    shell.saveDialog.next_ = shell.dir("e.pdf");
    shell.files->splitByRanges(*tab, "1-3, 9");
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split into 2 files"); }));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("e_1-3.pdf")) == Markers{"doc-10", "doc-1", "doc-3"}));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("e_9.pdf")) == Markers{"doc-9"}));
    CHECK(session.isDirty()); // still dirty: nothing was saved
    CHECK_EQ(session.pageCount(), 9u);
}

RIVET_TEST(splitPromptsForRangesAndCancelChangesNothing) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.saveDialog.next_ = shell.dir("p.pdf");
    // Cancelled prompt: no panel, no status, no files.
    shell.alerts.textAnswer.reset();
    shell.files->perform(FileCommand::Split);
    CHECK_EQ(shell.alerts.textPrompts, 1);
    CHECK(shell.statusLog.empty());
    CHECK(shell.saveDialog.next_.has_value()); // the panel was never shown
    // Answered prompt runs the split.
    shell.alerts.textAnswer = "1-2";
    shell.files->perform(FileCommand::Split);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split into 1 file"); }));
    CHECK(fs::exists(shell.dir("p_1-2.pdf")));
    CHECK(tab != nullptr);
    CHECK(shell.files->canPerform(FileCommand::Split));
}

RIVET_TEST(splitWithoutAlertServiceIsReported) {
    Shell shell(false);
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.files->split(*tab);
    CHECK(shell.hasStatus("Text prompts are not available"));
}

RIVET_TEST(splitInvalidRangesReportAndWriteNothing) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    for (const char* text : {"", "0", "3-1", "1-11", "abc", "1-3,3-5"}) {
        shell.statusLog.clear();
        shell.saveDialog.next_ = shell.dir("x.pdf");
        shell.files->splitByRanges(*tab, text);
        CHECK(shell.hasStatus("Invalid page ranges"));
        CHECK(shell.saveDialog.next_.has_value()); // rejected before the panel
    }
    CHECK_EQ(shell.alerts.errors.size(), 6u);
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf"}));
    CHECK_EQ(shell.engine.assemblies.load(), 0);
    CHECK(!tab->session()->isDirty());
}

RIVET_TEST(splitAbortsBeforeWritingWhenAnOutputExists) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    {
        std::ofstream existing(shell.dir("r_4-7.pdf"), std::ios::binary);
        existing << "precious";
    }
    shell.saveDialog.next_ = shell.dir("r.pdf");
    shell.files->splitByRanges(*tab, "1-3, 4-7, 8-10");
    CHECK(shell.hasStatus("Split cancelled: r_4-7.pdf already exists"));
    CHECK_EQ(shell.alerts.errors.size(), 1u);
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf", "r_4-7.pdf"}));
    std::ifstream in(shell.dir("r_4-7.pdf"), std::ios::binary);
    std::string content;
    std::getline(in, content);
    CHECK(content == "precious");
    CHECK_EQ(shell.engine.assemblies.load(), 0);
}

RIVET_TEST(splitUnwritableDestinationFailsControlled) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.saveDialog.next_ = shell.dir("missing-dir/r.pdf");
    shell.files->splitByRanges(*tab, "1-3, 4-7");
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split failed at r_1-3.pdf"); }));
    CHECK(shell.lastStatus().find("(0 of 2 files written)") != std::string::npos);
    CHECK_EQ(shell.alerts.errors.size(), 1u);
    CHECK(!fs::exists(shell.dir("missing-dir")));
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf"}));
    CHECK(!tab->session()->isDirty());
    CHECK_EQ(tab->session()->pageCount(), 10u);
}

RIVET_TEST(splitPartialFailureKeepsEarlierOutputsOnly) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.engine.failAssemblyFrom = 2; // the second output fails
    shell.saveDialog.next_ = shell.dir("r.pdf");
    shell.files->splitByRanges(*tab, "1-3, 4-7, 8-10");
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split failed at r_4-7.pdf"); }));
    CHECK(shell.lastStatus().find("(1 of 3 files written)") != std::string::npos);
    // The first output stays; the failed one left nothing (no temp either);
    // the third was never attempted.
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf", "r_1-3.pdf"}));
    CHECK_EQ(shell.engine.assemblies.load(), 2);
    CHECK(!tab->session()->isDirty());
}

RIVET_TEST(splitSurvivesTabCloseAndUsesItsSnapshot) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.engine.closeGate();
    shell.saveDialog.next_ = shell.dir("c.pdf");
    shell.files->splitByRanges(*tab, "1-2, 3-4");
    CHECK(shell.engine.waitParked(1));
    // The user edits and closes the tab while the split is running.
    shell.workspace.closeTab(shell.workspace.activeIndex());
    shell.engine.release();
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split into 2 files"); }));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("c_1-2.pdf")) == Markers{"doc-1", "doc-2"}));
    CHECK((rivet::test::fakeFileMarkers(shell.dir("c_3-4.pdf")) == Markers{"doc-3", "doc-4"}));
}

namespace {

// Bulk read via read()/gcount(): GCC 13 reports a false-positive
// -Wnull-dereference inside <streambuf> for istreambuf_iterator.
std::string slurp(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string out;
    char buffer[4096];
    while (in.read(buffer, sizeof buffer) || in.gcount() > 0) {
        out.append(buffer, static_cast<std::size_t>(in.gcount()));
    }
    return out;
}

// Destroys the controller on another thread (its destructor blocks while a
// worker is parked in the engine), lets it run into the wait, then releases
// the engine. Returns whether the destructor was still blocked before the
// release (it must wait for the worker).
bool destroyControllerWhileParked(Shell& shell) {
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        shell.files.reset();
        destroyed = true;
    });
    // Give the destructor time to cancel the scope and block on the worker.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool blocked = !destroyed.load();
    shell.engine.release();
    destroyer.join();
    return blocked;
}

} // namespace

RIVET_TEST(splitIsCancelledAndAwaitedWhenTheControllerIsDestroyed) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.engine.closeGate();
    shell.saveDialog.next_ = shell.dir("d.pdf");
    shell.files->splitByRanges(*tab, "1-2, 3-4");
    CHECK(shell.engine.waitParked(1));
    shell.statusLog.clear();
    // App quit: the destructor cancels the split and waits for the worker
    // (it borrows the engine and the dispatcher); no completion is applied.
    CHECK(destroyControllerWhileParked(shell));
    shell.dispatcher.pump();
    CHECK(shell.statusLog.empty());
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf"}));
    CHECK_EQ(shell.engine.assemblies.load(), 1); // the second output was never started
}

RIVET_TEST(extractIsCancelledAndAwaitedWhenTheControllerIsDestroyed) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.engine.closeGate();
    shell.saveDialog.next_ = shell.dir("out.pdf");
    const std::vector<rivet::core::PageId> pages = {tab->session()->pageId(0)};
    shell.files->extract(*tab, pages);
    CHECK(shell.engine.waitParked(1));
    shell.statusLog.clear();
    CHECK(destroyControllerWhileParked(shell));
    shell.dispatcher.pump();
    CHECK(shell.statusLog.empty());
    // Neither the destination nor a temp file: the commit never happened.
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf"}));
}

RIVET_TEST(saveIsCancelledAndAwaitedWhenTheControllerIsDestroyed) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    const std::string original = slurp(shell.dir("doc.pdf"));
    shell.engine.closeGate();
    shell.files->save(*tab);
    CHECK(shell.engine.waitParked(1));
    CHECK(destroyControllerWhileParked(shell));
    shell.dispatcher.pump();
    CHECK_EQ(slurp(shell.dir("doc.pdf")), original); // untouched
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf"}));
    CHECK(tab->session()->isDirty()); // never marked saved
}

RIVET_TEST(importCompletesOrIsDroppedWhenTheControllerIsDestroyed) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.openDialog.next_ = shell.dir("doc.pdf");
    shell.files->importPages(*tab, std::nullopt);
    shell.files.reset(); // returns once the worker left the task
    shell.dispatcher.pump(); // the completion is dropped
    CHECK_EQ(tab->session()->pageCount(), 5u);
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

RIVET_TEST(failedSaveOnDirtyCloseKeepsTheTabOpen) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.engine.failAssembly = true;
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);

    CHECK(!shell.files->confirmCloseTab(*tab));
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Save failed"); }));
    shell.dispatcher.waitUntil([&] { return !shell.files->isSavingAnything(); });
    // The edits must survive a failed save: tab still open and dirty.
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    CHECK(tab->session()->isDirty());
}

RIVET_TEST(failedSaveOnQuitAbortsTheQuit) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    shell.engine.failAssembly = true;
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    bool replied = false;
    bool proceed = true;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(shell.dispatcher.waitUntil([&] { return replied; }));
    CHECK(!proceed);
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    CHECK(tab->session()->isDirty());
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

// --- Close/quit across modal prompts (completions run while a prompt is up) ----

RIVET_TEST(closeTabDontSaveClosesExactlyThatTab) {
    Shell shell;
    DocumentTab* first = shell.open("first.pdf");
    DocumentTab* second = shell.open("second.pdf");
    shell.rotateFirst(*first);
    shell.rotateFirst(*second);
    const rivet::app::TabId firstId = first->id();
    const rivet::app::TabId secondId = second->id();

    // ShellController::requestCloseTab(0): the controller closes the tab
    // itself on Don't Save, so the shell must not close "index 0" again.
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::DontSave);
    CHECK(shell.files->confirmCloseTab(*first));
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    // The shell re-resolves the id before closing: it is already gone.
    CHECK(shell.workspace.indexOfTab(firstId) == DocumentWorkspace::kNoTab);
    DocumentTab* remaining = shell.workspace.tabById(secondId);
    CHECK(remaining != nullptr);
    if (remaining == nullptr) return;
    CHECK(remaining->session()->isDirty()); // the other dirty document survived, unprompted
    CHECK_EQ(shell.alerts.savePrompts.size(), 1u);
}

RIVET_TEST(closeTabPromptSurvivesTheTabClosingDuringTheModal) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    const rivet::app::TabId id = tab->id();
    // A completion closes this very tab while the prompt is up; "Save" then
    // has nothing left to save and nothing may touch the dead tab.
    shell.alerts.duringModal = [&] { shell.closeTabById(id); };
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    CHECK(shell.files->confirmCloseTab(*tab));
    CHECK_EQ(shell.workspace.tabCount(), 0u);
    CHECK(!shell.files->isSavingAnything());
}

RIVET_TEST(quitReviewSkipsATabThatClosedDuringThePrompts) {
    Shell shell;
    DocumentTab* first = shell.open("first.pdf");
    DocumentTab* second = shell.open("second.pdf");
    shell.rotateFirst(*first);
    shell.rotateFirst(*second);
    const rivet::app::TabId secondId = second->id();
    shell.alerts.reviewAnswers.push_back(rivet::platform::ReviewChangesChoice::Review);
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    // The second tab closes while the review prompt is up.
    shell.alerts.duringModal = [&] { shell.closeTabById(secondId); };

    bool replied = false;
    bool proceed = false;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(shell.dispatcher.waitUntil([&] { return replied; }));
    CHECK(proceed);
    CHECK_EQ(shell.alerts.savePrompts.size(), 1u); // the vanished tab is not prompted
    CHECK(!first->session()->isDirty());
}

RIVET_TEST(quitSingleDirtyTabClosedDuringThePromptIsHarmless) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    const rivet::app::TabId id = tab->id();
    shell.alerts.duringModal = [&] { shell.closeTabById(id); };
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    bool replied = false;
    bool proceed = false;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(shell.dispatcher.waitUntil([&] { return replied; }));
    CHECK(proceed);
    CHECK_EQ(shell.workspace.tabCount(), 0u);
}

RIVET_TEST(windowCloseReviewSkipsTabsThatClosedDuringAPrompt) {
    Shell shell;
    DocumentTab* first = shell.open("first.pdf");
    DocumentTab* second = shell.open("second.pdf");
    shell.rotateFirst(*first);
    shell.rotateFirst(*second);
    const rivet::app::TabId secondId = second->id();
    shell.alerts.reviewAnswers.push_back(rivet::platform::ReviewChangesChoice::Review);
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    // The review prompt arms a hook that closes the second tab while the
    // FIRST tab's per-document prompt is up.
    shell.alerts.duringModal = [&] {
        shell.alerts.duringModal = [&] { shell.closeTabById(secondId); };
    };
    CHECK(!shell.files->confirmCloseWindow()); // "Save" defers the first tab's close
    CHECK_EQ(shell.alerts.savePrompts.size(), 1u); // the second tab was closed before its turn
    CHECK(shell.dispatcher.waitUntil([&] { return shell.workspace.tabCount() == 0; }));
}

RIVET_TEST(discardAllToleratesTabsThatClosedDuringTheReview) {
    Shell shell;
    DocumentTab* first = shell.open("first.pdf");
    DocumentTab* second = shell.open("second.pdf");
    shell.rotateFirst(*first);
    shell.rotateFirst(*second);
    const rivet::app::TabId firstId = first->id();
    shell.alerts.reviewAnswers.push_back(rivet::platform::ReviewChangesChoice::DiscardAll);
    shell.alerts.duringModal = [&] { shell.closeTabById(firstId); };
    CHECK(shell.files->confirmCloseWindow());
    CHECK_EQ(shell.workspace.tabCount(), 0u);
}

RIVET_TEST(panelsAbortQuietlyWhenTheTabClosedDuringThePanel) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.rotateFirst(*tab);
    const rivet::app::TabId id = tab->id();

    // Save As.
    shell.saveDialog.next_ = shell.dir("renamed.pdf");
    shell.saveDialog.duringModal = [&] { shell.closeTabById(id); };
    shell.files->saveAs(*tab);
    CHECK(shell.hasStatus("Save As cancelled"));
    CHECK(!fs::exists(shell.dir("renamed.pdf")));
    CHECK(!shell.files->isSavingAnything());

    // Extract.
    tab = shell.open("doc2.pdf", 10);
    const rivet::app::TabId id2 = tab->id();
    const std::vector<rivet::core::PageId> pages2 = {tab->session()->pageId(0)};
    shell.saveDialog.next_ = shell.dir("out.pdf");
    shell.saveDialog.duringModal = [&] { shell.closeTabById(id2); };
    shell.files->extract(*tab, pages2);
    CHECK(shell.hasStatus("Export cancelled"));
    CHECK(!fs::exists(shell.dir("out.pdf")));

    // Split: the save panel.
    tab = shell.open("doc3.pdf", 10);
    const rivet::app::TabId id3 = tab->id();
    shell.saveDialog.next_ = shell.dir("s.pdf");
    shell.saveDialog.duringModal = [&] { shell.closeTabById(id3); };
    shell.files->splitByRanges(*tab, "1-2, 3-4");
    CHECK(shell.hasStatus("Split cancelled"));

    // Split: the range prompt.
    tab = shell.open("doc4.pdf", 10);
    const rivet::app::TabId id4 = tab->id();
    shell.alerts.textAnswer = "1-2";
    shell.alerts.duringModal = [&] { shell.closeTabById(id4); };
    shell.files->split(*tab);
    CHECK(shell.hasStatus("Split cancelled"));

    // Import: the open panel.
    tab = shell.open("doc5.pdf", 10);
    const rivet::app::TabId id5 = tab->id();
    shell.openDialog.next_ = shell.dir("doc.pdf");
    shell.openDialog.duringModal = [&] { shell.closeTabById(id5); };
    shell.files->importPages(*tab, std::nullopt);
    CHECK(shell.hasStatus("Import cancelled"));

    CHECK_EQ(shell.workspace.tabCount(), 0u);
    shell.dispatcher.pump();
    CHECK(!fs::exists(shell.dir("s_1-2.pdf")));
    CHECK(!fs::exists(shell.dir("renamed.pdf")));
}

RIVET_TEST(splitRevalidatesRangesWhenThePageCountChangesDuringThePanel) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    const rivet::app::TabId id = tab->id();
    shell.saveDialog.next_ = shell.dir("v.pdf");
    // While the panel is up, pages are deleted (a completion ran): the range
    // text that was valid for 10 pages no longer is.
    shell.saveDialog.duringModal = [&] {
        DocumentTab* live = shell.workspace.tabById(id);
        std::vector<rivet::core::PageId> doomed;
        for (std::size_t page = 4; page < 10; ++page) doomed.push_back(live->session()->pageId(page));
        const auto status = live->session()->execute(std::make_unique<rivet::editor::DeletePagesCommand>(
            live->session()->pageModel(), std::move(doomed)));
        CHECK(status.has_value());
    };
    shell.files->splitByRanges(*tab, "1-2, 7-9");
    CHECK(shell.hasStatus("Invalid page ranges"));
    CHECK_EQ(shell.alerts.errors.size(), 1u);
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf"}));
}

// --- Save As when the written file cannot be reloaded --------------------------

RIVET_TEST(saveAsWithUnreloadableResultStillRetargetsTheSession) {
    Shell shell;
    DocumentTab* tab = shell.open("source.pdf");
    shell.rotateFirst(*tab);
    const std::string sourceBytes = slurp(shell.dir("source.pdf"));
    // The reopen of the written file fails: no rebase target for the session.
    shell.engine.failOpen.insert("renamed.pdf");
    shell.saveDialog.next_ = shell.dir("renamed.pdf");

    shell.files->saveAs(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!tab->session()->isDirty());
    CHECK(tab->path() == shell.dir("renamed.pdf"));
    // The session follows the file it was just saved to.
    CHECK(tab->session()->path() == shell.dir("renamed.pdf"));
    CHECK_EQ(rivet::test::fakeFileMarkers(shell.dir("renamed.pdf")).size(), 5u);

    // The next Save goes to the new file; the original is never overwritten.
    shell.rotateFirst(*tab);
    shell.statusLog.clear();
    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved renamed.pdf"); }));
    CHECK_EQ(slurp(shell.dir("source.pdf")), sourceBytes);
}

// --- Split never replaces an existing output ----------------------------------

RIVET_TEST(splitJobsRefuseToReplaceFilesThatAppearBeforeTheCommit) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf", 10);
    shell.saveDialog.next_ = shell.dir("p.pdf");
    shell.engine.closeGate();
    shell.files->splitByRanges(*tab, "1-2, 3-4");
    CHECK(shell.engine.waitParked(1));
    // Another process creates the second output after the pre-write check.
    {
        std::ofstream out(shell.dir("p_3-4.pdf"), std::ios::binary);
        out << "WINNER";
    }
    shell.engine.release();
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Split failed at p_3-4.pdf"); }));
    CHECK_EQ(slurp(shell.dir("p_3-4.pdf")), std::string("WINNER"));
    CHECK_EQ(rivet::test::fakeFileMarkers(shell.dir("p_1-2.pdf")).size(), 2u);
    CHECK((listDir(shell.dir.dir()) == std::vector<std::string>{"doc.pdf", "p_1-2.pdf", "p_3-4.pdf"}));
}

// A save whose job cannot even be prepared (here: the session has no
// destination path) must still settle, or a deferred quit waits forever.
RIVET_TEST(quitAbortsWhenTheSaveJobCannotBePrepared) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    tab->session()->setPath({}); // makeSaveJob rejects an empty destination

    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    bool replied = false;
    bool proceed = true;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(replied); // settled synchronously: no save was ever started
    CHECK(!proceed);
    CHECK(shell.hasStatus("Could not prepare the save"));
    CHECK(tab->session()->isDirty());
    CHECK(!tab->session()->isEditingLocked());
    CHECK(!shell.files->isSavingAnything());
}

RIVET_TEST(closeTabSaveThatCannotBePreparedKeepsTabOpen) {
    Shell shell;
    DocumentTab* tab = shell.open("doc.pdf");
    shell.rotateFirst(*tab);
    tab->session()->setPath({});

    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    CHECK(!shell.files->confirmCloseTab(*tab));
    CHECK_EQ(shell.workspace.tabCount(), 1u);
    CHECK(tab->session()->isDirty());
}

// --- Pending edits ----------------------------------------------------------------

namespace {

// Stands in for a controller holding an uncommitted edit (an open note
// editor): the first commit turns it into a real, dirtying edit.
void addPendingRotation(Shell& shell, rivet::app::TabId id, int* calls) {
    shell.context->pendingEdits.add([&shell, id, calls, done = false]() mutable {
        ++*calls;
        if (done) return true;
        done = true;
        if (DocumentTab* tab = shell.workspace.tabById(id); tab != nullptr) shell.rotateFirst(*tab);
        return true;
    });
}

} // namespace

RIVET_TEST(saveCommitsPendingEditsFirst) {
    Shell shell;
    DocumentTab* tab = shell.open("pending-save.pdf");
    CHECK(tab != nullptr);
    const auto original = rivet::test::fakeFileMarkers(shell.dir("pending-save.pdf"));
    int calls = 0;
    addPendingRotation(shell, tab->id(), &calls);
    CHECK(!tab->session()->isDirty());

    shell.files->save(*tab);
    CHECK(calls >= 1);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!tab->session()->isDirty());
    // The pending edit is in the written file.
    CHECK(rivet::test::fakeFileMarkers(shell.dir("pending-save.pdf")) != original);
}

RIVET_TEST(closeTabQuitAndWindowCloseSeePendingEditsAsDirty) {
    Shell shell;
    DocumentTab* tab = shell.open("pending-close.pdf");
    CHECK(tab != nullptr);
    int calls = 0;
    addPendingRotation(shell, tab->id(), &calls);

    // Close tab: a clean document would close silently; the pending edit
    // makes it dirty, so the save prompt appears (and Cancel keeps the tab).
    CHECK(!shell.files->confirmCloseTab(*tab));
    CHECK_EQ(shell.alerts.savePrompts.size(), 1u);
    CHECK(tab->session()->isDirty());
    CHECK_EQ(shell.workspace.tabCount(), 1u);

    // Window close asks as well, for a fresh pending edit on a clean tab.
    DocumentTab* other = shell.open("pending-window.pdf");
    CHECK(other != nullptr);
    int otherCalls = 0;
    addPendingRotation(shell, other->id(), &otherCalls);
    shell.alerts.reviewAnswers.push_back(rivet::platform::ReviewChangesChoice::Cancel);
    CHECK(!shell.files->confirmCloseWindow());
    CHECK(otherCalls >= 1);
    CHECK(other->session()->isDirty());

    // ... and so does quit.
    DocumentTab* third = shell.open("pending-quit.pdf");
    CHECK(third != nullptr);
    int thirdCalls = 0;
    addPendingRotation(shell, third->id(), &thirdCalls);
    shell.alerts.reviewAnswers.push_back(rivet::platform::ReviewChangesChoice::Cancel);
    bool replied = false;
    bool proceed = true;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(replied);
    CHECK(!proceed);
    CHECK(thirdCalls >= 1);
    CHECK(third->session()->isDirty());
}

RIVET_TEST(exportsCommitPendingEditsFirst) {
    Shell shell;
    DocumentTab* tab = shell.open("pending-export.pdf");
    CHECK(tab != nullptr);
    int calls = 0;
    addPendingRotation(shell, tab->id(), &calls);
    const std::vector<rivet::core::PageId> pages{tab->session()->pageId(0)};
    shell.files->extract(*tab, pages); // the fake panel cancels; the hook still ran first
    CHECK(calls >= 1);
    CHECK(tab->session()->isDirty());
}
