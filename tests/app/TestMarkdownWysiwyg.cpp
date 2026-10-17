// SPDX-License-Identifier: MPL-2.0
// WYSIWYG editing in the Rendered view (ADR-0031), driven through the real
// Markdown host with the fake paint context: keys and clicks go into the
// preview, every edit lands in the tab's source through its command stack.
#include "RivetTest.h"

#include "Fakes.hpp"
#include "SourceEditorKit.hpp"
#include "app/MarkdownHostView.hpp"
#include "app/MarkdownPreviewView.hpp"
#include "app/MarkdownSourcePane.hpp"
#include "app/MarkdownTabState.hpp"
#include "app/MenuModel.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "platform/PlatformKit.hpp"

#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <utility>
#include <vector>

using namespace rivet;
using namespace rivet::app;
using ui::testing::chr;
using ui::testing::key;
using ui::testing::mods;

namespace {

class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    bool pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
        return !run.empty();
    }
    // Main-thread timers: recorded, fired only by the test.
    bool postDelayed(std::chrono::milliseconds delay, std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        delayed.emplace_back(delay, std::move(task));
        return true;
    }
    std::deque<std::pair<std::chrono::milliseconds, std::function<void()>>> delayed;

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

struct Clipboard final : platform::IClipboard {
    std::string value;
    core::Status setText(const std::string& t) override {
        value = t;
        return {};
    }
    std::string text() const override { return value; }
};

struct Fixture {
    QueueDispatcher dispatcher;
    std::deque<std::function<void()>> jobs; // background parses (run on demand)
    Clipboard clipboard;
    platform::ShellServices services;
    std::vector<std::string> statuses;
    ui::Widget* focused = nullptr;
    ui::testing::CountingRedrawSink sink;
    std::unique_ptr<MarkdownHostView> host;
    std::unique_ptr<MarkdownTabState> state;

    explicit Fixture(const std::string& text, MarkdownDisplayMode mode = MarkdownDisplayMode::Rendered,
                     std::size_t syncBytes = 64 * 1024) {
        services.mainDispatcher = &dispatcher;
        services.clipboard = &clipboard;
        MarkdownHostEnvironment env;
        env.dispatcher = &dispatcher;
        env.services = &services;
        env.setStatus = [this](std::string s) { statuses.push_back(std::move(s)); };
        env.setFocus = [this](ui::Widget* w) {
            if (focused != nullptr) focused->setFocused(false);
            focused = w;
            if (w != nullptr) w->setFocused(true);
        };
        env.parseTuning.debounce = std::chrono::milliseconds(0);
        env.parseTuning.synchronousParseBytes = syncBytes;
        env.runBackground = [this](std::function<void()> job) { jobs.push_back(std::move(job)); };
        host = createMarkdownHostView(std::move(env));
        host->setRedrawSink(&sink);
        host->setFrame(core::Rect{0.0, 0.0, 801.0, 400.0});
        DecodedText decoded;
        decoded.text = text;
        state = std::make_unique<MarkdownTabState>("doc.md", std::move(decoded));
        state->setMode(mode);
        state->setOnChanged([this] { host->stateChanged(); });
        host->bind(state.get());
        drain();
        paint();
        if (mode != MarkdownDisplayMode::Source) focus(*preview());
    }
    ~Fixture() {
        host->bind(nullptr);
        state->setOnChanged({});
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    MarkdownPreviewView* preview() { return host->previewView(); }
    ui::SourceEditor& editor() { return host->sourcePane()->editor(); }
    const std::string& src() const { return state->source(); }
    void focus(ui::Widget& w) {
        if (focused != nullptr) focused->setFocused(false);
        focused = &w;
        w.setFocused(true);
    }
    void paint() {
        ui::testing::FakePaintContext ctx;
        host->paint(ctx);
    }
    void drain() {
        for (;;) {
            bool any = dispatcher.pump();
            while (!jobs.empty()) {
                auto job = std::move(jobs.front());
                jobs.pop_front();
                job();
                any = true;
            }
            if (!any) break;
        }
    }
    // A key as the shell delivers it to the focused preview, then a frame.
    void press(const ui::KeyEvent& event) {
        preview()->onKey(event);
        paint();
    }
    void type(const std::string& text) {
        for (std::size_t i = 0; i < text.size();) {
            std::size_t next = i + 1;
            while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
            const std::string piece = text.substr(i, next - i);
            if (piece == " ") press(key(ui::Key::Space, {}, " "));
            else if (piece == "\n") press(key(ui::Key::Enter));
            else press(chr(piece));
            i = next;
        }
    }
    void caretAt(std::size_t offset) { preview()->setCaretFromSource(offset, offset, false); }
    std::size_t caret() { return preview()->caretOffset(); }
};

bool same(const std::string& actual, const std::string& expected) {
    if (actual == expected) return true;
    std::fprintf(stderr, "    expected: [%s]\n    actual:   [%s]\n", expected.c_str(), actual.c_str());
    return false;
}

ui::ModifierFlags primary() {
    ui::ModifierFlags m;
    m.command = true;
    return m;
}

} // namespace

RIVET_TEST(wysiwygTypingEditsTheSourceAndUndoIsShared) {
    Fixture f("Hello world\n");
    CHECK(f.preview()->editable());
    f.caretAt(5);
    f.type(", dear");
    CHECK(same(f.src(), "Hello, dear world\n"));
    CHECK_EQ(f.caret(), 11u);
    // Two undo steps: ", " and "dear" (a word typed after a space starts one).
    CHECK(f.state->undo());
    CHECK(same(f.src(), "Hello,  world\n") || same(f.src(), "Hello, world\n"));
    CHECK(f.state->undo());
    CHECK(same(f.src(), "Hello world\n"));
    CHECK(!f.state->canUndo());
    CHECK(f.state->redo());
    f.paint();
    // The caret follows undo/redo to the end of the change.
    CHECK_EQ(f.caret(), 7u);
}

RIVET_TEST(wysiwygTypingEscapesMarkupAndHandlesUnicode) {
    Fixture f("x\n");
    f.caretAt(1);
    f.type(" *not em* # [x] Привет 👍🏽");
    CHECK(same(f.src(), "x \\*not em\\* # \\[x\\] Привет 👍🏽\n"));
    const markdown::MarkdownDocument* doc = f.preview()->document();
    CHECK(doc != nullptr && doc->blocks.size() == 1 && doc->blocks[0].inlines.size() == 1);
    // Backspace removes the emoji with its skin tone in one step.
    f.press(key(ui::Key::Backspace));
    CHECK(same(f.src(), "x \\*not em\\* # \\[x\\] Привет \n"));
    f.press(key(ui::Key::Backspace));
    f.press(key(ui::Key::Backspace));
    CHECK(same(f.src(), "x \\*not em\\* # \\[x\\] Приве\n"));
}

RIVET_TEST(wysiwygBackspaceAcrossBlocksAndEnterInLists) {
    Fixture f("- one\n- two\n\npara\n");
    f.caretAt(8); // "two"
    f.press(key(ui::Key::Backspace));
    CHECK(same(f.src(), "- onetwo\n\npara\n"));
    f.caretAt(5); // after "one"
    f.press(key(ui::Key::Enter));
    CHECK(same(f.src(), "- one\n- two\n\npara\n"));
    f.press(key(ui::Key::End));
    f.press(key(ui::Key::Enter));
    CHECK(same(f.src(), "- one\n- two\n- \n\npara\n"));
    f.press(key(ui::Key::Enter)); // empty item: leave the list
    f.type("after");
    const markdown::MarkdownDocument* doc = f.preview()->document();
    CHECK(doc != nullptr);
    CHECK_EQ(doc->blocks.size(), 3u);
    CHECK(doc->blocks[1].kind == markdown::BlockKind::Paragraph);
    CHECK(doc->blocks[1].inlines.at(0).text == "after");
}

RIVET_TEST(wysiwygArrowsHomeEndAndVerticalMovement) {
    Fixture f("first line\n\nsecond **bold** line\n\n| a |\n|---|\n| b |\n\nlast\n");
    f.caretAt(0);
    f.press(key(ui::Key::End));
    CHECK_EQ(f.caret(), 10u);
    f.press(key(ui::Key::Right)); // to the next block
    CHECK_EQ(f.caret(), 12u);
    f.press(key(ui::Key::Right, mods(false, true)));
    CHECK_EQ(f.caret(), 18u); // word right
    f.press(key(ui::Key::Right)); // over the space; "after space" = "before b"
    CHECK_EQ(f.caret(), 19u);
    f.press(key(ui::Key::Right)); // the hidden "**" is skipped
    CHECK_EQ(f.caret(), 22u);
    f.press(key(ui::Key::Left, mods(true)));
    CHECK_EQ(f.preview()->anchorOffset(), 22u);
    CHECK_EQ(f.caret(), 19u);
    f.press(key(ui::Key::Home));
    CHECK_EQ(f.caret(), 12u);
    // Down skips the table (a read-only island) and lands on "last".
    f.press(key(ui::Key::Down));
    const std::size_t last = f.src().find("last");
    CHECK_GE(f.caret(), last);
    CHECK_LE(f.caret(), last + 4);
    f.press(key(ui::Key::Up, primary()));
    CHECK_EQ(f.caret(), 0u);
    f.press(key(ui::Key::Down, primary()));
    CHECK_EQ(f.caret(), f.src().size() - 1);
}

RIVET_TEST(wysiwygClickDragDoubleClickAndCopyCutPaste) {
    Fixture f("alpha beta gamma\n");
    const markdown::MarkdownLayout* layout = f.preview()->currentLayout();
    CHECK(layout != nullptr && !layout->runs.empty());
    const markdown::LayoutRun& run = layout->runs[0];
    const double y = run.baseline - 2.0 - f.preview()->scrollY();
    const double xBeta = run.x + 6.0 * 7.0; // "alpha " is 6 bytes at 7 px
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Down, xBeta, y));
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Up, xBeta, y));
    CHECK_EQ(f.caret(), 6u);
    // Drag to the end of "beta".
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Down, xBeta + 100.0, y)); // far: new click
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Up, xBeta + 100.0, y));
    // Double click selects the word.
    const double xGamma = run.x + 13.0 * 7.0;
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Down, xGamma, y));
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Up, xGamma, y));
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Down, xGamma, y));
    f.preview()->onMouse(ui::testing::pointer(ui::PointerEventType::Up, xGamma, y));
    CHECK_EQ(f.preview()->anchorOffset(), 11u);
    CHECK_EQ(f.caret(), 16u);
    f.press(chr("c", primary()));
    CHECK(same(f.clipboard.value, "gamma"));
    f.press(chr("x", primary()));
    CHECK(same(f.src(), "alpha beta \n"));
    f.clipboard.value = "*star*\nsecond";
    f.press(chr("v", primary()));
    CHECK(same(f.src(), "alpha beta \\*star\\*\n\nsecond\n"));
    // Select all + type replaces everything.
    f.press(chr("a", primary()));
    f.type("new");
    CHECK(same(f.src(), "new"));
}

RIVET_TEST(wysiwygImageBlocksAreSelectableAndDeletable) {
    Fixture f("a\n\n![alt](missing.png)\n\nb\n");
    f.caretAt(f.src().find("b\n"));
    f.press(key(ui::Key::Backspace)); // selects the image instead of joining
    CHECK_EQ(f.preview()->anchorOffset(), 3u);
    CHECK_EQ(f.caret(), 3u + std::string("![alt](missing.png)").size());
    f.press(key(ui::Key::Backspace));
    const markdown::MarkdownDocument* doc = f.preview()->document();
    CHECK(doc != nullptr && doc->blocks.size() == 2);
}

RIVET_TEST(wysiwygEditingIsLockedDuringASave) {
    Fixture f("text\n");
    f.caretAt(4);
    f.state->setEditingLocked(true);
    f.type("x");
    CHECK(same(f.src(), "text\n"));
    CHECK(!f.statuses.empty());
    CHECK(!f.host->applyFormat(MarkdownFormatCommand::Bold));
    f.state->setEditingLocked(false);
    f.type("x");
    CHECK(same(f.src(), "textx\n"));
}

RIVET_TEST(wysiwygFormatCommandsToggleInRenderedAndSourceViews) {
    Fixture f("make this bold\n");
    // Rendered: select "this" (shift+arrows), Bold twice = the original.
    f.caretAt(5);
    for (int i = 0; i < 4; ++i) f.press(key(ui::Key::Right, mods(true)));
    CHECK(f.host->applyFormat(MarkdownFormatCommand::Bold));
    f.paint();
    CHECK(same(f.src(), "make **this** bold\n"));
    CHECK(f.host->applyFormat(MarkdownFormatCommand::Bold));
    f.paint();
    CHECK(same(f.src(), "make this bold\n"));
    CHECK(f.host->applyFormat(MarkdownFormatCommand::Heading2));
    f.paint();
    CHECK(same(f.src(), "## make this bold\n"));
    // One undo step per command.
    CHECK(f.state->undo());
    CHECK(same(f.src(), "make this bold\n"));
    // Source view: the editor's selection is wrapped the same way.
    f.state->setMode(MarkdownDisplayMode::Source);
    f.paint();
    f.editor().setSelection(10, 14);
    CHECK(f.host->applyFormat(MarkdownFormatCommand::Italic));
    CHECK(same(f.src(), "make this *bold*\n"));
    CHECK_EQ(f.editor().selection().begin, 11u);
    CHECK(f.host->applyFormat(MarkdownFormatCommand::Italic));
    CHECK(same(f.src(), "make this bold\n"));
    CHECK(f.host->applyFormat(MarkdownFormatCommand::BulletList));
    CHECK(same(f.src(), "- make this bold\n"));
}

RIVET_TEST(wysiwygFormatMenuIsPortableAndEnabledForMarkdownOnly) {
    bool found = false;
    for (const Menu& menu : buildMenuBar()) {
        if (menu.title != "Format") continue;
        found = true;
        bool bold = false;
        for (const MenuItem& item : menu.items) {
            CHECK(item.command.kind == AppCommand::Kind::MarkdownFormat);
            if (item.command.markdownFormat() == MarkdownFormatCommand::Bold) {
                bold = item.shortcut.has_value() && item.shortcut->key == "b" && item.shortcut->primary;
            }
        }
        CHECK(bold);
        CHECK_EQ(menu.items.size(), 16u);
    }
    CHECK(found);
}

RIVET_TEST(wysiwygSplitModeKeepsOneCaret) {
    Fixture f("alpha beta\n", MarkdownDisplayMode::Split);
    f.caretAt(0);
    f.press(key(ui::Key::Right, mods(false, true)));
    CHECK_EQ(f.caret(), 5u);
    CHECK_EQ(f.editor().caretOffset(), 5u); // the Source pane follows
    // Typing in the preview shows in the editor's mirror with the same caret.
    f.type("!");
    CHECK(same(f.editor().buffer().text(), "alpha! beta\n"));
    CHECK_EQ(f.editor().caretOffset(), 6u);
    // And the other way round: the editor's caret moves the preview's.
    f.focus(f.editor());
    f.editor().onKey(key(ui::Key::End));
    CHECK_EQ(f.caret(), 11u);
}

RIVET_TEST(wysiwygKeysWaitForTheLayoutOfTheCurrentSource) {
    Fixture f("abc\n\ndef\n");
    f.caretAt(3);
    f.preview()->onKey(key(ui::Key::Enter)); // re-parsed synchronously; layout pending until paint
    f.preview()->onKey(key(ui::Key::Down));   // needs the new layout: queued
    CHECK_EQ(f.preview()->pendingInputCount(), 1u);
    f.preview()->onKey(chr("x")); // queued behind it, order kept
    f.paint();
    CHECK_EQ(f.preview()->pendingInputCount(), 0u);
    CHECK(same(f.src(), "abc\n\n\n\nxdef\n"));
}

RIVET_TEST(wysiwygTypingIntoALargeDocumentIsOptimisticWhileItParses) {
    std::string text = "start\n\n";
    while (text.size() < 70 * 1024) text += "Some filler paragraph text for the background parse.\n\n";
    Fixture f(text, MarkdownDisplayMode::Rendered, 1024); // parse in the background
    f.caretAt(5);
    f.type("XY");
    // The source changed at once; the preview still shows the old snapshot...
    CHECK(f.src().starts_with("startXY\n"));
    CHECK(f.preview()->revision() != f.state->revision());
    CHECK(f.preview()->caretRect().has_value());
    // ...until the parse lands.
    f.drain();
    f.paint();
    CHECK(f.preview()->revision() == f.state->revision());
    CHECK_EQ(f.caret(), 7u);
    // Undo is still one step for the burst.
    CHECK(f.state->undo());
    CHECK(f.src().starts_with("start\n"));
}

RIVET_TEST(wysiwygCaretIsDrawnAndBlinksOnlyWhenFocused) {
    Fixture f("text\n");
    f.caretAt(2);
    const std::optional<core::Rect> r = f.preview()->caretRect();
    CHECK(r.has_value());
    ui::testing::FakePaintContext ctx;
    f.host->paint(ctx);
    bool caretDrawn = false;
    for (const auto& fill : ctx.fills) {
        if (std::abs(fill.rect.size.width - 1.5) < 0.01) caretDrawn = true;
    }
    CHECK(caretDrawn);
    f.focus(f.editor());
    ui::testing::FakePaintContext ctx2;
    f.host->paint(ctx2);
    for (const auto& fill : ctx2.fills) CHECK(std::abs(fill.rect.size.width - 1.5) > 0.01);
}

// QA bug 15: the caret blink is a main-thread timer (postDelayed), not a
// worker sleeping on the pool.
RIVET_TEST(wysiwygCaretBlinkUsesMainThreadTimers) {
    Fixture f("Hello\n");
    MarkdownPreviewView::Tuning tuning;
    tuning.caretBlink = std::chrono::milliseconds(1);
    f.preview()->setTuning(tuning);
    f.dispatcher.delayed.clear();
    f.type("x");
    CHECK(f.dispatcher.delayed.size() == 1);
    if (f.dispatcher.delayed.empty()) return;
    CHECK(f.dispatcher.delayed.front().first == std::chrono::milliseconds(1));
    CHECK(f.preview()->caretBlinkOn());
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto tick = std::move(f.dispatcher.delayed.front().second);
    f.dispatcher.delayed.pop_front();
    tick();
    CHECK(!f.preview()->caretBlinkOn()); // toggled
    CHECK(f.dispatcher.delayed.size() == 1); // and re-armed
}
