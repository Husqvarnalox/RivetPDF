// SPDX-License-Identifier: MPL-2.0
// AnnotationController over a real DocumentSession (fake page backend):
// intents -> editor commands, undo/redo, selection pruning, keys, restyle vs
// defaults, markup from the text selection, the note editor and the editing
// lock. Deterministic: the test thread is the main thread and only blocks on
// the dispatcher queue.
#include "RivetTest.h"

#include "fakes/FakePageDocument.hpp"

#include "app/AnnotationBarController.hpp"
#include "app/AnnotationController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/ShellContext.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextArea.hpp"
#include "ui/UiTypes.hpp"

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
using app::AnnotationCommand;
using app::AnnotationController;
using app::AnnotationInteraction;
using app::AnnotationTool;
using app::DocumentTab;
using app::DocumentWorkspace;
using app::ShellContext;
using Intent = AnnotationInteraction::Intent;
using IntentKind = AnnotationInteraction::Intent::Kind;

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

std::filesystem::path tempPdf(const char* name) {
    auto path = std::filesystem::temp_directory_path() /
                std::filesystem::path{std::string{"rivet-annotctl-"} + name + ".pdf"};
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
    DocumentWorkspace workspace{engine, scheduler, &dispatcher};
    platform::ShellServices services;

    std::unique_ptr<ui::Container> root = std::make_unique<ui::Container>();
    ui::PdfViewport* viewport = nullptr;
    ui::Widget* focused = nullptr;
    std::vector<std::string> statusLog;
    std::unique_ptr<ShellContext> context;
    std::unique_ptr<AnnotationController> annotations;
    int stateChanges = 0;

    Shell() {
        services.mainDispatcher = &dispatcher;
        auto vp = std::make_unique<ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(core::Rect{180.0, 68.0, 800.0, 600.0});
        context = std::make_unique<ShellContext>(ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) { statusLog.push_back(std::move(message)); },
            [this](ui::Widget* widget) {
                if (focused == widget) return;
                if (focused != nullptr) focused->setFocused(false);
                focused = widget;
                if (focused != nullptr) focused->setFocused(true);
            },
            [] {},
        });
        root->addChild(std::move(vp));
        annotations = std::make_unique<AnnotationController>(*context, *root);
        annotations->layout(viewport->frame());
        annotations->setOnStateChanged([this] { ++stateChanges; });
        workspace.setOnActiveTabChanged([this] { bind(); });
    }

    ~Shell() {
        annotations.reset();
        viewport->clearDocument();
        workspace.setOnActiveTabChanged({});
    }

    void bind() {
        DocumentTab* tab = workspace.activeTab();
        if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
            viewport->clearDocument();
            annotations->bindTab(nullptr);
            return;
        }
        editor::DocumentSession* session = tab->session();
        viewport->setDocument(session->id(), &session->layout(), &session->renderSource(),
                              [session] { return session->revision(); }, &tab->viewState());
        annotations->bindTab(tab);
    }

    DocumentTab* open(const char* name) {
        const std::size_t before = workspace.tabCount();
        workspace.openDocument(tempPdf(name));
        if (!dispatcher.waitUntil([this, before] {
                DocumentTab* tab = workspace.activeTab();
                return workspace.tabCount() > before && tab != nullptr && tab->state() == DocumentTab::State::Ready;
            })) {
            return nullptr;
        }
        DocumentTab* tab = workspace.activeTab();
        // Originals of every page (so resolved lists are complete).
        for (std::size_t i = 0; i < tab->session()->pageCount(); ++i) loadPage(*tab->session(), i);
        return tab;
    }

    void loadPage(editor::DocumentSession& session, std::size_t index) {
        const std::size_t before = session.annotations().cachedOriginalPages();
        session.annotations().annotations(session.pageId(index));
        dispatcher.waitUntil([&] { return session.annotations().cachedOriginalPages() > before; });
    }

    DocumentTab* tab() { return workspace.activeTab(); }
    editor::DocumentSession& session() { return *tab()->session(); }
    std::size_t countOn(std::size_t page) {
        auto list = session().annotations().annotations(session().pageId(page));
        return list ? list->size() : 0;
    }
    std::optional<editor::AnnotationView> viewOf(core::AnnotationId id, std::size_t page) {
        return session().annotations().find(session().pageId(page), id);
    }
    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }
};

Intent intent(IntentKind kind, std::size_t page = 0) {
    Intent i;
    i.kind = kind;
    i.page = page;
    return i;
}

Intent shapeIntent(AnnotationTool tool, core::Rect rect) {
    Intent i = intent(IntentKind::CreateShape);
    i.tool = tool;
    i.rect = rect;
    return i;
}

Intent lineIntent(AnnotationTool tool) {
    Intent i = intent(IntentKind::CreateShape);
    i.tool = tool;
    i.a = core::Point{50.0, 50.0};
    i.b = core::Point{200.0, 120.0};
    return i;
}

core::AnnotationId createRect(Shell& shell, core::Rect rect = core::Rect{100.0, 100.0, 80.0, 40.0}) {
    shell.annotations->applyIntent(shapeIntent(AnnotationTool::Rectangle, rect));
    return shell.annotations->selectedId().value_or(core::AnnotationId{});
}

} // namespace

RIVET_TEST(controllerCreatesEveryKindThroughIntentsAndUndoRedoRestoresThem) {
    Shell shell;
    CHECK(shell.open("kinds") != nullptr);
    AnnotationController& c = *shell.annotations;

    struct Case {
        Intent intent;
        pdf::PdfAnnotationKind kind;
    };
    Intent note = intent(IntentKind::CreateNote);
    note.rect = core::Rect{300.0, 300.0, 20.0, 20.0};
    Intent ink = intent(IntentKind::CreateInk);
    ink.zoom = 1.0;
    ink.strokes = {{core::Point{10.0, 10.0}, core::Point{20.0, 10.0}, core::Point{30.0, 10.0},
                    core::Point{40.0, 30.0}, core::Point{60.0, 50.0}}};
    Intent stamp = intent(IntentKind::CreateStamp);
    stamp.rect = core::Rect{200.0, 400.0, 150.0, 50.0};
    const std::vector<Case> cases = {
        {note, pdf::PdfAnnotationKind::Note},
        {ink, pdf::PdfAnnotationKind::Ink},
        {shapeIntent(AnnotationTool::Rectangle, core::Rect{100.0, 100.0, 80.0, 40.0}), pdf::PdfAnnotationKind::Square},
        {shapeIntent(AnnotationTool::Ellipse, core::Rect{100.0, 200.0, 80.0, 40.0}), pdf::PdfAnnotationKind::Circle},
        {lineIntent(AnnotationTool::Line), pdf::PdfAnnotationKind::Line},
        {lineIntent(AnnotationTool::Arrow), pdf::PdfAnnotationKind::Arrow},
        {stamp, pdf::PdfAnnotationKind::Stamp},
    };
    std::size_t expected = 0;
    for (const Case& test : cases) {
        c.applyIntent(test.intent);
        ++expected;
        CHECK_EQ(shell.countOn(0), expected);
        const auto id = c.selectedId();
        CHECK(id.has_value());
        const auto view = shell.viewOf(*id, 0);
        CHECK(view.has_value());
        if (view.has_value()) CHECK(view->kind == test.kind);
        c.closeNoteEditor(); // the note's editor is open after a note creation
    }
    CHECK(shell.lastStatus().empty());
    // The ink stroke was reduced (collinear points dropped).
    const auto inkView = shell.session().annotations().annotations(shell.session().pageId(0));
    for (const auto& view : *inkView) {
        if (view.kind == pdf::PdfAnnotationKind::Ink) CHECK_LT(view.strokes[0].size(), 5u);
    }

    for (std::size_t i = 0; i < cases.size(); ++i) CHECK(shell.session().undo());
    CHECK_EQ(shell.countOn(0), 0u);
    CHECK(!c.selectedId().has_value()); // pruned: nothing resolves any more
    for (std::size_t i = 0; i < cases.size(); ++i) CHECK(shell.session().redo());
    CHECK_EQ(shell.countOn(0), cases.size());
}

RIVET_TEST(controllerSelectionIsPrunedLazilyAfterUndoOfItsCreation) {
    Shell shell;
    CHECK(shell.open("prune") != nullptr);
    const core::AnnotationId id = createRect(shell);
    CHECK(id);
    CHECK(shell.annotations->currentSelection().has_value());
    CHECK(shell.session().undo());
    CHECK(!shell.annotations->currentSelection().has_value());
    CHECK(!shell.annotations->selectedId().has_value());
    // Redo does not resurrect the selection.
    CHECK(shell.session().redo());
    CHECK(!shell.annotations->selectedId().has_value());
}

RIVET_TEST(controllerSelectionIsPerTab) {
    Shell shell;
    CHECK(shell.open("tab-a") != nullptr);
    const core::AnnotationId a = createRect(shell);
    CHECK(a);
    const std::size_t firstTab = shell.workspace.activeIndex();
    CHECK(shell.open("tab-b") != nullptr);
    CHECK(!shell.annotations->selectedId().has_value());
    shell.workspace.activateTab(firstTab);
    CHECK(shell.annotations->selectedId() == std::optional<core::AnnotationId>{a});
}

RIVET_TEST(controllerDeleteKeyDeletesTheSelectionThroughTheViewport) {
    Shell shell;
    CHECK(shell.open("delete") != nullptr);
    (void)createRect(shell);
    CHECK_EQ(shell.countOn(0), 1u);
    ui::KeyEvent key;
    key.key = ui::Key::Delete;
    CHECK(shell.viewport->onKey(key));
    CHECK_EQ(shell.countOn(0), 0u);
    CHECK(!shell.annotations->selectedId().has_value());
    // Nothing selected: the key is not consumed by the layer.
    CHECK(shell.session().undo());
    CHECK_EQ(shell.countOn(0), 1u);
    // Backspace too.
    const auto restored = shell.session().annotations().annotations(shell.session().pageId(0));
    shell.annotations->applyIntent([&] {
        Intent select = intent(IntentKind::Select);
        select.id = restored->front().id;
        return select;
    }());
    ui::KeyEvent backspace;
    backspace.key = ui::Key::Backspace;
    CHECK(shell.viewport->onKey(backspace));
    CHECK_EQ(shell.countOn(0), 0u);
}

RIVET_TEST(controllerMoveResizeAndLineEndpointIntentsEachAreOneUndoStep) {
    Shell shell;
    CHECK(shell.open("edit") != nullptr);
    const core::AnnotationId id = createRect(shell);
    Intent move = intent(IntentKind::Move);
    move.id = id;
    move.delta = core::Point{10.0, 20.0};
    shell.annotations->applyIntent(move);
    auto view = shell.viewOf(id, 0);
    CHECK_NEAR(view->bounds.minX(), 100.0 + 10.0 - static_cast<double>(view->style.borderWidth) / 2.0, 1.5);
    Intent resize = intent(IntentKind::Resize);
    resize.id = id;
    resize.rect = core::Rect{50.0, 60.0, 200.0, 100.0};
    shell.annotations->applyIntent(resize);
    view = shell.viewOf(id, 0);
    CHECK_NEAR(view->bounds.size.width, 200.0, 1.5);
    CHECK(shell.session().undo()); // the resize
    view = shell.viewOf(id, 0);
    CHECK_LT(view->bounds.size.width, 100.0);
    CHECK(shell.session().undo()); // the move
    view = shell.viewOf(id, 0);
    CHECK_NEAR(view->bounds.minX(), 100.0 - static_cast<double>(view->style.borderWidth) / 2.0, 1.5);

    shell.annotations->applyIntent(lineIntent(AnnotationTool::Line));
    const core::AnnotationId line = shell.annotations->selectedId().value_or(core::AnnotationId{});
    Intent endpoints = intent(IntentKind::LineEndpoints);
    endpoints.id = line;
    endpoints.a = core::Point{60.0, 60.0};
    endpoints.b = core::Point{300.0, 300.0};
    shell.annotations->applyIntent(endpoints);
    const auto lineView = shell.viewOf(line, 0);
    CHECK_NEAR(lineView->lineEnd.x, 300.0, 1e-3);
    CHECK(shell.lastStatus().empty());
}

RIVET_TEST(controllerRestyleWithSelectionEditsItWithoutSelectionEditsTheToolDefault) {
    Shell shell;
    CHECK(shell.open("restyle") != nullptr);
    AnnotationController& c = *shell.annotations;
    const core::AnnotationId id = createRect(shell);
    const pdf::PdfColor blue = app::kPresetColors[3];
    c.setColor(blue);
    auto view = shell.viewOf(id, 0);
    CHECK(view->style.color == blue);
    // The Rectangle default (red) did not change: restyle went to the annotation.
    CHECK(c.toolStyle(AnnotationTool::Ellipse).style.color == app::defaultToolStyle(AnnotationTool::Ellipse).style.color);
    CHECK(shell.session().undo()); // one undo step reverts the restyle
    view = shell.viewOf(id, 0);
    CHECK(view->style.color == app::defaultToolStyle(AnnotationTool::Rectangle).style.color);

    // No selection: the current tool's default changes, no command runs.
    c.setTool(AnnotationTool::Ellipse);
    CHECK(!c.selectedId().has_value());
    const std::size_t before = shell.countOn(0);
    c.setColor(app::kPresetColors[2]);
    c.setOpacity(0.5F);
    c.setBorderWidth(4.0F);
    c.setFillEnabled(true);
    const app::ToolStyle& ellipse = c.toolStyle(AnnotationTool::Ellipse);
    CHECK(ellipse.style.color == app::kPresetColors[2]);
    CHECK_NEAR(static_cast<double>(ellipse.style.opacity), 0.5, 1e-6);
    CHECK_NEAR(static_cast<double>(ellipse.style.borderWidth), 4.0, 1e-6);
    CHECK(ellipse.style.interiorColor.has_value());
    CHECK_EQ(shell.countOn(0), before);
    // The next creation uses the new default.
    c.applyIntent(shapeIntent(AnnotationTool::Ellipse, core::Rect{20.0, 20.0, 50.0, 50.0}));
    const auto created = shell.viewOf(*c.selectedId(), 0);
    CHECK(created->style.color == app::kPresetColors[2]);
    CHECK_NEAR(static_cast<double>(created->style.borderWidth), 4.0, 1e-6);
    CHECK(created->style.interiorColor.has_value());
    // The displayed style follows the selection.
    CHECK(c.displayedStyle().color == app::kPresetColors[2]);
    // Select tool without selection: nothing to change.
    c.setTool(AnnotationTool::Select);
    const int stateBefore = shell.stateChanges;
    c.setColor(blue);
    CHECK_EQ(shell.stateChanges, stateBefore);
}

RIVET_TEST(controllerMarkupFromTextSelectionIsOneUndoStepAcrossPages) {
    Shell shell;
    DocumentTab* tab = shell.open("markup");
    CHECK(tab != nullptr);
    AnnotationController& c = *shell.annotations;
    editor::DocumentSession& session = shell.session();
    // Selection from page 0 char 2 to page 1 char 4.
    tab->selection().start(editor::TextPosition{session.pageId(0), 2});
    tab->selection().setFocus(editor::TextPosition{session.pageId(1), 4});

    // Text not extracted yet: nothing is created, the user is told.
    c.convertTextSelection(AnnotationTool::Highlight);
    CHECK_EQ(shell.countOn(0), 0u);
    CHECK_EQ(shell.countOn(1), 0u);
    CHECK(shell.lastStatus() == "Annotate: Text not ready yet");
    CHECK(!tab->selection().empty()); // kept: the user can retry

    // The request warmed the pages.
    CHECK(shell.dispatcher.waitUntil([&] {
        return session.textService().cachedTextPage(session.pageId(0)) != nullptr &&
               session.textService().cachedTextPage(session.pageId(1)) != nullptr;
    }));
    shell.statusLog.clear();
    c.convertTextSelection(AnnotationTool::Highlight);
    CHECK(shell.statusLog.empty());
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK_EQ(shell.countOn(1), 1u);
    CHECK(tab->selection().empty()); // cleared afterwards
    const auto list = session.annotations().annotations(session.pageId(0));
    CHECK(list->front().kind == pdf::PdfAnnotationKind::Highlight);
    CHECK_GE(list->front().quads.size(), 1u);
    CHECK_NEAR(static_cast<double>(list->front().style.opacity), 0.4, 1e-6);

    CHECK(session.undo()); // ONE step removes both pages' markup
    CHECK_EQ(shell.countOn(0), 0u);
    CHECK_EQ(shell.countOn(1), 0u);
    CHECK(session.redo());
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK_EQ(shell.countOn(1), 1u);
}

RIVET_TEST(controllerChoosingAMarkupToolWithASelectionConvertsImmediately) {
    Shell shell;
    DocumentTab* tab = shell.open("markup-tool");
    CHECK(tab != nullptr);
    editor::DocumentSession& session = shell.session();
    CHECK(shell.dispatcher.waitUntil([&] {
        session.textService().ensureTextPage(session.pageId(0));
        return session.textService().cachedTextPage(session.pageId(0)) != nullptr;
    }));
    tab->selection().start(editor::TextPosition{session.pageId(0), 1});
    tab->selection().setFocus(editor::TextPosition{session.pageId(0), 5});
    shell.annotations->setTool(AnnotationTool::Underline);
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK(shell.annotations->tool() == AnnotationTool::Underline); // the tool stays
    CHECK(tab->selection().empty());
    // A collapsed (click) selection converts nothing and says nothing.
    tab->selection().start(editor::TextPosition{session.pageId(0), 1});
    shell.statusLog.clear();
    shell.annotations->convertTextSelection(AnnotationTool::Underline);
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK(shell.statusLog.empty());
}

RIVET_TEST(controllerNoteEditorCommitsOneEditContentsCommandOnClose) {
    Shell shell;
    CHECK(shell.open("note") != nullptr);
    AnnotationController& c = *shell.annotations;
    Intent create = intent(IntentKind::CreateNote);
    create.rect = core::Rect{300.0, 300.0, 20.0, 20.0};
    c.applyIntent(create);
    CHECK(c.noteEditorOpen());
    CHECK(shell.focused == &c.noteArea());
    const core::AnnotationId id = *c.selectedId();
    // Anchored inside the viewport frame.
    CHECK(shell.root->children().size() >= 2);

    c.noteArea().setText("first line\nsecond line");
    c.closeNoteEditor();
    CHECK(!c.noteEditorOpen());
    CHECK(shell.focused == nullptr);
    CHECK(shell.viewOf(id, 0)->contents == "first line\nsecond line");
    CHECK(shell.session().undo()); // ONE step: back to the empty note
    CHECK(shell.viewOf(id, 0).has_value());
    CHECK(shell.viewOf(id, 0)->contents.empty());
    CHECK(shell.session().undo()); // creation
    CHECK_EQ(shell.countOn(0), 0u);
}

RIVET_TEST(controllerNoteEditorWithoutChangesRunsNoCommand) {
    Shell shell;
    CHECK(shell.open("note-noop") != nullptr);
    AnnotationController& c = *shell.annotations;
    Intent create = intent(IntentKind::CreateNote);
    create.rect = core::Rect{300.0, 300.0, 20.0, 20.0};
    c.applyIntent(create);
    c.closeNoteEditor();
    CHECK(shell.session().undo()); // the creation is the only step
    CHECK_EQ(shell.countOn(0), 0u);
    CHECK(!shell.session().undo());
}

RIVET_TEST(controllerNoteEditorClosesViaEscapeCommitCommandAndDoneButton) {
    Shell shell;
    CHECK(shell.open("note-close") != nullptr);
    AnnotationController& c = *shell.annotations;
    Intent create = intent(IntentKind::CreateNote);
    create.rect = core::Rect{300.0, 300.0, 20.0, 20.0};
    c.applyIntent(create);
    const core::AnnotationId id = *c.selectedId();

    // Escape inside the text area commits.
    c.noteArea().setText("via escape");
    ui::KeyEvent escape;
    escape.key = ui::Key::Escape;
    CHECK(c.noteArea().onKey(escape));
    CHECK(!c.noteEditorOpen());
    CHECK(shell.viewOf(id, 0)->contents == "via escape");

    // Re-open with Enter on the selected note (layer key path), Cmd+Enter commits.
    ui::KeyEvent enter;
    enter.key = ui::Key::Enter;
    CHECK(shell.viewport->onKey(enter));
    CHECK(c.noteEditorOpen());
    CHECK(c.noteArea().text() == "via escape");
    c.noteArea().setText("via command enter");
    ui::KeyEvent commit;
    commit.key = ui::Key::Enter;
    commit.modifiers.command = true;
    CHECK(c.noteArea().onKey(commit));
    CHECK(shell.viewOf(id, 0)->contents == "via command enter");

    // The Done button commits.
    c.editSelectedNote();
    CHECK(c.noteEditorOpen());
    c.noteArea().setText("via done");
    const core::Rect frame = c.noteDoneButton().frame();
    ui::PointerEvent down{ui::PointerEventType::Down, core::Point{frame.size.width / 2.0, frame.size.height / 2.0}, 1, {}, {}, false};
    ui::PointerEvent up = down;
    up.type = ui::PointerEventType::Up;
    (void)c.noteDoneButton().onMouse(down);
    (void)c.noteDoneButton().onMouse(up);
    CHECK(!c.noteEditorOpen());
    CHECK(shell.viewOf(id, 0)->contents == "via done");
}

RIVET_TEST(controllerNoteEditorCommitsOnToolChangeAndPointerPress) {
    Shell shell;
    CHECK(shell.open("note-tool") != nullptr);
    AnnotationController& c = *shell.annotations;
    Intent create = intent(IntentKind::CreateNote);
    create.rect = core::Rect{300.0, 300.0, 20.0, 20.0};
    c.applyIntent(create);
    const core::AnnotationId id = *c.selectedId();
    c.noteArea().setText("tool change");
    c.setTool(AnnotationTool::Ink);
    CHECK(!c.noteEditorOpen());
    CHECK(shell.viewOf(id, 0)->contents == "tool change");

    c.setTool(AnnotationTool::Select);
    c.applyIntent([&] {
        Intent select = intent(IntentKind::Select);
        select.id = id;
        return select;
    }());
    c.editSelectedNote();
    c.noteArea().setText("click elsewhere");
    c.pointerPressed(); // what the layer does on a press
    CHECK(!c.noteEditorOpen());
    CHECK(shell.viewOf(id, 0)->contents == "click elsewhere");
}

RIVET_TEST(controllerTabSwitchCommitsTheNoteAgainstItsOwnTab) {
    Shell shell;
    CHECK(shell.open("note-tab-a") != nullptr);
    AnnotationController& c = *shell.annotations;
    Intent create = intent(IntentKind::CreateNote);
    create.rect = core::Rect{300.0, 300.0, 20.0, 20.0};
    c.applyIntent(create);
    const core::AnnotationId id = *c.selectedId();
    editor::DocumentSession* firstSession = &shell.session();
    const core::PageId page = firstSession->pageId(0);
    c.noteArea().setText("kept across tabs");

    CHECK(shell.open("note-tab-b") != nullptr); // activates + binds the second tab
    CHECK(!c.noteEditorOpen());
    const auto view = firstSession->annotations().find(page, id);
    CHECK(view.has_value());
    CHECK(view->contents == "kept across tabs");
    CHECK_EQ(shell.countOn(0), 0u); // the new tab was not touched
}

RIVET_TEST(controllerEditingLockRefusesEditsAndReportsIt) {
    Shell shell;
    CHECK(shell.open("locked") != nullptr);
    const core::AnnotationId id = createRect(shell);
    shell.session().setEditingLocked(true, "saving");
    shell.statusLog.clear();

    shell.annotations->applyIntent(shapeIntent(AnnotationTool::Rectangle, core::Rect{10.0, 10.0, 40.0, 40.0}));
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK(!shell.lastStatus().empty());
    CHECK(shell.lastStatus().find("save is in progress") != std::string::npos);

    Intent del = intent(IntentKind::Delete);
    del.id = id;
    shell.statusLog.clear();
    shell.annotations->applyIntent(del);
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK(!shell.statusLog.empty());
    CHECK(!shell.annotations->canPerform(AnnotationCommand::DeleteAnnotation));

    shell.session().setEditingLocked(false);
    CHECK(shell.annotations->canPerform(AnnotationCommand::DeleteAnnotation));
    shell.annotations->applyIntent(del);
    CHECK_EQ(shell.countOn(0), 0u);
}

RIVET_TEST(controllerFailuresAreReportedWithTheAnnotatePrefix) {
    Shell shell;
    CHECK(shell.open("failure") != nullptr);
    // Resizing something that does not exist is NotFound.
    Intent resize = intent(IntentKind::Resize);
    resize.id = core::AnnotationId{987654};
    resize.rect = core::Rect{0.0, 0.0, 10.0, 10.0};
    shell.annotations->applyIntent(resize);
    CHECK(shell.lastStatus().rfind("Annotate: ", 0) == 0);
    CHECK_EQ(shell.countOn(0), 0u);
}

RIVET_TEST(controllerMenuCommandsSelectToolsAndValidate) {
    Shell shell;
    AnnotationController& c = *shell.annotations;
    // No document: nothing is enabled and performing says why.
    CHECK(!c.canPerform(AnnotationCommand::ToolHighlight));
    c.perform(AnnotationCommand::ToolHighlight);
    CHECK(!shell.statusLog.empty());
    CHECK(c.tool() == AnnotationTool::Select);

    CHECK(shell.open("menu") != nullptr);
    CHECK(c.canPerform(AnnotationCommand::ToolHighlight));
    CHECK(!c.canPerform(AnnotationCommand::DeleteAnnotation));
    CHECK(!c.canPerform(AnnotationCommand::EditNote));
    c.perform(AnnotationCommand::ToolRectangle);
    CHECK(c.tool() == AnnotationTool::Rectangle);
    c.perform(AnnotationCommand::ToolSelect);
    CHECK(c.tool() == AnnotationTool::Select);

    (void)createRect(shell);
    CHECK(c.canPerform(AnnotationCommand::DeleteAnnotation));
    CHECK(!c.canPerform(AnnotationCommand::EditNote)); // a rectangle has no text
    c.perform(AnnotationCommand::DeleteAnnotation);
    CHECK_EQ(shell.countOn(0), 0u);
}

RIVET_TEST(controllerToolChangeDeselectsAndCancelsTheGesture) {
    Shell shell;
    CHECK(shell.open("tool") != nullptr);
    AnnotationController& c = *shell.annotations;
    (void)createRect(shell);
    CHECK(c.selectedId().has_value());
    c.setTool(AnnotationTool::Note);
    CHECK(!c.selectedId().has_value());
    // Choosing the same tool again does not deselect a fresh selection.
    (void)createRect(shell);
    c.setTool(AnnotationTool::Note);
    CHECK(c.selectedId().has_value());
}

RIVET_TEST(controllerBindTabEndsGesturesAndPaintsNothingWithoutADocument) {
    Shell shell;
    AnnotationController& c = *shell.annotations;
    CHECK(c.pageAnnotations(0) == nullptr);
    CHECK(!c.currentSelection().has_value());
    c.applyIntent(shapeIntent(AnnotationTool::Rectangle, core::Rect{0.0, 0.0, 10.0, 10.0})); // no tab: ignored
    CHECK(shell.open("bind") != nullptr);
    c.interaction().setTool(AnnotationTool::Rectangle);
    AnnotationInteraction::PointerInput in;
    in.page = 0;
    in.point = core::Point{10.0, 10.0};
    in.pageSize = core::Size{612.0, 792.0};
    (void)c.interaction().pointerDown(in);
    CHECK(c.interaction().gestureActive());
    c.bindTab(shell.tab());
    CHECK(!c.interaction().gestureActive());
}

namespace {

void click(ui::Button& button) {
    const core::Point center{button.frame().size.width / 2.0, button.frame().size.height / 2.0};
    ui::PointerEvent down{ui::PointerEventType::Down, center, 1, {}, {}, false};
    ui::PointerEvent up = down;
    up.type = ui::PointerEventType::Up;
    (void)button.onMouse(down);
    (void)button.onMouse(up);
}

} // namespace

RIVET_TEST(annotationBarShowsToolsAndOnlyTheStyleControlsThatApply) {
    Shell shell;
    CHECK(shell.open("bar") != nullptr);
    app::AnnotationBarController bar(*shell.context, *shell.root, *shell.annotations);
    bar.layout(core::Rect{0.0, 0.0, 1600.0, app::AnnotationBarController::kHeight});
    CHECK(!bar.visible());
    CHECK_EQ(bar.height(), 0.0);
    bar.setVisible(true);
    CHECK_EQ(bar.height(), app::AnnotationBarController::kHeight);
    bar.layout(core::Rect{0.0, 0.0, 1600.0, app::AnnotationBarController::kHeight});

    // Select with nothing selected: tools only.
    CHECK(bar.controlShown(bar.toolButton(AnnotationTool::Ink)));
    CHECK(bar.toolButton(AnnotationTool::Select).active());

    click(bar.toolButton(AnnotationTool::Rectangle));
    CHECK(shell.annotations->tool() == AnnotationTool::Rectangle);
    CHECK(bar.toolButton(AnnotationTool::Rectangle).active());
    CHECK(!bar.toolButton(AnnotationTool::Select).active());
    // The swatch / opacity / width / fill controls are the last children.
    const auto& children = bar.strip().children();
    std::size_t shown = 0;
    for (const auto& child : children) {
        if (!child->frame().isEmpty()) ++shown;
    }
    CHECK_EQ(shown, std::size_t{11 + 6 + 4 + 4 + 1}); // tools, swatches, opacity, widths, fill

    click(bar.toolButton(AnnotationTool::Highlight));
    shown = 0;
    for (const auto& child : children) {
        if (!child->frame().isEmpty()) ++shown;
    }
    CHECK_EQ(shown, std::size_t{11 + 6 + 4}); // no width, no fill

    click(bar.toolButton(AnnotationTool::Stamp));
    shown = 0;
    for (const auto& child : children) {
        if (!child->frame().isEmpty()) ++shown;
    }
    CHECK_EQ(shown, std::size_t{11 + 6 + 1}); // colors + the stamp name

    // Hiding returns to the Select tool.
    bar.setVisible(false);
    CHECK(shell.annotations->tool() == AnnotationTool::Select);
}

RIVET_TEST(annotationBarSwatchRestylesTheSelectionAndMarksTheCurrentColor) {
    Shell shell;
    CHECK(shell.open("bar-style") != nullptr);
    app::AnnotationBarController bar(*shell.context, *shell.root, *shell.annotations);
    bar.setVisible(true);
    bar.layout(core::Rect{0.0, 0.0, 1600.0, app::AnnotationBarController::kHeight});
    const core::AnnotationId id = createRect(shell);
    CHECK(id);
    bar.refresh();
    // Select tool + a selected rectangle: the style controls address it.
    const auto& children = bar.strip().children();
    ui::Button* blueSwatch = nullptr;
    std::size_t swatchIndex = 0;
    for (const auto& child : children) {
        auto* button = static_cast<ui::Button*>(child.get());
        if (!button->swatch().has_value()) continue;
        if (swatchIndex++ == 3) blueSwatch = button;
    }
    CHECK(blueSwatch != nullptr);
    CHECK(!blueSwatch->frame().isEmpty());
    click(*blueSwatch);
    CHECK(shell.viewOf(id, 0)->style.color == app::kPresetColors[3]);
    CHECK(blueSwatch->active());
}
