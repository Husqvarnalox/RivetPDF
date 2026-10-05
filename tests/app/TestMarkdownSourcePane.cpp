// SPDX-License-Identifier: MPL-2.0
// MarkdownSourcePane: the source editor bound to a MarkdownTabState. Edits
// become TextEditCommands on the tab's stack (one authority), undo/redo come
// back through stateChanged() (the shell routes Cmd+Z to the stack), typing
// runs coalesce into one undo step, saved-checkpoint dirty tracking stays
// exact, and two tabs never mix histories.
#include "Fakes.hpp"
#include "SourceEditorKit.hpp"

#include "RivetTest.h"

#include "app/MarkdownSourcePane.hpp"
#include "app/MarkdownTabState.hpp"
#include "platform/PlatformKit.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace rivet::app;
using namespace rivet::ui;
using namespace rivet::ui::testing;

namespace {

class FakeClipboard final : public rivet::platform::IClipboard {
public:
    std::string content;
    rivet::core::Status setText(const std::string& text) override {
        content = text;
        return rivet::core::ok();
    }
    std::string text() const override { return content; }
};

struct Env {
    FakeClipboard clipboard;
    rivet::platform::ShellServices services;
    std::vector<std::string> statuses;
    Widget* focused = nullptr;
    std::unique_ptr<MarkdownSourcePane> pane;

    Env() {
        services.clipboard = &clipboard;
        MarkdownSourcePaneEnvironment environment;
        environment.services = &services;
        environment.setStatus = [this](std::string s) { statuses.push_back(std::move(s)); };
        environment.setFocus = [this](Widget* w) {
            focused = w;
            if (w != nullptr) w->setFocused(true);
        };
        pane = createMarkdownSourcePane(std::move(environment));
        pane->setFrame(rivet::core::Rect{0.0, 0.0, 400.0, 200.0});
        pane->editor().setShowLineNumbers(false);
        pane->editor().setFocused(true);
    }

    void press(Key k, ModifierFlags m = {}) { pane->editor().onKey(key(k, m)); }
    void type(const std::string& s) { typeString(pane->editor(), s); }
    // What the shell does for Cmd+Z / Cmd+Shift+Z, plus the onChanged forward.
    bool undo(MarkdownTabState& state) {
        const bool ok = state.undo();
        pane->stateChanged();
        return ok;
    }
    bool redo(MarkdownTabState& state) {
        const bool ok = state.redo();
        pane->stateChanged();
        return ok;
    }
    std::string mirror() const { return pane->editor().buffer().text(); }
};

std::unique_ptr<MarkdownTabState> makeState(const std::string& text = {}) {
    DecodedText decoded;
    decoded.text = text;
    return std::make_unique<MarkdownTabState>("doc.md", std::move(decoded));
}

} // namespace

RIVET_TEST(sourcePaneEditsBecomeCommandsAndBumpRevision) {
    Env env;
    auto state = makeState("hello");
    int changes = 0;
    state->setOnChanged([&] { ++changes; env.pane->stateChanged(); });
    env.pane->bind(state.get());
    CHECK_EQ(env.mirror(), std::string("hello"));
    CHECK(!state->isDirty());

    env.press(Key::End);
    const auto revision = state->revision();
    env.type("!");
    CHECK_EQ(state->source(), std::string("hello!"));
    CHECK_EQ(env.mirror(), state->source());
    CHECK(state->revision() > revision);
    CHECK(state->isDirty());
    CHECK(state->canUndo());
    CHECK(changes > 0);
}

RIVET_TEST(sourcePaneTypingRunIsOneUndoStep) {
    Env env;
    auto state = makeState();
    env.pane->bind(state.get());
    env.type("hello");
    CHECK_EQ(state->source(), std::string("hello"));
    CHECK_EQ(state->commands().depth(), std::size_t{1});
    CHECK(env.undo(*state));
    CHECK_EQ(state->source(), std::string());
    CHECK_EQ(env.mirror(), std::string());
    CHECK(!state->isDirty());
    CHECK(env.redo(*state));
    CHECK_EQ(state->source(), std::string("hello"));
    CHECK_EQ(env.mirror(), std::string("hello"));
    CHECK_EQ(env.pane->editor().caretOffset(), std::size_t{5}); // caret after the redone text
}

RIVET_TEST(sourcePaneWordBoundaryAndNewlineSplitUndoSteps) {
    Env env;
    auto state = makeState();
    env.pane->bind(state.get());
    env.type("ab cd");
    CHECK_EQ(state->commands().depth(), std::size_t{2}); // "ab " | "cd"
    env.press(Key::Enter);
    env.type("ef");
    CHECK_EQ(state->commands().depth(), std::size_t{4});
    CHECK(env.undo(*state));
    CHECK_EQ(state->source(), std::string("ab cd\n"));
    CHECK_EQ(env.pane->editor().caretOffset(), std::size_t{6});
    CHECK(env.undo(*state)); // the newline
    CHECK_EQ(state->source(), std::string("ab cd"));
    CHECK(env.undo(*state));
    CHECK_EQ(state->source(), std::string("ab "));
    CHECK_EQ(env.pane->editor().caretOffset(), std::size_t{3});
    CHECK(env.undo(*state));
    CHECK_EQ(state->source(), std::string());
    CHECK(!env.undo(*state));
}

RIVET_TEST(sourcePaneCaretJumpBreaksTheRun) {
    Env env;
    auto state = makeState("0123456789");
    env.pane->bind(state.get());
    env.pane->editor().setCaretOffset(2);
    env.type("ab");
    env.pane->editor().setCaretOffset(8);
    env.type("cd");
    CHECK_EQ(state->commands().depth(), std::size_t{2});
    env.undo(*state);
    CHECK_EQ(state->source(), std::string("01ab23456789"));
    env.undo(*state);
    CHECK_EQ(state->source(), std::string("0123456789"));
}

RIVET_TEST(sourcePaneBackspaceAndDeleteRunsMerge) {
    Env env;
    auto state = makeState("abcdefgh");
    env.pane->bind(state.get());
    env.pane->editor().setCaretOffset(5);
    env.press(Key::Backspace);
    env.press(Key::Backspace);
    env.press(Key::Backspace);
    CHECK_EQ(state->source(), std::string("abfgh"));
    CHECK_EQ(state->commands().depth(), std::size_t{1});
    env.press(Key::Delete);
    env.press(Key::Delete);
    CHECK_EQ(state->source(), std::string("abh"));
    CHECK_EQ(state->commands().depth(), std::size_t{2});
    env.undo(*state);
    CHECK_EQ(state->source(), std::string("abfgh"));
    CHECK_EQ(env.mirror(), std::string("abfgh"));
    env.undo(*state);
    CHECK_EQ(state->source(), std::string("abcdefgh"));
    CHECK_EQ(env.mirror(), std::string("abcdefgh"));
    env.redo(*state);
    env.redo(*state);
    CHECK_EQ(state->source(), std::string("abh"));
    CHECK_EQ(env.mirror(), std::string("abh"));
}

RIVET_TEST(sourcePasteIsItsOwnStep) {
    Env env;
    auto state = makeState();
    env.pane->bind(state.get());
    env.type("a");
    env.clipboard.content = "x\r\ny";
    env.pane->editor().onKey(chr("v", mods(false, false, true)));
    env.type("b");
    CHECK_EQ(state->source(), std::string("ax\nyb"));
    CHECK_EQ(state->commands().depth(), std::size_t{3});
    env.undo(*state);
    CHECK_EQ(state->source(), std::string("ax\ny"));
    env.undo(*state);
    CHECK_EQ(state->source(), std::string("a"));
}

RIVET_TEST(sourcePaneDirtyFollowsSavedCheckpointAcrossUndo) {
    Env env;
    auto state = makeState("x");
    env.pane->bind(state.get());
    env.press(Key::End);
    env.type("a");
    state->markSaved();
    CHECK(!state->isDirty());
    // Typing right after a save must NOT merge into the saved step (the state
    // id would stay equal and the tab would look clean).
    env.type("b");
    CHECK(state->isDirty());
    CHECK_EQ(state->commands().depth(), std::size_t{2});
    CHECK(env.undo(*state));
    CHECK_EQ(state->source(), std::string("xa"));
    CHECK(!state->isDirty()); // back at the saved state
    env.undo(*state);
    CHECK(state->isDirty());
    env.redo(*state);
    CHECK(!state->isDirty());
    // Edit after undo-to-saved: new step, dirty, never falsely clean.
    env.type("c");
    CHECK(state->isDirty());
    env.undo(*state);
    CHECK(!state->isDirty());
}

RIVET_TEST(sourcePaneUndoRedoRestoreCaretAndKeepUtf8Valid) {
    Env env;
    auto state = makeState("привет мир");
    env.pane->bind(state.get());
    env.pane->editor().setSelection(0, std::string("привет").size());
    env.type("Ы"); // replaces the selection
    CHECK_EQ(state->source(), std::string("Ы мир"));
    env.undo(*state);
    CHECK_EQ(state->source(), std::string("привет мир"));
    CHECK_EQ(env.mirror(), state->source());
    CHECK_EQ(env.pane->editor().caretOffset(), std::string("привет").size());
    env.redo(*state);
    CHECK_EQ(env.pane->editor().caretOffset(), std::string("Ы").size());
    CHECK(!TextBuffer::needsNormalization(env.mirror()));
}

RIVET_TEST(sourcePaneExternalCommandResyncsMirror) {
    Env env;
    auto state = makeState("яблоко");
    env.pane->bind(state.get());
    // Same-lead-byte replacement (я -> ю) must not split a code point in the diff.
    CHECK(state->execute(std::make_unique<TextEditCommand>(*state, 0, 2, "ю")));
    env.pane->stateChanged();
    CHECK_EQ(env.mirror(), std::string("юблоко"));
    CHECK(!TextBuffer::needsNormalization(env.mirror()));
    CHECK(state->execute(std::make_unique<TextEditCommand>(*state, state->source().size(), 0, "\nnew")));
    env.pane->stateChanged();
    CHECK_EQ(env.mirror(), state->source());
    CHECK_EQ(env.pane->editor().buffer().lineCount(), std::size_t{2});
    // Typing after an external change starts a fresh step.
    env.pane->editor().setCaretOffset(0);
    env.type("1");
    CHECK_EQ(state->commands().depth(), std::size_t{3});
}

RIVET_TEST(sourcePaneEditorDoesNotConsumeUndoShortcuts) {
    Env env;
    auto state = makeState("x");
    env.pane->bind(state.get());
    CHECK(!env.pane->editor().onKey(chr("z", mods(false, false, true))));
    CHECK(!env.pane->editor().onKey(chr("Z", mods(true, false, true))));
    CHECK_EQ(state->commands().depth(), std::size_t{0});
}

RIVET_TEST(sourcePaneEditingLockedRefusesAndReports) {
    Env env;
    auto state = makeState("abc");
    env.pane->bind(state.get());
    state->setEditingLocked(true);
    env.press(Key::End);
    env.type("x");
    env.press(Key::Backspace);
    CHECK_EQ(state->source(), std::string("abc"));
    CHECK_EQ(env.mirror(), std::string("abc"));
    CHECK(!env.statuses.empty());
    state->setEditingLocked(false);
    env.type("x");
    CHECK_EQ(state->source(), std::string("abcx"));
}

RIVET_TEST(sourcePaneTabSwitchingKeepsHistoriesApart) {
    Env env;
    auto a = makeState("AAA");
    auto b = makeState("BBB");
    env.pane->bind(a.get());
    env.press(Key::End);
    env.type("1");
    env.pane->bind(b.get());
    CHECK_EQ(env.mirror(), std::string("BBB"));
    CHECK_EQ(env.pane->editor().caretOffset(), std::size_t{0});
    env.press(Key::End);
    env.type("2");
    CHECK_EQ(b->source(), std::string("BBB2"));
    CHECK_EQ(a->source(), std::string("AAA1"));
    CHECK_EQ(a->commands().depth(), std::size_t{1});
    CHECK_EQ(b->commands().depth(), std::size_t{1});

    // Back to A: its text and caret are restored; typing must not merge into B's step.
    env.pane->bind(a.get());
    CHECK_EQ(env.mirror(), std::string("AAA1"));
    CHECK_EQ(env.pane->editor().caretOffset(), std::size_t{4});
    env.type("3");
    CHECK_EQ(a->source(), std::string("AAA13"));
    CHECK_EQ(a->commands().depth(), std::size_t{2}); // no merge memory across bind
    CHECK_EQ(b->commands().depth(), std::size_t{1});
    env.undo(*b); // b is not bound: the shell would not do this, but it must not corrupt A
    env.pane->bind(nullptr);
    CHECK_EQ(env.mirror(), std::string());
    env.pane->bind(b.get());
    CHECK_EQ(env.mirror(), std::string("BBB"));
    env.pane->bind(nullptr);
    env.pane->editor().setFocused(true);
    env.type("z"); // unbound: refused, nothing crashes
}

RIVET_TEST(sourcePaneLargeDocumentTypingStaysCorrect) {
    Env env;
    std::string big;
    while (big.size() < (5u << 20)) big += "Строка текста for the large document test 😀\n";
    auto state = makeState(big);
    env.pane->bind(state.get());
    env.pane->editor().setCaretOffset(env.pane->editor().buffer().lineStart(env.pane->editor().buffer().lineCount() / 2));
    for (int i = 0; i < 200; ++i) env.type(i % 20 == 19 ? "\n" : "ы");
    CHECK_EQ(env.mirror().size(), state->source().size());
    CHECK(env.mirror() == state->source());
    CHECK(env.undo(*state));
    CHECK(env.mirror() == state->source());
    CHECK(state->commands().depth() < 30); // runs are coalesced
}

RIVET_TEST(sourcePaneTryMergeShapes) {
    auto state = makeState("0123456789");
    auto command = std::make_unique<TextEditCommand>(*state, 5, 0, "ab");
    TextEditCommand* raw = command.get();
    CHECK(state->execute(std::move(command)));
    CHECK_EQ(state->source(), std::string("01234ab56789"));
    CHECK(raw->tryMerge(7, 0, "c"));      // after
    CHECK(raw->tryMerge(6, 1, ""));       // inside (removes "b")
    CHECK(raw->tryMerge(4, 1, ""));       // before (removes "4")
    CHECK(raw->tryMerge(6, 2, "Z"));      // after the (now shorter) insert: forward delete "56"
    CHECK(!raw->tryMerge(0, 1, "q"));     // far away: refused, state unchanged
    CHECK(!raw->tryMerge(999, 0, "q"));
    CHECK_EQ(state->source(), std::string("0123acZ789"));
    CHECK(state->undo());
    CHECK_EQ(state->source(), std::string("0123456789"));
    CHECK(state->redo());
    CHECK_EQ(state->source(), std::string("0123acZ789"));
}
