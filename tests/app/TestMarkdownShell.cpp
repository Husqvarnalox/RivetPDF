// SPDX-License-Identifier: MPL-2.0
// Markdown document kind in the shell: kind detection, the UTF-8/BOM/line-ending
// codec, async open through the workspace (no PDF backend needed), save / save
// as / close / quit through FileController (atomic writer, failed saves),
// undo-driven dirty checkpoints, and ShellController binding when the active
// tab alternates between PDF and Markdown.
#include "RivetTest.h"

#include "fakes/FakeWritableEngine.hpp"

#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/MarkdownTabState.hpp"
#include "app/ShellContext.hpp"
#include "app/ShellController.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace rivet::app;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::Result;
using rivet::core::TaskScheduler;
using rivet::test::FakeWritableEngine;

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("rivet-mdshell-" + std::to_string(++counter));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path operator()(const std::string& name) const { return path_ / name; }

private:
    fs::path path_;
};

void writeBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string readBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

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

class FakeSaveDialog final : public rivet::platform::ISaveDialog {
public:
    std::optional<fs::path> runSavePanel(const Options& options) override {
        lastOptions = options;
        auto next = next_;
        next_.reset();
        return next;
    }
    std::optional<fs::path> next_;
    Options lastOptions;
};

class FakeAlerts final : public rivet::platform::IAlertService {
public:
    rivet::platform::SaveChangesChoice askSaveChanges(std::string_view) override {
        if (saveAnswers.empty()) return rivet::platform::SaveChangesChoice::Cancel;
        auto answer = saveAnswers.front();
        saveAnswers.pop_front();
        return answer;
    }
    rivet::platform::ReviewChangesChoice askReviewUnsavedChanges(std::size_t) override {
        return rivet::platform::ReviewChangesChoice::Cancel;
    }
    void showError(std::string_view, std::string_view message) override { errors.emplace_back(message); }
    std::optional<std::string> promptForText(std::string_view, std::string_view, std::string_view) override {
        return std::nullopt;
    }
    std::deque<rivet::platform::SaveChangesChoice> saveAnswers;
    std::vector<std::string> errors;
};

struct Shell {
    FakeWritableEngine engine;
    TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    TempDir dir;
    FakeSaveDialog saveDialog;
    FakeAlerts alerts;
    rivet::platform::ShellServices services;
    std::unique_ptr<rivet::ui::Container> root = std::make_unique<rivet::ui::Container>();
    rivet::ui::PdfViewport* viewport = nullptr;
    std::unique_ptr<ShellContext> context;
    std::unique_ptr<FileController> files;
    std::vector<std::string> statusLog;

    Shell() {
        services.mainDispatcher = &dispatcher;
        services.saveDialog = &saveDialog;
        services.alerts = &alerts;
        auto vp = std::make_unique<rivet::ui::PdfViewport>();
        viewport = vp.get();
        context = std::make_unique<ShellContext>(ShellContext{workspace, services, *viewport, {}, {}, {}});
        root->addChild(std::move(vp));
        files = std::make_unique<FileController>(engine, *context, scheduler,
                                                 [this](std::string text) { statusLog.push_back(std::move(text)); },
                                                 [] {});
    }
    ~Shell() {
        files.reset();
        viewport->clearDocument();
    }

    DocumentTab* openMd(const std::string& name, const std::string& bytes) {
        writeBytes(dir(name), bytes);
        workspace.openDocument(dir(name));
        dispatcher.waitUntil([this] {
            DocumentTab* tab = workspace.activeTab();
            return tab != nullptr && tab->state() != DocumentTab::State::Loading;
        });
        return workspace.activeTab();
    }
    bool hasStatus(const std::string& prefix) const {
        return !statusLog.empty() && statusLog.back().rfind(prefix, 0) == 0;
    }
    static void type(DocumentTab& tab, std::size_t at, const std::string& text) {
        CHECK(tab.markdown()->execute(std::make_unique<TextEditCommand>(*tab.markdown(), at, 0, text)));
    }
};

} // namespace

// --- Kind detection and codec ---------------------------------------------------

RIVET_TEST(markdownKindIsChosenByExtensionOnly) {
    CHECK(documentKindForPath("a.md") == DocumentKind::Markdown);
    CHECK(documentKindForPath("/x/B.MARKDOWN") == DocumentKind::Markdown);
    CHECK(documentKindForPath("c.Mdown") == DocumentKind::Markdown);
    CHECK(documentKindForPath("a.pdf") == DocumentKind::Pdf);
    CHECK(documentKindForPath("notes") == DocumentKind::Pdf);
    CHECK(documentKindForPath("md") == DocumentKind::Pdf);
    CHECK(documentKindForPath("a.md.pdf") == DocumentKind::Pdf);
    CHECK(hasMarkdownExtension("x.MD"));
    CHECK(!hasMarkdownExtension("x.txt"));
}

RIVET_TEST(markdownCodecDetectsBomLineEndingsAndRejectsInvalidUtf8) {
    auto plain = decodeMarkdownBytes("a\nb\n");
    CHECK(plain.has_value() && plain->lineEnding == LineEnding::LF && !plain->hadBom);

    auto crlf = decodeMarkdownBytes("a\r\nb\r\n");
    CHECK(crlf.has_value());
    CHECK(crlf->lineEnding == LineEnding::CRLF);
    CHECK(crlf->text == "a\nb\n");
    CHECK_EQ(encodeMarkdownBytes(crlf->text, crlf->lineEnding, crlf->hadBom), std::string("a\r\nb\r\n"));

    auto bom = decodeMarkdownBytes("\xEF\xBB\xBF" "# T\n");
    CHECK(bom.has_value() && bom->hadBom && bom->text == "# T\n");
    CHECK_EQ(encodeMarkdownBytes(bom->text, bom->lineEnding, true), std::string("\xEF\xBB\xBF" "# T\n"));

    CHECK(decodeMarkdownBytes("\xFF\xFEh\0i\0").has_value() == false); // UTF-16 BOM
    CHECK(!decodeMarkdownBytes("ok \xC3\x28").has_value());            // bad continuation
    CHECK(!decodeMarkdownBytes("\xC0\xAF").has_value());               // overlong
    CHECK(!decodeMarkdownBytes("\xED\xA0\x80").has_value());           // surrogate
    auto bad = decodeMarkdownBytes("\x80");
    CHECK(!bad.has_value() && bad.error().code == ErrorCode::InvalidDocument);
    CHECK(decodeMarkdownBytes("h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x98\x80").has_value());
}

// --- Workspace open ---------------------------------------------------------------

RIVET_TEST(workspaceOpensMarkdownWithoutPdfBackend) {
    Shell shell;
    DocumentTab* tab = shell.openMd("note.md", "# Hi\r\nthere\r\n");
    CHECK(tab != nullptr);
    CHECK(tab->state() == DocumentTab::State::Ready);
    CHECK(tab->kind() == DocumentKind::Markdown);
    CHECK(tab->session() == nullptr);
    CHECK(tab->markdown() != nullptr);
    CHECK(tab->markdown()->source() == "# Hi\nthere\n");
    CHECK(tab->markdown()->lineEnding() == LineEnding::CRLF);
    CHECK(tab->markdown()->mode() == MarkdownDisplayMode::Rendered);
    CHECK(!tab->isDirty());
    CHECK_EQ(shell.engine.opens.load(), 0); // the PDF engine was never consulted
}

RIVET_TEST(workspaceOpenBomAndUtf8Preserved) {
    Shell shell;
    DocumentTab* tab = shell.openMd("bom.md", "\xEF\xBB\xBF" "\xC3\xA9\n");
    CHECK(tab != nullptr && tab->state() == DocumentTab::State::Ready);
    CHECK(tab->markdown()->hasBom());
    CHECK(tab->markdown()->source() == "\xC3\xA9\n");
}

RIVET_TEST(workspaceInvalidUtf8GivesControlledErrorTab) {
    Shell shell;
    DocumentTab* tab = shell.openMd("bad.md", "ok \xC3\x28 broken");
    CHECK(tab != nullptr);
    CHECK(tab->state() == DocumentTab::State::Error);
    CHECK(tab->markdown() == nullptr);
    CHECK(!tab->isDirty());
}

RIVET_TEST(workspaceOpensLargeMarkdown) {
    Shell shell;
    std::string big;
    for (int i = 0; i < 200000; ++i) big += "line " + std::to_string(i) + " of text\n";
    DocumentTab* tab = shell.openMd("big.md", big);
    CHECK(tab != nullptr && tab->state() == DocumentTab::State::Ready);
    CHECK_EQ(tab->markdown()->source().size(), big.size());
}

RIVET_TEST(markdownDirtyCheckpointFollowsUndoRedo) {
    Shell shell;
    DocumentTab* tab = shell.openMd("d.md", "abc\n");
    CHECK(tab->markdown() != nullptr);
    const auto rev0 = tab->markdown()->revision();
    Shell::type(*tab, 3, "d");
    CHECK(tab->isDirty() && tab->canUndo() && !tab->canRedo());
    CHECK(tab->markdown()->revision() > rev0);
    CHECK(tab->markdown()->source() == "abcd\n");
    tab->undo();
    CHECK(!tab->isDirty() && tab->canRedo());
    CHECK(tab->markdown()->source() == "abc\n");
    tab->redo();
    CHECK(tab->isDirty());
    // New edit after undo gets a fresh checkpoint: undoing it back is clean,
    // another fresh edit is not mistaken for the saved state.
    tab->undo();
    Shell::type(*tab, 0, "x");
    CHECK(tab->isDirty());
    tab->undo();
    CHECK(!tab->isDirty());
}

// --- FileController ----------------------------------------------------------------

RIVET_TEST(markdownSavePreservesCrlfAndBomAndMarksClean) {
    Shell shell;
    DocumentTab* tab = shell.openMd("s.md", "\xEF\xBB\xBF" "a\r\nb\r\n");
    Shell::type(*tab, 1, "!");
    CHECK(tab->isDirty());
    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!tab->isDirty());
    CHECK(!tab->markdown()->isEditingLocked());
    CHECK(readBytes(shell.dir("s.md")) == "\xEF\xBB\xBF" "a!\r\nb\r\n");
    // Undo after a save is dirty again (the saved state is a checkpoint).
    tab->undo();
    CHECK(tab->isDirty());
}

RIVET_TEST(markdownFailedSaveKeepsDirtyAndFileUntouched) {
    Shell shell;
    DocumentTab* tab = shell.openMd("f.md", "orig\n");
    Shell::type(*tab, 0, "new ");
    shell.files->setMarkdownFaultInjector([](rivet::core::io::AtomicWriteFault fault) {
        return fault == rivet::core::io::AtomicWriteFault::Rename ? ENOSPC : 0;
    });
    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Save failed"); }));
    CHECK(tab->isDirty());
    CHECK(!tab->markdown()->isEditingLocked());
    CHECK(readBytes(shell.dir("f.md")) == "orig\n");
    // Repair and retry.
    shell.files->setMarkdownFaultInjector({});
    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!tab->isDirty());
    CHECK(readBytes(shell.dir("f.md")) == "new orig\n");
}

RIVET_TEST(markdownSaveAsKeepsMarkdownExtensionAndRetitles) {
    Shell shell;
    DocumentTab* tab = shell.openMd("a.md", "text\n");
    Shell::type(*tab, 0, "z");
    shell.saveDialog.next_ = shell.dir("renamed"); // no extension: .md is appended
    shell.files->saveAs(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!shell.saveDialog.lastOptions.allowedExtensions.empty());
    CHECK(tab->path() == shell.dir("renamed.md"));
    CHECK(tab->title() == "renamed.md");
    CHECK(tab->markdown()->path() == shell.dir("renamed.md"));
    CHECK(readBytes(shell.dir("renamed.md")) == "ztext\n");
    CHECK(readBytes(shell.dir("a.md")) == "text\n"); // the source is untouched
    CHECK(!tab->isDirty());
}

RIVET_TEST(markdownPdfOnlyCommandsAreDisabled) {
    Shell shell;
    DocumentTab* tab = shell.openMd("p.md", "x\n");
    CHECK(tab != nullptr);
    CHECK(shell.files->canPerform(FileCommand::Save));
    CHECK(shell.files->canPerform(FileCommand::SaveAs));
    CHECK(!shell.files->canPerform(FileCommand::Extract));
    CHECK(!shell.files->canPerform(FileCommand::Merge));
    CHECK(!shell.files->canPerform(FileCommand::Split));
    CHECK(!shell.files->canPerform(FileCommand::ImportBefore));
}

RIVET_TEST(markdownCloseConfirmationSaveDontSaveCancel) {
    Shell shell;
    DocumentTab* tab = shell.openMd("c.md", "c\n");
    CHECK(shell.files->confirmCloseTab(*tab)); // clean: closes at once

    Shell::type(*tab, 0, "1");
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Cancel);
    CHECK(!shell.files->confirmCloseTab(*tab));
    CHECK(tab->isDirty());

    // Don't Save discards the edits and closes the tab; the file is untouched.
    const TabId first = tab->id();
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::DontSave);
    CHECK(shell.files->confirmCloseTab(*tab));
    CHECK(shell.workspace.indexOfTab(first) == DocumentWorkspace::kNoTab);
    CHECK(readBytes(shell.dir("c.md")) == "c\n");

    // Save writes, then closes the tab when the save completes.
    tab = shell.openMd("c.md", "c\n");
    CHECK(tab != nullptr);
    Shell::type(*tab, 0, "1");
    const TabId id = tab->id();
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    (void)shell.files->confirmCloseTab(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.workspace.indexOfTab(id) == DocumentWorkspace::kNoTab; }));
    CHECK(readBytes(shell.dir("c.md")) == "1c\n");
}

RIVET_TEST(markdownQuitSavesDirtyTabsBeforeReplying) {
    Shell shell;
    DocumentTab* tab = shell.openMd("q.md", "q\n");
    Shell::type(*tab, 0, "!");
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Save);
    bool replied = false;
    bool proceed = false;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(shell.dispatcher.waitUntil([&] { return replied; }));
    CHECK(proceed);
    CHECK(readBytes(shell.dir("q.md")) == "!q\n");

    // Cancel keeps running.
    Shell::type(*tab, 0, "?");
    shell.alerts.saveAnswers.push_back(rivet::platform::SaveChangesChoice::Cancel);
    replied = false;
    proceed = true;
    shell.files->handleQuitRequest([&](bool decision) {
        replied = true;
        proceed = decision;
    });
    CHECK(replied && !proceed);
}

// --- ShellController: binding when kinds alternate ------------------------------------

RIVET_TEST(shellSwitchingKindsLeavesNoStaleBindings) {
    FakeWritableEngine* raw = nullptr;
    auto engine = std::make_unique<FakeWritableEngine>();
    raw = engine.get();
    (void)raw;
    WaitDispatcher dispatcher;
    FakeAlerts alerts;
    rivet::platform::ShellServices services;
    services.mainDispatcher = &dispatcher;
    services.alerts = &alerts;
    TempDir dir;
    writeBytes(dir("doc.pdf"), "placeholder\n");
    writeBytes(dir("note.md"), "# n\n");

    auto shell = ShellController::createWithEngine(services, std::move(engine));
    shell->rootWidget().setFrame(rivet::core::Rect{0, 0, 1000, 700});
    auto ready = [&](std::size_t count) {
        return dispatcher.waitUntil([&] {
            DocumentTab* tab = shell->workspace().activeTab();
            return shell->workspace().tabCount() == count && tab != nullptr &&
                   tab->state() == DocumentTab::State::Ready;
        });
    };
    shell->openDocument(dir("doc.pdf"));
    CHECK(ready(1));
    CHECK(!shell->canPerformMarkdownMode());
    CHECK(!shell->pdfViewport().frame().isEmpty());
    CHECK(shell->markdownHost().frame().isEmpty());

    shell->openDocument(dir("note.md"));
    CHECK(ready(2));
    CHECK(shell->canPerformMarkdownMode());
    CHECK(shell->activeMarkdownMode() == MarkdownDisplayMode::Rendered);
    CHECK(shell->pdfViewport().frame().isEmpty());
    CHECK(!shell->markdownHost().frame().isEmpty());

    shell->performMarkdownMode(MarkdownDisplayMode::Split);
    CHECK(shell->activeMarkdownMode() == MarkdownDisplayMode::Split);

    for (int round = 0; round < 4; ++round) {
        shell->workspace().activateTab(0);
        dispatcher.pump();
        CHECK(!shell->canPerformMarkdownMode());
        CHECK(!shell->activeMarkdownMode().has_value());
        CHECK(!shell->pdfViewport().frame().isEmpty());
        CHECK(shell->markdownHost().frame().isEmpty());
        // Mode changes must not reach a PDF tab.
        shell->performMarkdownMode(MarkdownDisplayMode::Source);
        CHECK(shell->workspace().tab(1)->markdown()->mode() == MarkdownDisplayMode::Split);

        shell->workspace().activateTab(1);
        dispatcher.pump();
        CHECK(shell->canPerformMarkdownMode());
        CHECK(shell->activeMarkdownMode() == MarkdownDisplayMode::Split);
        CHECK(shell->pdfViewport().frame().isEmpty());
        CHECK(!shell->markdownHost().frame().isEmpty());
    }

    // Markdown edits flow to Undo/Redo and dirty state of the active tab.
    DocumentTab* md = shell->workspace().activeTab();
    CHECK(md->markdown()->execute(std::make_unique<TextEditCommand>(*md->markdown(), 0, 0, "x")));
    CHECK(md->isDirty());
    CHECK(shell->workspace().tab(0)->isDirty() == false);
    md->undo();
    CHECK(!md->isDirty());
    // Close the Markdown tab: the PDF tab is bound again.
    shell->workspace().closeTab(1);
    dispatcher.pump();
    CHECK(!shell->pdfViewport().frame().isEmpty());
    CHECK(shell->markdownHost().frame().isEmpty());
}
