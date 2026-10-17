// SPDX-License-Identifier: MPL-2.0
// Recognize Text in the app layer (ADR-0027): the OcrController over a real
// DocumentSession (fake page backend) with a fake ITextRecognizer: progress
// and the outcome in the status bar, the pages-without-text mode, one
// undoable edit per page, cancellation, the menu entries and the
// unavailable reasons. The test thread is the main thread and only blocks on
// the dispatcher queue.
#include "RivetTest.h"

#include "fakes/FakePageDocument.hpp"

#include "app/ContentTool.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/MenuModel.hpp"
#include "app/OcrController.hpp"
#include "app/ShellContext.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "platform/PlatformKit.hpp"
#include "platform/TextRecognizer.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace rivet;
using app::ContentCommand;

namespace {

constexpr const char* kSecretWord = "Zebrafish";

class WaitDispatcher final : public core::IMainThreadDispatcher {
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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        for (;;) {
            pump();
            if (predicate()) return true;
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(30)),
                                [this] { return !queue_.empty(); })) {
                if (std::chrono::steady_clock::now() >= deadline) return predicate();
            }
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

// One word per page; optionally blocks until cancelled.
class FakeRecognizer final : public platform::ITextRecognizer {
public:
    mutable std::atomic<int> calls{0};
    bool blockUntilStopped = false;
    mutable std::mutex mutex;
    mutable std::vector<std::string> languages;

    core::Result<platform::RecognizedPage> recognize(const core::Bitmap& bitmap, std::span<const std::string> langs,
                                                     std::stop_token stop) const override {
        ++calls;
        {
            std::lock_guard<std::mutex> lock(mutex);
            languages.assign(langs.begin(), langs.end());
        }
        if (blockUntilStopped) {
            while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancelled", "test"));
        }
        const double w = bitmap.width();
        const double h = bitmap.height();
        platform::RecognizedWord word;
        word.text = kSecretWord;
        word.quad = {core::Point{w * 0.1, h * 0.1}, core::Point{w * 0.5, h * 0.1}, core::Point{w * 0.5, h * 0.2},
                     core::Point{w * 0.1, h * 0.2}};
        word.confidence = 0.9F;
        platform::RecognizedLine line;
        line.words.push_back(word);
        platform::RecognizedPage page;
        page.lines.push_back(line);
        return page;
    }
};

std::filesystem::path tempPdf(const char* name) {
    auto path = std::filesystem::temp_directory_path() / std::filesystem::path{std::string{"rivet-ocrctl-"} + name + ".pdf"};
    if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
        std::fputs("%PDF-1.4\n", file);
        std::fclose(file);
    }
    return path;
}

struct Shell {
    test::FakePageEngine engine{3};
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    app::DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    FakeRecognizer recognizer;
    platform::ShellServices services;
    std::unique_ptr<ui::PdfViewport> viewport = std::make_unique<ui::PdfViewport>();
    std::vector<std::string> statusLog;
    std::unique_ptr<app::ShellContext> context;
    std::unique_ptr<app::OcrController> ocr;

    explicit Shell(bool withRecognizer = true) {
        services.mainDispatcher = &dispatcher;
        if (withRecognizer) services.textRecognizer = &recognizer;
        context = std::make_unique<app::ShellContext>(app::ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) { statusLog.push_back(std::move(message)); },
            [](ui::Widget*) {},
            [] {},
        });
        ocr = std::make_unique<app::OcrController>(*context, scheduler);
    }
    ~Shell() { ocr.reset(); }

    bool open(const char* name) {
        workspace.openDocument(tempPdf(name));
        return dispatcher.waitUntil([this] {
            app::DocumentTab* tab = workspace.activeTab();
            return tab != nullptr && tab->state() == app::DocumentTab::State::Ready;
        });
    }
    editor::DocumentSession& session() { return *workspace.activeTab()->session(); }
    bool waitIdle() {
        return dispatcher.waitUntil([this] { return !ocr->running(); });
    }
    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }
    std::size_t pagesWithOcr() {
        std::size_t n = 0;
        for (const editor::PageEntry& entry : session().pageSnapshot()->entries()) {
            if (entry.contentEdits != nullptr && !entry.contentEdits->invisibleRuns.empty()) ++n;
        }
        return n;
    }
    bool anyStatusContains(const std::string& needle) const {
        return std::any_of(statusLog.begin(), statusLog.end(),
                           [&](const std::string& s) { return s.find(needle) != std::string::npos; });
    }
};

} // namespace

RIVET_TEST(ocrControllerIsUnavailableWithoutARecognizerOrADocument) {
    Shell none(false);
    CHECK(none.open("none"));
    CHECK(!none.ocr->canPerform(ContentCommand::RecognizeTextAll));
    none.ocr->perform(ContentCommand::RecognizeTextAll);
    CHECK(none.lastStatus().find("not available") != std::string::npos);
    CHECK(!none.ocr->running());

    Shell noDocument;
    CHECK(!noDocument.ocr->canPerform(ContentCommand::RecognizeTextMissing));
    CHECK(noDocument.ocr->unavailableReason().find("open a PDF") != std::string::npos);
    CHECK(!noDocument.ocr->canPerform(ContentCommand::ToolEdit)); // not an OCR command
}

RIVET_TEST(ocrControllerMenuListsBothCommands) {
    int found = 0;
    for (const app::Menu& menu : app::buildMenuBar()) {
        for (const app::MenuItem& item : menu.items) {
            if (item.command.kind != app::AppCommand::Kind::Content) continue;
            if (item.command.content() == ContentCommand::RecognizeTextMissing &&
                item.title == "Recognize Text (Pages Without Text)") {
                ++found;
            }
            if (item.command.content() == ContentCommand::RecognizeTextAll &&
                item.title == "Recognize Text (All Pages)") {
                ++found;
            }
        }
    }
    CHECK_EQ(found, 2);
}

RIVET_TEST(ocrControllerAllPagesAddsOneUndoableEditPerPage) {
    Shell shell;
    CHECK(shell.open("all"));
    CHECK(shell.ocr->canPerform(ContentCommand::RecognizeTextAll));
    shell.ocr->perform(ContentCommand::RecognizeTextAll);
    CHECK(shell.ocr->running());
    CHECK(!shell.ocr->canPerform(ContentCommand::RecognizeTextAll)); // one job at a time
    CHECK(shell.waitIdle());

    CHECK_EQ(shell.recognizer.calls.load(), 3);
    CHECK_EQ(shell.pagesWithOcr(), std::size_t{3});
    CHECK(shell.lastStatus().find("Recognized text on 3 of 3 pages") != std::string::npos);
    CHECK(shell.anyStatusContains("of 3 (Esc to cancel)")); // progress was shown
    {
        std::lock_guard<std::mutex> lock(shell.recognizer.mutex);
        CHECK_EQ(shell.recognizer.languages.size(), std::size_t{2});
        if (shell.recognizer.languages.size() == 2) {
            CHECK_EQ(shell.recognizer.languages[0], std::string("en-US"));
            CHECK_EQ(shell.recognizer.languages[1], std::string("ru-RU"));
        }
    }
    CHECK(shell.session().isDirty());
    // Recognized text is never put in the status bar (or any log line).
    CHECK(!shell.anyStatusContains(kSecretWord));

    // One undo step per page.
    CHECK(shell.session().undo());
    CHECK_EQ(shell.pagesWithOcr(), std::size_t{2});
    CHECK(shell.session().undo());
    CHECK(shell.session().undo());
    CHECK_EQ(shell.pagesWithOcr(), std::size_t{0});
    CHECK(!shell.session().isDirty());
    CHECK(shell.session().redo());
    CHECK_EQ(shell.pagesWithOcr(), std::size_t{1});
}

RIVET_TEST(ocrControllerPagesWithTextAreSkippedInMissingMode) {
    Shell shell;
    CHECK(shell.open("missing"));
    // Every fake page has extractable text.
    shell.ocr->perform(ContentCommand::RecognizeTextMissing);
    CHECK(shell.waitIdle());
    CHECK_EQ(shell.recognizer.calls.load(), 0);
    CHECK_EQ(shell.pagesWithOcr(), std::size_t{0});
    CHECK(shell.lastStatus().find("3 already had text") != std::string::npos);
    CHECK(!shell.session().isDirty());
}

RIVET_TEST(ocrControllerEscapeCancelsAndLeavesTheDocumentUntouched) {
    Shell shell;
    shell.recognizer.blockUntilStopped = true;
    CHECK(shell.open("cancel"));
    shell.ocr->perform(ContentCommand::RecognizeTextAll);
    CHECK(shell.ocr->running());
    CHECK(shell.dispatcher.waitUntil([&] { return shell.recognizer.calls.load() >= 1; }));
    CHECK(shell.ocr->requestCancel());
    CHECK(!shell.ocr->requestCancel()); // already cancelling
    CHECK(shell.waitIdle());
    CHECK(shell.lastStatus().find("cancelled") != std::string::npos);
    CHECK_EQ(shell.pagesWithOcr(), std::size_t{0});
    CHECK_EQ(shell.recognizer.calls.load(), 1); // later pages were never started
    CHECK(!shell.session().isDirty());
    CHECK(!shell.ocr->requestCancel()); // nothing to cancel any more
}

RIVET_TEST(ocrControllerClosingTheTabCancelsTheJob) {
    Shell shell;
    shell.recognizer.blockUntilStopped = true;
    CHECK(shell.open("close"));
    const app::TabId tab = shell.workspace.activeTab()->id();
    shell.ocr->perform(ContentCommand::RecognizeTextAll);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.recognizer.calls.load() >= 1; }));
    shell.ocr->cancelForTab(tab);
    CHECK(shell.waitIdle());
    CHECK(shell.lastStatus().find("cancelled") != std::string::npos);
}
