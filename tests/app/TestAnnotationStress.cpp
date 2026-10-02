// SPDX-License-Identifier: MPL-2.0
// App-level annotation stress scenarios (sanitizer targets): rapid tool
// switching with cancelled gestures, create/delete/undo/redo loops, a seeded
// undo/redo storm, page deletion under a selection, closing a tab while its
// annotation originals are still loading, and saving / quitting while
// annotation edits are in flight. Deterministic: the test thread is the main
// thread, the dispatcher queue is pumped explicitly, and background work is
// parked on the fakes' gates (no sleeps). The editor-level races live in
// tests/editor/TestAnnotationConcurrency.cpp.
#include "RivetTest.h"

#include "Fakes.hpp"
#include "fakes/FakePageDocument.hpp"
#include "fakes/FakeWritableEngine.hpp"

#include "app/AnnotationController.hpp"
#include "app/DocumentWorkspace.hpp"
#include "app/FileController.hpp"
#include "app/PageEditingController.hpp"
#include "app/ShellContext.hpp"
#include "app/SidebarController.hpp"
#include "app/StatusBarController.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSession.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Container.hpp"
#include "ui/PdfViewport.hpp"
#include "ui/TextArea.hpp"
#include "ui/UiTypes.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
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

namespace fs = std::filesystem;

constexpr double kPageWidth = 612.0;
constexpr double kPageHeight = 792.0;

// Main-thread queue; the test thread pumps it.
class WaitDispatcher final : public core::IMainThreadDispatcher {
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
    // Waits for a worker to POST (without delivering anything).
    bool waitPosted(int target) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (posted.load() < target) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    }
    std::atomic<int> posted{0};

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() / ("rivet-annstress-" + std::to_string(++counter));
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

class FakeSaveDialog final : public platform::ISaveDialog {
public:
    std::optional<fs::path> runSavePanel(const Options&) override { return std::nullopt; }
};

// The shell pieces an annotation session touches, wired like ShellController
// (member order = the destruction contract: workspace after the controllers,
// engine and scheduler before everything).
template <class EngineT>
struct BasicShell {
    EngineT engine;
    core::TaskScheduler scheduler{2};
    WaitDispatcher dispatcher;
    TempDir dir;
    std::unique_ptr<DocumentWorkspace> workspaceOwner;
    DocumentWorkspace& workspace;
    FakeSaveDialog saveDialog;
    platform::ShellServices services;

    std::unique_ptr<ui::Container> root = std::make_unique<ui::Container>();
    ui::PdfViewport* viewport = nullptr;
    ui::Widget* focused = nullptr;
    std::vector<std::string> statusLog;
    std::function<void()> onBound;

    std::unique_ptr<ShellContext> context;
    std::unique_ptr<app::SidebarController> sidebar;
    std::unique_ptr<app::StatusBarController> status;
    std::unique_ptr<app::PageEditingController> editing;
    std::unique_ptr<AnnotationController> annotations;
    std::unique_ptr<app::FileController> files;

    template <class... Args>
    explicit BasicShell(Args&&... args)
        : engine(std::forward<Args>(args)...),
          workspaceOwner(std::make_unique<DocumentWorkspace>(engine, scheduler, &dispatcher)),
          workspace(*workspaceOwner) {
        services.mainDispatcher = &dispatcher;
        services.saveDialog = &saveDialog;
        auto vp = std::make_unique<ui::PdfViewport>();
        viewport = vp.get();
        viewport->setFrame(core::Rect{180.0, 68.0, 800.0, 600.0});
        context = std::make_unique<ShellContext>(ShellContext{
            workspace,
            services,
            *viewport,
            [this](std::string message) {
                statusLog.push_back(message);
                if (status != nullptr) status->setStatus(std::move(message));
            },
            [this](ui::Widget* widget) {
                if (focused == widget) return;
                if (focused != nullptr) focused->setFocused(false);
                focused = widget;
                if (focused != nullptr) focused->setFocused(true);
            },
            [] {},
        });
        // As ShellController wires it: a press on the viewport takes the keyboard.
        viewport->setOnFocusRequested([this] { context->setFocus(nullptr); });
        sidebar = std::make_unique<app::SidebarController>(*context, *root);
        root->addChild(std::move(vp));
        status = std::make_unique<app::StatusBarController>(*context, *root, "Ready");
        editing = std::make_unique<app::PageEditingController>(*context, *sidebar, *status);
        annotations = std::make_unique<AnnotationController>(*context, *root);
        files = std::make_unique<app::FileController>(
            engine, *context, scheduler, [this](std::string text) { statusLog.push_back(std::move(text)); }, [] {});
        sidebar->layout(core::Rect{0.0, 68.0, app::SidebarController::kWidth, 600.0});
        status->layout(core::Rect{0.0, 668.0, 1020.0, app::StatusBarController::kHeight});
        annotations->layout(viewport->frame());
        workspace.setOnActiveTabChanged([this] { bind(); });
    }

    // The application quit: controllers first, then the workspace (and every
    // session in it) while background work may still be running.
    void quit() {
        if (workspaceOwner == nullptr) return;
        workspace.setOnActiveTabChanged({});
        files.reset();
        annotations.reset();
        editing.reset();
        status.reset();
        sidebar.reset();
        viewport->clearDocument();
        workspaceOwner.reset();
    }

    ~BasicShell() { quit(); }

    void bind() {
        DocumentTab* tab = workspace.activeTab();
        if (tab == nullptr || tab->state() != DocumentTab::State::Ready) {
            viewport->clearDocument();
            sidebar->bindTab(nullptr);
            editing->bindTab(nullptr);
            annotations->bindTab(nullptr);
        } else {
            editor::DocumentSession* session = tab->session();
            viewport->setDocument(session->id(), &session->layout(), &session->renderSource(),
                                  [session] { return session->revision(); }, &tab->viewState());
            sidebar->bindTab(tab);
            editing->bindTab(tab);
            annotations->bindTab(tab);
        }
        if (onBound) onBound();
    }

    // Opens a fresh document; with `loadOriginals` every page's originals
    // are loaded (so resolved lists are complete). A raised pageCount only
    // applies to engines that support it (the writable fake).
    DocumentTab* open(const std::string& name, bool loadOriginals = true, std::size_t pages = 3) {
        if (std::FILE* file = std::fopen(dir(name).string().c_str(), "wb")) {
            std::fputs("%PDF-1.4\n", file);
            std::fclose(file);
        }
        if constexpr (requires { engine.pageCounts; }) engine.pageCounts[name] = pages;
        const std::size_t before = workspace.tabCount();
        workspace.openDocument(dir(name));
        if (!dispatcher.waitUntil([this, before] {
                DocumentTab* active = workspace.activeTab();
                return workspace.tabCount() > before && active != nullptr &&
                       active->state() == DocumentTab::State::Ready;
            })) {
            return nullptr;
        }
        DocumentTab* active = workspace.activeTab();
        if (loadOriginals) {
            for (std::size_t i = 0; i < active->session()->pageCount(); ++i) loadPage(*active->session(), i);
        }
        return active;
    }

    void loadPage(editor::DocumentSession& session, std::size_t index) {
        const std::size_t before = session.annotations().cachedOriginalPages();
        session.annotations().annotations(session.pageId(index));
        dispatcher.waitUntil([&] { return session.annotations().cachedOriginalPages() > before; });
    }

    DocumentTab* tab() { return workspace.activeTab(); }
    editor::DocumentSession& session() { return *tab()->session(); }
    std::size_t depth() { return session().commands().depth(); }

    std::size_t countOn(std::size_t page) {
        auto list = session().annotations().annotations(session().pageId(page));
        return list ? list->size() : 0;
    }
    std::size_t totalCount() {
        std::size_t total = 0;
        for (std::size_t i = 0; i < session().pageCount(); ++i) total += countOn(i);
        return total;
    }
    std::optional<editor::AnnotationView> viewOf(core::AnnotationId id, std::size_t page) {
        return session().annotations().find(session().pageId(page), id);
    }
    std::string lastStatus() const { return statusLog.empty() ? std::string() : statusLog.back(); }
    bool hasStatus(const std::string& prefix) const {
        return !statusLog.empty() && statusLog.back().rfind(prefix, 0) == 0;
    }
    void paint() {
        ui::testing::FakePaintContext paintContext;
        viewport->paint(paintContext);
    }
};

using PageShell = BasicShell<test::FakePageEngine>;
using FileShell = BasicShell<test::FakeWritableEngine>;

// --- Intents and gestures --------------------------------------------------------

Intent intent(IntentKind kind, std::size_t page = 0) {
    Intent i;
    i.kind = kind;
    i.page = page;
    return i;
}

Intent shapeIntent(AnnotationTool tool, core::Rect rect, std::size_t page = 0) {
    Intent i = intent(IntentKind::CreateShape, page);
    i.tool = tool;
    i.rect = rect;
    return i;
}

Intent selectIntent(core::AnnotationId id) {
    Intent i = intent(IntentKind::Select);
    i.id = id;
    return i;
}

template <class ShellT>
core::AnnotationId createRect(ShellT& shell, core::Rect rect = core::Rect{100.0, 100.0, 80.0, 40.0},
                              std::size_t page = 0) {
    shell.annotations->applyIntent(shapeIntent(AnnotationTool::Rectangle, rect, page));
    return shell.annotations->selectedId().value_or(core::AnnotationId{});
}

ui::KeyEvent keyEvent(ui::Key key) {
    ui::KeyEvent event;
    event.key = key;
    return event;
}

AnnotationInteraction::PointerInput pointer(std::size_t page, core::Point point) {
    AnnotationInteraction::PointerInput in;
    in.page = page;
    in.point = point;
    in.pageSize = core::Size{kPageWidth, kPageHeight};
    return in;
}

// What AnnotationLayer does for a press / move / release (the layer is owned
// by the controller; the controller is the layer's client).
void press(AnnotationController& c, std::size_t page, core::Point point) {
    c.pointerPressed();
    c.interaction().setSelection(c.currentSelection());
    c.applyIntent(c.interaction().pointerDown(pointer(page, point)));
}

void move(AnnotationController& c, std::size_t page, core::Point point) {
    if (!c.interaction().gestureActive()) return;
    (void)c.interaction().pointerMove(pointer(page, point));
}

void release(AnnotationController& c, std::size_t page, core::Point point) {
    if (!c.interaction().gestureActive()) return;
    c.applyIntent(c.interaction().pointerUp(pointer(page, point)));
}

bool createsOnCompletion(AnnotationTool tool) {
    switch (tool) {
    case AnnotationTool::Note:
    case AnnotationTool::Ink:
    case AnnotationTool::Rectangle:
    case AnnotationTool::Ellipse:
    case AnnotationTool::Line:
    case AnnotationTool::Arrow:
    case AnnotationTool::Stamp:
        return true;
    default:
        return false;
    }
}

// Releases the gate of a fake document on scope exit: a failing CHECK must
// not leave a parked worker behind (the session destructor waits for it).
struct DocGateGuard {
    test::FakePageDocument* document;
    ~DocGateGuard() {
        if (document != nullptr) document->releaseAnnotationGate();
    }
};

struct EngineGateGuard {
    test::FakeWritableEngine& engine;
    ~EngineGateGuard() { engine.release(); }
};

} // namespace

// --- 1. Rapid tool switching ---------------------------------------------------

RIVET_TEST(stressRapidToolSwitchingWithCancelledGestures) {
    PageShell shell(3);
    CHECK(shell.open("tools") != nullptr);
    AnnotationController& c = *shell.annotations;
    shell.session().commands().setMaxDepth(10000);

    std::size_t completed = 0;
    for (int i = 0; i < 500; ++i) {
        const AnnotationTool tool = static_cast<AnnotationTool>(i % static_cast<int>(app::kAnnotationToolCount));
        const int variant = (i / static_cast<int>(app::kAnnotationToolCount)) % 3; // 0 complete, 1 tool switch, 2 Esc
        const std::size_t page = static_cast<std::size_t>(i) % 3;
        const std::size_t local = static_cast<std::size_t>(i) / 3;
        // A fresh 36-point cell per gesture: nothing is ever hit, so every
        // press starts the tool (and only the tool).
        const core::Point origin{10.0 + 36.0 * static_cast<double>(local % 16),
                                 10.0 + 36.0 * static_cast<double>(local / 16)};

        c.perform(static_cast<AnnotationCommand>(tool));
        CHECK(c.tool() == tool);
        const std::size_t depthBefore = shell.depth();
        const std::size_t countBefore = shell.totalCount();

        press(c, page, origin);
        // Drag (a note is a click: no travel).
        if (tool != AnnotationTool::Note) {
            move(c, page, core::Point{origin.x + 4.0, origin.y + 3.0});
            move(c, page, core::Point{origin.x + 12.0, origin.y + 9.0});
            move(c, page, core::Point{origin.x + 24.0, origin.y + 16.0});
        }
        bool cancelled = false;
        if (variant == 1) {
            // A tool switch (menu / toolbar) in the middle of the press.
            c.perform(static_cast<AnnotationCommand>((static_cast<int>(tool) + 1) %
                                                     static_cast<int>(app::kAnnotationToolCount)));
            CHECK(!c.interaction().gestureActive());
            cancelled = true;
        } else if (variant == 2) {
            // Esc through the viewport (the layer's key path).
            const bool wasInProgress = c.interaction().gestureInProgress();
            const bool consumed = shell.viewport->onKey(keyEvent(ui::Key::Escape));
            if (wasInProgress) {
                CHECK(consumed);
                cancelled = true;
            } else {
                cancelled = false; // Select / markup tools: no gesture to cancel
            }
        }
        // The release: ignored by the layer when no gesture is active (a
        // cancelled or swallowed press is not a creation).
        release(c, page, tool == AnnotationTool::Note ? origin : core::Point{origin.x + 24.0, origin.y + 16.0});
        CHECK(!c.interaction().gestureActive());
        c.closeNoteEditor();

        const bool creates = variant == 0 && createsOnCompletion(tool);
        if (creates) ++completed;
        CHECK_EQ(shell.depth(), depthBefore + (creates ? 1u : 0u));
        CHECK_EQ(shell.totalCount(), countBefore + (creates ? 1u : 0u));
        if (cancelled) CHECK_EQ(shell.depth(), depthBefore);
        // A stray release / move without a gesture never creates anything.
        const Intent stray = c.interaction().pointerUp(pointer(page, origin));
        CHECK(!stray.consumed);
        c.applyIntent(stray);
        CHECK_EQ(shell.depth(), depthBefore + (creates ? 1u : 0u));
    }
    CHECK_EQ(shell.depth(), completed);
    CHECK_EQ(shell.totalCount(), completed);
    CHECK(completed > 100);
    // Everything undoes cleanly, one step per completed gesture.
    for (std::size_t i = 0; i < completed; ++i) CHECK(shell.session().undo());
    CHECK(!shell.session().undo());
    CHECK_EQ(shell.totalCount(), 0u);
    CHECK(!shell.session().isDirty());
}

// --- 2. Create / delete repeatedly ------------------------------------------------

RIVET_TEST(stressCreateDeleteUndoRedoLoopKeepsIdsSelectionAndDirtyStateConsistent) {
    PageShell shell(3);
    CHECK(shell.open("loop") != nullptr);
    AnnotationController& c = *shell.annotations;
    editor::DocumentSession& session = shell.session();
    CHECK(!session.isDirty());

    std::set<core::AnnotationId> everSeen;
    for (int i = 0; i < 200; ++i) {
        const core::Rect rect{20.0 + static_cast<double>(i % 20) * 20.0, 30.0 + static_cast<double>(i % 30) * 15.0,
                              40.0, 24.0};
        c.applyIntent(shapeIntent(AnnotationTool::Rectangle, rect));
        const core::AnnotationId id = c.selectedId().value_or(core::AnnotationId{});
        CHECK(id);
        CHECK(everSeen.insert(id).second); // never reused, not even after undo + new creation
        CHECK(session.isDirty());
        CHECK_EQ(shell.countOn(0), 1u);

        if (i % 25 == 24) {
            // Saved-state tracking: undoing back to the marked state is clean.
            session.markSaved();
            CHECK(!session.isDirty());
            c.applyIntent(selectIntent(id));
            CHECK(c.selectedId() == std::optional<core::AnnotationId>{id});
            CHECK(shell.viewport->onKey(keyEvent(ui::Key::Delete)));
            CHECK(session.isDirty());
            CHECK(!c.selectedId().has_value());
            CHECK(session.undo());
            CHECK(!session.isDirty()); // the saved state again
            CHECK(shell.viewOf(id, 0).has_value());
            CHECK(session.redo());
            CHECK(session.isDirty());
            CHECK(session.undo());
            CHECK(!session.isDirty());
            // A new command after the undo never matches the saved state ...
            const core::AnnotationId other = createRect(shell, rect);
            CHECK(other);
            CHECK(everSeen.insert(other).second);
            CHECK(session.isDirty());
            CHECK(session.undo());
            CHECK(!session.isDirty()); // ... but undoing it returns to it
            CHECK(session.undo());     // the creation of `id`: before the saved state
            CHECK(session.isDirty());
            session.markSaved();       // new baseline: nothing on the page
            CHECK(!session.isDirty());
            CHECK_EQ(shell.countOn(0), 0u);
            continue;
        }

        if (i % 2 == 0) {
            // select -> Delete key -> undo -> redo
            c.applyIntent(selectIntent(id));
            CHECK(c.selectedId() == std::optional<core::AnnotationId>{id});
            CHECK(shell.viewport->onKey(keyEvent(ui::Key::Delete)));
            CHECK_EQ(shell.countOn(0), 0u);
            CHECK(!c.selectedId().has_value());
            CHECK(session.undo());
            CHECK_EQ(shell.countOn(0), 1u);
            CHECK(shell.viewOf(id, 0).has_value()); // same id
            CHECK(!c.selectedId().has_value());     // deselected by the delete, not resurrected
            CHECK(session.redo());
            CHECK_EQ(shell.countOn(0), 0u);
            CHECK(!shell.viewOf(id, 0).has_value());
            CHECK(session.isDirty());
            CHECK(session.undo()); // delete undone
            CHECK(session.undo()); // creation undone
        } else {
            // undo of the creation prunes the selection lazily; redo does not resurrect it
            CHECK(session.undo());
            CHECK(!c.selectedId().has_value());
            CHECK(!c.currentSelection().has_value());
            // Selecting something that no longer resolves leaves nothing selected.
            c.applyIntent(selectIntent(id));
            CHECK(!c.selectedId().has_value());
            CHECK(session.redo());
            CHECK(!c.selectedId().has_value());
            CHECK(shell.viewOf(id, 0).has_value());
            CHECK(session.undo());
        }
        CHECK_EQ(shell.countOn(0), 0u);
        CHECK(!session.isDirty()); // back at the (re)saved state
        CHECK(!c.selectedId().has_value());
    }
    CHECK_EQ(everSeen.size(), 208u); // 200 loop creations + 8 "other" creations (every 25th iteration)
}

// --- 3. Undo / redo storm ------------------------------------------------------------

namespace {

template <class ShellT>
void checkStormInvariants(ShellT& shell, std::set<core::AnnotationId>& everSeen) {
    editor::DocumentSession& session = shell.session();
    std::set<core::AnnotationId> ids;
    for (std::size_t page = 0; page < session.pageCount(); ++page) {
        const auto list = session.annotations().annotations(session.pageId(page));
        CHECK(list != nullptr);
        for (const editor::AnnotationView& view : *list) {
            CHECK(view.id);
            CHECK(ids.insert(view.id).second); // unique across all pages
            everSeen.insert(view.id);
            // Every listed annotation locates (selectable / editable).
            CHECK(session.annotations().locate(view.id).has_value());
        }
    }
    if (const auto selected = shell.annotations->selectedId(); selected.has_value()) {
        CHECK(ids.count(*selected) == 1u);
        CHECK(session.annotations().locate(*selected).has_value());
        const auto current = shell.annotations->currentSelection();
        CHECK(current.has_value());
        if (current.has_value()) CHECK(current->info.id == *selected);
    } else {
        CHECK(!shell.annotations->currentSelection().has_value());
    }
}

} // namespace

RIVET_TEST(stressSeededUndoRedoStormKeepsIdsUniqueAndTheSelectionResolving) {
    PageShell shell(3);
    CHECK(shell.open("storm") != nullptr);
    AnnotationController& c = *shell.annotations;
    editor::DocumentSession& session = shell.session();
    session.commands().setMaxDepth(10000); // the whole storm stays undoable
    std::mt19937 rng(0xA11CEu);
    auto pick = [&rng](std::uint32_t n) { return static_cast<std::uint32_t>(rng() % n); };
    auto coord = [&](double span) { return static_cast<double>(pick(1000)) / 1000.0 * span; };

    std::set<core::AnnotationId> everSeen;
    std::size_t executed = 0;
    for (int step = 0; step < 300; ++step) {
        const std::uint32_t op = pick(16);
        const std::size_t page = pick(3);
        const std::size_t before = shell.totalCount();
        const std::optional<core::AnnotationId> selected = c.selectedId();
        switch (op) {
        case 0:
        case 1:
            c.applyIntent(shapeIntent(op == 0 ? AnnotationTool::Rectangle : AnnotationTool::Ellipse,
                                      core::Rect{coord(400.0), coord(600.0), 20.0 + coord(80.0), 20.0 + coord(60.0)},
                                      page));
            break;
        case 2: {
            Intent note = intent(IntentKind::CreateNote, page);
            note.rect = core::Rect{coord(500.0), coord(700.0), 20.0, 20.0};
            c.applyIntent(note);
            c.closeNoteEditor();
            break;
        }
        case 3: {
            Intent ink = intent(IntentKind::CreateInk, page);
            const double x = coord(400.0);
            const double y = coord(600.0);
            ink.strokes = {{core::Point{x, y}, core::Point{x + 10.0, y + 3.0}, core::Point{x + 20.0, y + 25.0},
                            core::Point{x + 40.0, y + 20.0}}};
            c.applyIntent(ink);
            break;
        }
        case 4: {
            Intent line = intent(IntentKind::CreateShape, page);
            line.tool = (pick(2) == 0) ? AnnotationTool::Line : AnnotationTool::Arrow;
            line.a = core::Point{coord(300.0), coord(300.0)};
            line.b = core::Point{line.a.x + 30.0 + coord(100.0), line.a.y + 20.0 + coord(100.0)};
            c.applyIntent(line);
            break;
        }
        case 5: {
            Intent stamp = intent(IntentKind::CreateStamp, page);
            stamp.rect = core::Rect{coord(300.0), coord(600.0), 90.0, 40.0};
            c.applyIntent(stamp);
            break;
        }
        case 6: {
            Intent mv = intent(IntentKind::Move);
            mv.id = selected.value_or(core::AnnotationId{1 + pick(50)});
            mv.delta = core::Point{coord(40.0) - 20.0, coord(40.0) - 20.0};
            c.applyIntent(mv);
            break;
        }
        case 7: {
            Intent rz = intent(IntentKind::Resize);
            rz.id = selected.value_or(core::AnnotationId{1 + pick(50)});
            rz.rect = core::Rect{coord(300.0), coord(500.0), 10.0 + coord(150.0), 10.0 + coord(100.0)};
            c.applyIntent(rz);
            break;
        }
        case 8:
            c.perform(AnnotationCommand::DeleteAnnotation);
            break;
        case 9:
        case 10:
        case 11:
            if (pick(2) == 0) (void)session.undo();
            else (void)shell.editing->perform(app::PageEditCommand::Undo);
            break;
        case 12:
        case 13:
            if (pick(2) == 0) (void)session.redo();
            else (void)shell.editing->perform(app::PageEditCommand::Redo);
            break;
        case 14:
            c.setColor(app::kPresetColors[pick(6)]);
            break;
        default: {
            // Select a random id, live or not.
            const auto list = session.annotations().annotations(session.pageId(page));
            Intent sel = intent(IntentKind::Select);
            sel.id = (!list->empty() && pick(3) != 0) ? (*list)[pick(static_cast<std::uint32_t>(list->size()))].id
                                                      : core::AnnotationId{1 + pick(500)};
            c.applyIntent(sel);
            break;
        }
        }
        if (shell.totalCount() > before && op <= 5) {
            // A creation: its id is new (never reissued, even after undos).
            const auto created = c.selectedId();
            CHECK(created.has_value());
            if (created.has_value()) CHECK(everSeen.count(*created) == 0u);
        }
        ++executed;
        checkStormInvariants(shell, everSeen);
    }
    CHECK_EQ(executed, 300u);
    CHECK_GE(everSeen.size(), 20u); // the storm really created things
    // Unwinding everything leaves a consistent, clean model.
    while (session.undo()) {}
    CHECK_EQ(shell.totalCount(), 0u);
    CHECK(!session.isDirty());
    checkStormInvariants(shell, everSeen);
}

// --- 4. Page delete with a selected annotation -------------------------------------

RIVET_TEST(stressPageDeleteWithSelectedAnnotationClearsSelectionLazilyAndUndoRestoresIt) {
    PageShell shell(3);
    CHECK(shell.open("pagedel") != nullptr);
    AnnotationController& c = *shell.annotations;
    editor::DocumentSession& session = shell.session();

    const core::AnnotationId onFirst = createRect(shell, core::Rect{50.0, 50.0, 60.0, 30.0}, 0);
    const core::AnnotationId onSecond = createRect(shell, core::Rect{100.0, 120.0, 80.0, 40.0}, 1);
    const core::AnnotationId onThird = createRect(shell, core::Rect{70.0, 200.0, 50.0, 50.0}, 2);
    CHECK(onFirst && onSecond && onThird);
    c.applyIntent(selectIntent(onSecond));
    CHECK(c.selectedId() == std::optional<core::AnnotationId>{onSecond});
    const core::PageId deletedPage = session.pageId(1);
    const std::size_t depthBefore = shell.depth();

    shell.editing->handleRowClicked(1, ui::PageThumbnailList::ClickGesture::Replace);
    shell.editing->deletePages();
    CHECK_EQ(session.pageCount(), 2u);
    CHECK(session.pageIndexFor(deletedPage) == editor::DocumentSession::kInvalidPage);
    CHECK_EQ(shell.depth(), depthBefore + 1);

    // Painting right away (before anything asked about the selection) must not
    // touch the dead annotation: the layer resolves the selection lazily.
    shell.paint();
    CHECK(!c.selectedId().has_value()); // pruned
    CHECK(!c.currentSelection().has_value());
    CHECK(!c.canPerform(AnnotationCommand::DeleteAnnotation));
    // Delete does nothing harmful: no command, the other pages untouched.
    const std::size_t depthAfterDelete = shell.depth();
    (void)shell.viewport->onKey(keyEvent(ui::Key::Delete));
    (void)shell.viewport->onKey(keyEvent(ui::Key::Backspace));
    c.perform(AnnotationCommand::DeleteAnnotation);
    c.perform(AnnotationCommand::EditNote);
    CHECK_EQ(shell.depth(), depthAfterDelete);
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK_EQ(shell.countOn(1), 1u); // the former third page
    CHECK(shell.viewOf(onFirst, 0).has_value());
    CHECK(shell.viewOf(onThird, 1).has_value());
    // A restyle without a selection never reaches the removed annotation.
    c.setColor(app::kPresetColors[3]);
    CHECK_EQ(shell.depth(), depthAfterDelete);
    // Intents addressed to the removed annotation fail cleanly.
    Intent mv = intent(IntentKind::Move);
    mv.id = onSecond;
    mv.delta = core::Point{5.0, 5.0};
    shell.statusLog.clear();
    c.applyIntent(mv);
    CHECK(shell.lastStatus().rfind("Annotate: ", 0) == 0);
    CHECK_EQ(shell.depth(), depthAfterDelete);

    // Undo brings the page and the annotation (same id) back.
    CHECK(session.undo());
    CHECK_EQ(session.pageCount(), 3u);
    CHECK(session.pageIndexFor(deletedPage) == 1u);
    CHECK(shell.viewOf(onSecond, 1).has_value());
    CHECK_EQ(shell.countOn(1), 1u);
    CHECK(shell.viewOf(onThird, 2).has_value());
    CHECK(!c.selectedId().has_value()); // not resurrected
    shell.paint();
    // ... and it is selectable and deletable again.
    c.applyIntent(selectIntent(onSecond));
    CHECK(c.selectedId() == std::optional<core::AnnotationId>{onSecond});
    CHECK(shell.viewport->onKey(keyEvent(ui::Key::Delete)));
    CHECK_EQ(shell.countOn(1), 0u);
    CHECK(session.undo()); // the delete
    CHECK(shell.viewOf(onSecond, 1).has_value());
    CHECK_EQ(session.pageCount(), 3u);
}

RIVET_TEST(stressPageDeleteWhileANoteEditorIsOpenOnThatPageCommitsNothingAndKeepsTheModel) {
    PageShell shell(3);
    CHECK(shell.open("pagedel-note") != nullptr);
    AnnotationController& c = *shell.annotations;
    editor::DocumentSession& session = shell.session();

    Intent note = intent(IntentKind::CreateNote, 2);
    note.rect = core::Rect{80.0, 80.0, 20.0, 20.0};
    c.applyIntent(note);
    CHECK(c.noteEditorOpen());
    const core::AnnotationId id = *c.selectedId();
    c.noteArea().setText("typed before the page vanished");
    const std::size_t depthBefore = shell.depth();

    shell.editing->handleRowClicked(2, ui::PageThumbnailList::ClickGesture::Replace);
    shell.editing->deletePages();
    CHECK_EQ(session.pageCount(), 2u);
    shell.paint(); // the open editor's anchor no longer resolves
    c.layout(shell.viewport->frame());
    // Closing the editor tries to commit against a removed annotation: refused
    // and reported, nothing changes.
    shell.statusLog.clear();
    c.closeNoteEditor();
    CHECK(!c.noteEditorOpen());
    CHECK(shell.lastStatus().rfind("Annotate: ", 0) == 0);
    CHECK_EQ(shell.depth(), depthBefore + 1); // only the page deletion
    CHECK(!c.selectedId().has_value());
    // Undo restores the page and the annotation with its original (empty) contents.
    CHECK(session.undo());
    CHECK_EQ(session.pageCount(), 3u);
    const auto view = shell.viewOf(id, 2);
    CHECK(view.has_value());
    if (view.has_value()) CHECK(view->contents.empty());
}

// --- 5. Close tab during annotation load ----------------------------------------------

RIVET_TEST(stressCloseTabWhileOriginalsAreLoadingDropsTheLateCompletion) {
    PageShell shell(3);
    DocumentTab* tab = shell.open("loading-a", false);
    CHECK(tab != nullptr);
    AnnotationController& c = *shell.annotations;
    test::FakePageDocument* document = shell.engine.lastDocument;
    CHECK(document != nullptr);
    DocGateGuard guard{document};
    document->closeAnnotationGate();
    // Page 0's originals are requested: the load parks inside the document.
    CHECK(tab->session()->annotations().annotations(tab->session()->pageId(0)) != nullptr);
    CHECK(document->waitAnnotationParked(1));
    CHECK_EQ(tab->session()->annotations().cachedOriginalPages(), 0u);

    // Annotate meanwhile: a completed gesture (overlay-only list) ...
    c.perform(AnnotationCommand::ToolRectangle);
    press(c, 0, core::Point{40.0, 40.0});
    move(c, 0, core::Point{60.0, 55.0});
    move(c, 0, core::Point{90.0, 80.0});
    release(c, 0, core::Point{90.0, 80.0});
    CHECK_EQ(shell.countOn(0), 1u);
    // ... and one more held while the tab goes away.
    press(c, 0, core::Point{300.0, 300.0});
    move(c, 0, core::Point{340.0, 330.0});
    CHECK(c.interaction().gestureActive());
    shell.paint();

    // The load finishes: its completion is posted but NOT delivered yet.
    const int posted = shell.dispatcher.posted.load();
    document->releaseAnnotationGate();
    guard.document = nullptr;
    CHECK(shell.dispatcher.waitPosted(posted + 1));

    int changed = 0;
    tab->session()->setOnAnnotationsChanged([&changed](core::PageId) { ++changed; });
    shell.workspace.closeTab(shell.workspace.activeIndex());
    CHECK_EQ(shell.workspace.tabCount(), 0u);
    CHECK(!c.interaction().gestureActive());
    CHECK(!c.selectedId().has_value());
    CHECK(c.pageAnnotations(0) == nullptr);
    // The queued delivery refers to a dead service: dropped by its liveness flag.
    shell.dispatcher.pump();
    CHECK(shell.dispatcher.waitUntil([] { return true; }));
    CHECK_EQ(changed, 0);
    CHECK(c.pageAnnotations(0) == nullptr);
    CHECK(shell.statusLog.empty());
    // Late input is harmless.
    release(c, 0, core::Point{340.0, 330.0});
    shell.viewport->onKey(keyEvent(ui::Key::Delete));
    shell.paint();

    // The shell keeps working with the next document.
    CHECK(shell.open("loading-a2") != nullptr);
    c.perform(AnnotationCommand::ToolSelect);
    CHECK(createRect(shell).operator bool());
    CHECK_EQ(shell.countOn(0), 1u);
}

RIVET_TEST(stressCloseTabWhileOriginalsAreLoadingAndTheLoadFinishesDuringTheClose) {
    PageShell shell(3);
    DocumentTab* tab = shell.open("loading-b", false);
    CHECK(tab != nullptr);
    AnnotationController& c = *shell.annotations;
    test::FakePageDocument* document = shell.engine.lastDocument;
    CHECK(document != nullptr);
    DocGateGuard guard{document};
    document->closeAnnotationGate();
    CHECK(tab->session()->annotations().annotations(tab->session()->pageId(1)) != nullptr);
    CHECK(document->waitAnnotationParked(1));

    // A note being edited with unsaved text, a selection, and a held gesture.
    Intent note = intent(IntentKind::CreateNote, 1);
    note.rect = core::Rect{60.0, 60.0, 20.0, 20.0};
    c.applyIntent(note);
    CHECK(c.noteEditorOpen());
    c.noteArea().setText("pending text");
    c.perform(AnnotationCommand::ToolInk);
    press(c, 1, core::Point{200.0, 200.0});
    move(c, 1, core::Point{210.0, 215.0});
    CHECK(c.interaction().gestureActive());

    // Release the gate from another thread once the close has started (the
    // hook fires while the closing tab is still alive): the session destructor
    // then waits for the in-flight load.
    std::mutex mutex;
    std::condition_variable cv;
    bool closing = false;
    std::thread releaser([&] {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return closing; });
        document->releaseAnnotationGate(); // the load job still owns the document
    });
    shell.onBound = [&] {
        if (shell.workspace.activeTab() != nullptr) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
        }
        cv.notify_all();
    };
    shell.workspace.closeTab(shell.workspace.activeIndex());
    releaser.join();
    guard.document = nullptr;
    shell.onBound = nullptr;
    CHECK_EQ(shell.workspace.tabCount(), 0u);
    CHECK(!c.noteEditorOpen());
    CHECK(!c.interaction().gestureActive());
    CHECK(c.pageAnnotations(0) == nullptr);
    shell.dispatcher.pump(); // the completion posted by the finished load is dropped
    CHECK(shell.dispatcher.waitUntil([] { return true; }));
    release(c, 1, core::Point{210.0, 215.0});
    shell.paint();
    CHECK(!c.selectedId().has_value());
}

// --- 6. Save during / after annotation mutation ---------------------------------------

RIVET_TEST(stressAnnotationIntentsAreRefusedWhileASaveHoldsTheEditingLock) {
    FileShell shell;
    DocumentTab* tab = shell.open("save.pdf", true, 3);
    CHECK(tab != nullptr);
    AnnotationController& c = *shell.annotations;
    editor::DocumentSession& session = shell.session();

    const core::AnnotationId id = createRect(shell, core::Rect{100.0, 100.0, 80.0, 40.0}, 0);
    CHECK(id);
    CHECK(session.isDirty());
    c.applyIntent(selectIntent(id));

    EngineGateGuard gate{shell.engine};
    shell.engine.closeGate();
    shell.files->save(*tab);
    CHECK(shell.engine.waitParked(1));
    CHECK(session.isEditingLocked());

    // Snapshot of the model while the lock is held.
    const std::size_t depthBefore = shell.depth();
    const std::uint64_t stateBefore = session.commands().stateId();
    const auto viewBefore = shell.viewOf(id, 0);
    CHECK(viewBefore.has_value());
    const std::size_t countBefore = shell.totalCount();
    auto expectUnchanged = [&] {
        CHECK_EQ(shell.depth(), depthBefore);
        CHECK(session.commands().stateId() == stateBefore);
        CHECK_EQ(shell.totalCount(), countBefore);
        const auto now = shell.viewOf(id, 0);
        CHECK(now.has_value());
        if (now.has_value() && viewBefore.has_value()) {
            CHECK(now->bounds == viewBefore->bounds);
            CHECK(now->style.color == viewBefore->style.color);
        }
        CHECK(session.isDirty());
    };
    auto expectRefused = [&] {
        CHECK(!shell.statusLog.empty());
        CHECK(shell.lastStatus().find("save is in progress") != std::string::npos);
        expectUnchanged();
    };

    shell.statusLog.clear();
    c.applyIntent(shapeIntent(AnnotationTool::Ellipse, core::Rect{10.0, 10.0, 40.0, 40.0}));
    expectRefused();

    Intent mv = intent(IntentKind::Move);
    mv.id = id;
    mv.delta = core::Point{10.0, 10.0};
    shell.statusLog.clear();
    c.applyIntent(mv);
    expectRefused();

    Intent rz = intent(IntentKind::Resize);
    rz.id = id;
    rz.rect = core::Rect{20.0, 20.0, 100.0, 60.0};
    shell.statusLog.clear();
    c.applyIntent(rz);
    expectRefused();

    shell.statusLog.clear();
    c.setColor(app::kPresetColors[3]);
    expectRefused();

    Intent del = intent(IntentKind::Delete);
    del.id = id;
    shell.statusLog.clear();
    c.applyIntent(del);
    expectRefused();
    CHECK(c.selectedId() == std::optional<core::AnnotationId>{id}); // a refused delete keeps the selection

    CHECK(!c.canPerform(AnnotationCommand::DeleteAnnotation));
    shell.statusLog.clear();
    c.perform(AnnotationCommand::DeleteAnnotation);
    expectRefused();

    shell.statusLog.clear();
    (void)shell.viewport->onKey(keyEvent(ui::Key::Delete));
    expectRefused();

    // A drag gesture ending under the lock is refused too (nothing is created).
    c.perform(AnnotationCommand::ToolRectangle);
    shell.statusLog.clear();
    press(c, 1, core::Point{300.0, 300.0});
    move(c, 1, core::Point{340.0, 330.0});
    release(c, 1, core::Point{360.0, 340.0});
    expectRefused();
    c.perform(AnnotationCommand::ToolSelect);

    // Undo / redo refuse while locked.
    CHECK(!session.undo());
    CHECK(!session.redo());
    expectUnchanged();

    // Complete the save: clean, ids preserved, edits afterwards work.
    shell.engine.release();
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!session.isDirty());
    CHECK(!session.isEditingLocked());
    CHECK_EQ(shell.engine.lastEdits.empty() ? 0u : shell.engine.lastEdits[0].create.size(), 1u);
    const auto saved = shell.viewOf(id, 0);
    CHECK(saved.has_value()); // same id after the rebase
    if (saved.has_value() && viewBefore.has_value()) CHECK(saved->bounds == viewBefore->bounds);
    CHECK(!session.commands().canUndo()); // the saved file is the new base
    shell.paint();

    shell.statusLog.clear();
    c.applyIntent(selectIntent(id));
    c.applyIntent(mv);
    CHECK(shell.statusLog.empty());
    CHECK(session.isDirty());
    CHECK_EQ(shell.depth(), 1u);
    const core::AnnotationId second = createRect(shell, core::Rect{200.0, 200.0, 40.0, 40.0}, 0);
    CHECK(second);
    CHECK(second != id);
    CHECK_EQ(shell.countOn(0), 2u);
    CHECK(session.undo());
    CHECK(session.undo());
    CHECK(!session.isDirty()); // back at the saved state
    CHECK(shell.viewOf(id, 0).has_value());

    // And the document can be saved again.
    CHECK(session.redo());
    shell.statusLog.clear();
    shell.files->save(*tab);
    CHECK(shell.dispatcher.waitUntil([&] { return shell.hasStatus("Saved"); }));
    CHECK(!session.isDirty());
    CHECK(shell.viewOf(id, 0).has_value());
}

RIVET_TEST(stressQuitWhileASaveWithAnnotationEditsIsInFlightLeavesNothingDangling) {
    FileShell shell;
    DocumentTab* tab = shell.open("quit.pdf", true, 3);
    CHECK(tab != nullptr);
    (void)createRect(shell, core::Rect{100.0, 100.0, 80.0, 40.0}, 0);
    Intent note = intent(IntentKind::CreateNote, 1);
    note.rect = core::Rect{50.0, 50.0, 20.0, 20.0};
    shell.annotations->applyIntent(note);
    shell.annotations->noteArea().setText("uncommitted at quit");
    CHECK(shell.annotations->noteEditorOpen());
    press(*shell.annotations, 0, core::Point{400.0, 400.0}); // a held gesture too

    EngineGateGuard gate{shell.engine};
    shell.engine.closeGate();
    shell.annotations->closeNoteEditor(); // commits the text before the save starts
    shell.files->save(*tab);
    CHECK(shell.engine.waitParked(1));
    CHECK(tab->session()->isEditingLocked());
    shell.statusLog.clear();

    const int posted = shell.dispatcher.posted.load();
    // Quitting destroys the controllers first; FileController's destructor
    // fences its workers (AsyncScope), so it waits for the parked save. Open
    // the gate from another thread while quit is waiting on it.
    std::thread releaser([&shell] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        shell.engine.release();
    });
    shell.quit(); // controllers, then the workspace and its sessions, mid-save
    releaser.join();
    // The worker finished and posted its completion; the dead controller ignores it.
    CHECK(shell.dispatcher.waitPosted(posted + 1));
    shell.dispatcher.pump();
    CHECK(shell.dispatcher.waitUntil([] { return true; }));
    CHECK(shell.statusLog.empty());
    CHECK_EQ(shell.engine.assemblies.load(), 1);
}

// --- Keyboard ownership ------------------------------------------------------------

RIVET_TEST(deleteAndBackspaceWhileCroppingKeepTheSelectedAnnotation) {
    PageShell shell(3);
    CHECK(shell.open("cropkeys") != nullptr);
    const core::AnnotationId id = createRect(shell);
    CHECK(id);
    CHECK_EQ(shell.countOn(0), 1u);

    shell.editing->beginCrop();
    CHECK(shell.editing->isCropping());
    const std::size_t depth = shell.depth();
    for (const ui::Key key : {ui::Key::Delete, ui::Key::Backspace}) {
        const ui::KeyEvent event = keyEvent(key);
        if (!shell.editing->handleToolKey(event)) (void)shell.viewport->onKey(event);
    }
    CHECK_EQ(shell.countOn(0), 1u);
    CHECK(shell.viewOf(id, 0).has_value());
    CHECK_EQ(shell.depth(), depth);

    // Once the crop is cancelled the layer owns the keyboard again.
    CHECK(shell.editing->handleToolKey(keyEvent(ui::Key::Escape)));
    CHECK(!shell.editing->isCropping());
    CHECK(shell.viewport->onKey(keyEvent(ui::Key::Delete)));
    CHECK_EQ(shell.countOn(0), 0u);
}

RIVET_TEST(deleteAfterClickingAnAnnotationDeletesTheAnnotationNotTheSelectedPages) {
    PageShell shell(3);
    CHECK(shell.open("clickfocus") != nullptr);
    const core::AnnotationId id = createRect(shell, core::Rect{100.0, 100.0, 80.0, 40.0}, 0);
    CHECK(id);

    // The thumbnails hold keyboard focus with page 0 selected.
    shell.editing->handleRowClicked(0, ui::PageThumbnailList::ClickGesture::Replace);
    shell.context->setFocus(&shell.sidebar->thumbnails());
    CHECK(shell.focused == &shell.sidebar->thumbnails());

    // Click the annotation in the viewport (press + release).
    const auto pageRect = shell.viewport->pageRectInViewport(0);
    CHECK(pageRect.has_value());
    const double zoom = shell.viewport->zoomFactor();
    const auto view = shell.viewOf(id, 0);
    CHECK(view.has_value());
    // A hollow rectangle is hit on its outline: press on the left edge.
    const core::Point center{pageRect->origin.x + zoom * (view->bounds.origin.x + 1.0),
                             pageRect->origin.y + zoom * (view->bounds.origin.y + view->bounds.size.height / 2.0)};
    ui::PointerEvent down;
    down.type = ui::PointerEventType::Down;
    down.position = center;
    down.button = 1;
    (void)shell.viewport->onMouse(down);
    ui::PointerEvent up = down;
    up.type = ui::PointerEventType::Up;
    (void)shell.viewport->onMouse(up);
    CHECK(shell.focused == nullptr);
    CHECK(shell.annotations->selectedId() == std::optional<core::AnnotationId>{id});

    // Delete is routed the way the shell does: focused widget, else viewport.
    const ui::KeyEvent del = keyEvent(ui::Key::Delete);
    const bool handled = shell.focused != nullptr ? shell.focused->onKey(del) : shell.viewport->onKey(del);
    CHECK(handled);
    CHECK_EQ(shell.session().pageCount(), 3u);
    CHECK_EQ(shell.countOn(0), 0u);
}

RIVET_TEST(pageEditingControllerDestructionUninstallsItsCropTool) {
    PageShell shell(3);
    CHECK(shell.open("croptool") != nullptr);
    shell.editing->beginCrop();
    CHECK(shell.viewport->activeTool() != nullptr);
    shell.editing.reset();
    CHECK(shell.viewport->activeTool() == nullptr);
}
