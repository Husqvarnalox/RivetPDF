// SPDX-License-Identifier: MPL-2.0
// Redaction in the app layer (ADR-0026): the pure RedactionInteraction
// machine, the menu entries, and the RedactionController over a real
// DocumentSession (fake page backend): the Redact tool in the shared tool
// state, marks from drags, selection and Delete, Esc, and Apply Redactions
// with its confirmation and metadata prompts. Deterministic: the test thread
// is the main thread and only blocks on the dispatcher queue.
#include "RivetTest.h"

#include "fakes/FakePageDocument.hpp"

#include "app/AnnotationController.hpp"
#include "app/ContentBackend.hpp"
#include "app/ContentController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/MenuModel.hpp"
#include "app/RedactionController.hpp"
#include "app/RedactionInteraction.hpp"
#include "app/ShellContext.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/ContentService.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/FormService.hpp"
#include "platform/Alerts.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace rivet;
using app::AppCommand;
using app::ContentTool;
using app::RedactCommand;
using app::RedactionInteraction;
using Kind = app::RedactionInteraction::Intent::Kind;

// --- Interaction machine --------------------------------------------------------------

RIVET_TEST(redactionInteractionDragAddsAClampedMark) {
    RedactionInteraction machine;
    CHECK(machine.press(2, core::Point{10, 20}, core::Size{100, 200}).consumed);
    CHECK(machine.dragging());
    machine.drag(core::Point{150, 60}); // clamped to the page width
    const auto preview = machine.preview();
    CHECK(preview.has_value());
    if (preview) {
        CHECK_EQ(preview->first, std::size_t{2});
        CHECK_NEAR(preview->second.size.width, 90.0, 1e-9);
    }
    const RedactionInteraction::Intent intent = machine.release(core::Point{150, 60});
    CHECK(intent.kind == Kind::AddMark);
    CHECK_EQ(intent.page, std::size_t{2});
    CHECK_NEAR(intent.rect.origin.x, 10.0, 1e-9);
    CHECK_NEAR(intent.rect.origin.y, 20.0, 1e-9);
    CHECK_NEAR(intent.rect.size.width, 90.0, 1e-9);
    CHECK_NEAR(intent.rect.size.height, 40.0, 1e-9);
    CHECK(!machine.dragging());
}

RIVET_TEST(redactionInteractionClickSelectsAndDeleteRemoves) {
    RedactionInteraction machine;
    machine.setMarkHitTest([](std::size_t page, core::Point p) -> std::optional<std::size_t> {
        if (page == 0 && p.x < 50.0) return std::size_t{3};
        return std::nullopt;
    });
    machine.press(0, core::Point{10, 10}, core::Size{100, 100});
    RedactionInteraction::Intent intent = machine.release(core::Point{11, 10}); // a click
    CHECK(intent.kind == Kind::SelectMark);
    CHECK_EQ(intent.index, std::size_t{3});
    CHECK(machine.selection().has_value());

    intent = machine.key(ui::Key::Delete);
    CHECK(intent.kind == Kind::RemoveMark);
    CHECK_EQ(intent.index, std::size_t{3});
    CHECK(!machine.selection().has_value());
    CHECK(!machine.key(ui::Key::Delete).consumed);

    // A click elsewhere clears a selection; Escape too.
    machine.setSelection(RedactionInteraction::MarkRef{0, 1});
    machine.press(0, core::Point{80, 80}, core::Size{100, 100});
    CHECK(machine.release(core::Point{80, 80}).kind == Kind::ClearSelection);
    machine.setSelection(RedactionInteraction::MarkRef{0, 1});
    CHECK(machine.key(ui::Key::Escape).kind == Kind::ClearSelection);
    CHECK(!machine.key(ui::Key::Escape).consumed);

    // Escape during a drag cancels it without a mark.
    machine.press(0, core::Point{10, 10}, core::Size{100, 100});
    machine.drag(core::Point{60, 60});
    CHECK(machine.key(ui::Key::Escape).consumed);
    CHECK(!machine.dragging());
    CHECK(!machine.release(core::Point{60, 60}).consumed);
}

RIVET_TEST(menuModelHasTheRedactionEntries) {
    const auto bar = app::buildMenuBar();
    int found = 0;
    for (const app::Menu& menu : bar) {
        for (const app::MenuItem& item : menu.items) {
            if (item.command.kind != AppCommand::Kind::Redact) continue;
            CHECK_EQ(menu.title, std::string("Content"));
            ++found;
            if (item.command == AppCommand::of(RedactCommand::ToolRedact)) CHECK(item.checkable);
        }
    }
    CHECK_EQ(found, 4);
    CHECK(AppCommand::of(RedactCommand::ApplyRedactions).redact() == RedactCommand::ApplyRedactions);
}

// --- Controller over a session ----------------------------------------------------------

namespace {

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
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        for (;;) {
            pump();
            if (predicate()) return true;
            std::unique_lock<std::mutex> lock(mutex_);
            if (!cv_.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(5)),
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

class ScriptedAlerts final : public platform::IAlertService {
public:
    std::deque<bool> confirmAnswers;
    std::vector<std::string> confirmTitles;
    std::vector<std::string> errorTitles;

    platform::SaveChangesChoice askSaveChanges(std::string_view) override { return platform::SaveChangesChoice::Cancel; }
    platform::ReviewChangesChoice askReviewUnsavedChanges(std::size_t) override {
        return platform::ReviewChangesChoice::Cancel;
    }
    void showError(std::string_view title, std::string_view) override { errorTitles.emplace_back(title); }
    bool confirm(std::string_view title, std::string_view, std::string_view) override {
        confirmTitles.emplace_back(title);
        if (confirmAnswers.empty()) return false;
        const bool answer = confirmAnswers.front();
        confirmAnswers.pop_front();
        return answer;
    }
};

std::filesystem::path tempPdf(const char* name) {
    auto path = std::filesystem::temp_directory_path() /
                std::filesystem::path{std::string{"rivet-redactctl-"} + name + ".pdf"};
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
    ScriptedAlerts alerts;
    platform::ShellServices services;

    std::unique_ptr<ui::Container> root = std::make_unique<ui::Container>();
    ui::PdfViewport* viewport = nullptr;
    std::vector<std::string> statusLog;
    std::unique_ptr<app::ShellContext> context;
    std::unique_ptr<app::AnnotationController> annotations;
    std::unique_ptr<app::ContentController> content;
    std::unique_ptr<app::RedactionController> redaction;

    Shell() {
        services.mainDispatcher = &dispatcher;
        services.alerts = &alerts;
        auto vp = std::make_unique<ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(core::Rect{180.0, 68.0, 800.0, 600.0});
        context = std::make_unique<app::ShellContext>(app::ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) { statusLog.push_back(std::move(message)); },
            [](ui::Widget*) {},
            [] {},
        });
        root->addChild(std::move(vp));
        annotations = std::make_unique<app::AnnotationController>(*context, *root);
        content = std::make_unique<app::ContentController>(*context, *root, *annotations, scheduler,
                                                           app::createEditorContentBackend());
        redaction = std::make_unique<app::RedactionController>(*context, *content);
        workspace.setOnActiveTabChanged([this] { bind(); });
    }

    ~Shell() {
        redaction.reset();
        content.reset();
        annotations.reset();
        viewport->clearDocument();
        workspace.setOnActiveTabChanged({});
    }

    void bind() {
        app::DocumentTab* tab = workspace.activeTab();
        if (tab == nullptr || tab->state() != app::DocumentTab::State::Ready) {
            viewport->clearDocument();
            annotations->bindTab(nullptr);
            content->bindTab(nullptr);
            redaction->bindTab(nullptr);
            return;
        }
        editor::DocumentSession* session = tab->session();
        viewport->setDocument(session->id(), &session->layout(), &session->renderSource(),
                              [session] { return session->revision(); }, &tab->viewState());
        annotations->bindTab(tab);
        content->bindTab(tab);
        redaction->bindTab(tab);
    }

    bool open(const char* name) {
        workspace.openDocument(tempPdf(name));
        return dispatcher.waitUntil([this] {
            app::DocumentTab* tab = workspace.activeTab();
            return tab != nullptr && tab->state() == app::DocumentTab::State::Ready;
        });
    }

    editor::DocumentSession& session() { return *workspace.activeTab()->session(); }

    // Everything Apply Redactions needs for page 0.
    bool loadPage0() {
        editor::DocumentSession& s = session();
        const core::PageId page = s.pageId(0);
        s.forms().ensureLoaded();
        return dispatcher.waitUntil([&] {
            return s.contentService().content(page)->loaded && s.annotations().originals(page) != nullptr &&
                   s.forms().state() == editor::FormService::State::Ready;
        });
    }

    void addMark(core::Rect rect) {
        RedactionInteraction::Intent intent;
        intent.kind = Kind::AddMark;
        intent.page = 0;
        intent.rect = rect;
        redaction->applyIntent(intent);
    }

    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }
};

} // namespace

RIVET_TEST(redactionControllerToolSharesTheToolStateAndMarks) {
    Shell shell;
    CHECK(!shell.redaction->canPerform(RedactCommand::ToolRedact));
    CHECK(shell.open("tool"));
    CHECK(shell.redaction->canPerform(RedactCommand::ToolRedact));
    CHECK(!shell.redaction->canPerform(RedactCommand::ApplyRedactions));
    CHECK(!shell.redaction->canPerform(RedactCommand::MarkSelection));

    shell.redaction->perform(RedactCommand::ToolRedact);
    CHECK(shell.content->tool() == ContentTool::Redact);
    CHECK(shell.redaction->isChecked(RedactCommand::ToolRedact).value_or(false));
    CHECK(shell.redaction->active());
    CHECK(!shell.content->active()); // the object tools ignore input
    CHECK(shell.annotations->suspended());

    shell.addMark(core::Rect{10, 20, 100, 30});
    CHECK_EQ(shell.redaction->marks(0).size(), std::size_t{1});
    if (!shell.redaction->marks(0).empty()) {
        const core::Rect mark = shell.redaction->marks(0).front();
        CHECK_NEAR(mark.origin.x, 10.0, 1e-6);
        CHECK_NEAR(mark.origin.y, 20.0, 1e-6);
        CHECK_NEAR(mark.size.width, 100.0, 1e-6);
    }
    CHECK(shell.redaction->canPerform(RedactCommand::ApplyRedactions));
    CHECK(shell.redaction->canPerform(RedactCommand::ClearMarks));

    // Select + Delete through the machine; Esc clears a selection.
    shell.redaction->interaction().setSelection(RedactionInteraction::MarkRef{0, 0});
    CHECK(shell.redaction->handleEscape());
    CHECK(!shell.redaction->interaction().selection().has_value());
    shell.redaction->interaction().setSelection(RedactionInteraction::MarkRef{0, 0});
    shell.redaction->applyIntent(shell.redaction->interaction().key(ui::Key::Delete));
    CHECK(shell.redaction->marks(0).empty());
    CHECK(shell.session().undo());
    CHECK_EQ(shell.redaction->marks(0).size(), std::size_t{1});

    shell.redaction->perform(RedactCommand::ClearMarks);
    CHECK(shell.redaction->marks(0).empty());

    // Choosing the tool again ends it; marks stay painted whatever the tool.
    shell.redaction->perform(RedactCommand::ToolRedact);
    CHECK(shell.content->tool() == ContentTool::None);
    CHECK(!shell.annotations->suspended());
    CHECK(shell.session().undo());
    CHECK_EQ(shell.redaction->marks(0).size(), std::size_t{1});
}

RIVET_TEST(redactionControllerApplyAsksAndRunsOneUndoStep) {
    Shell shell;
    CHECK(shell.open("apply"));
    CHECK(shell.loadPage0());
    shell.addMark(core::Rect{10, 20, 100, 30});

    // Declined: nothing happens.
    shell.alerts.confirmAnswers = {false};
    shell.redaction->perform(RedactCommand::ApplyRedactions);
    CHECK_EQ(shell.alerts.confirmTitles.size(), std::size_t{1});
    CHECK(shell.session().pageSnapshot()->at(0).contentEdits == nullptr);
    CHECK(shell.session().pageSnapshot()->at(0).redactionMarks != nullptr);

    // Confirmed, metadata kept.
    shell.alerts.confirmAnswers = {true, false};
    shell.redaction->perform(RedactCommand::ApplyRedactions);
    CHECK_EQ(shell.alerts.confirmTitles.size(), std::size_t{3});
    const editor::PageEntry& entry = shell.session().pageSnapshot()->at(0);
    CHECK(entry.redactionMarks == nullptr);
    CHECK(entry.contentEdits != nullptr && entry.contentEdits->redactions.size() == 1);
    CHECK(!shell.session().pageSnapshot()->removeMetadata());
    CHECK(shell.session().isDirty());

    CHECK(shell.session().undo());
    CHECK(shell.session().pageSnapshot()->at(0).contentEdits == nullptr);
    CHECK(shell.session().pageSnapshot()->at(0).redactionMarks != nullptr);

    // Confirmed with metadata removal.
    shell.alerts.confirmAnswers = {true, true};
    shell.redaction->perform(RedactCommand::ApplyRedactions);
    CHECK(shell.session().pageSnapshot()->removeMetadata());
}

RIVET_TEST(redactionControllerApplyReportsWhyItCannotRun) {
    Shell shell;
    CHECK(shell.open("refuse"));
    pdf::PdfPageContent unsafe;
    unsafe.regenerationSafe = false;
    unsafe.regenerationIssue = "page contains shading objects";
    shell.engine.lastDocument->pageContents[0] = unsafe;
    CHECK(shell.loadPage0());
    shell.addMark(core::Rect{10, 20, 100, 30});
    shell.alerts.confirmAnswers = {true, true};
    shell.redaction->perform(RedactCommand::ApplyRedactions);
    // Refused before anything is asked, with the probe's reason.
    CHECK(shell.alerts.confirmTitles.empty());
    CHECK_EQ(shell.alerts.errorTitles.size(), std::size_t{1});
    CHECK(shell.lastStatus().find("shading") != std::string::npos);
    CHECK(shell.session().pageSnapshot()->at(0).contentEdits == nullptr);
}
