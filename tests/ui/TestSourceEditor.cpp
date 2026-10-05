// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"
#include "SourceEditorKit.hpp"

#include "RivetTest.h"

#include "core/geometry/Rect.hpp"
#include "ui/SourceEditor.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>

using namespace rivet::ui;
using namespace rivet::ui::testing;
using rivet::core::Rect;

namespace {

// 400x200 editor, 7-point cells (the fake measures 7 per byte), 20-point
// lines, no gutter unless asked: column c starts at x = 6 + 7c, line L at
// y = 20 L.
struct Fixture {
    SourceEditor editor;
    RecordingSink sink;
    FakeTextClipboard clipboard;
    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);

    explicit Fixture(const std::string& text = {}, bool gutter = false) {
        editor.setShowLineNumbers(gutter);
        editor.setFrame(Rect{0.0, 0.0, 400.0, 200.0});
        editor.setSink(&sink);
        editor.setClipboard(&clipboard);
        editor.setClock([this] { return now; });
        editor.setFocused(true);
        editor.setText(text);
        FakePaintContext measure; // measures the 7-point cell
        editor.paint(measure);
    }

    void press(Key k, ModifierFlags m = {}, std::string text = {}) { editor.onKey(key(k, m, std::move(text))); }
    std::string text() const { return editor.buffer().text(); }
    static double x(std::size_t column) { return 6.0 + 7.0 * static_cast<double>(column) + 1.0; }
    static double y(std::size_t line) { return 20.0 * static_cast<double>(line) + 10.0; }
    void click(std::size_t column, std::size_t line, bool shift = false) {
        editor.onMouse(pointer(PointerEventType::Down, x(column), y(line), 1, shift));
        editor.onMouse(pointer(PointerEventType::Up, x(column), y(line), 1, shift));
    }
};

} // namespace

RIVET_TEST(sourceEditorTypesAsciiAndCyrillic) {
    Fixture f;
    typeString(f.editor, "hi привет 😀");
    CHECK_EQ(f.text(), std::string("hi привет 😀"));
    CHECK_EQ(f.editor.caretOffset(), f.text().size());
    CHECK_EQ(f.editor.caretColumn(), std::size_t{3 + 6 + 1 + 2}); // emoji = 2 cells
    // Left moves over whole code points.
    f.press(Key::Left);
    CHECK_EQ(f.editor.caretOffset(), f.text().size() - 4);
    f.press(Key::Left);
    f.press(Key::Left);
    CHECK_EQ(f.editor.caretOffset(), f.text().size() - 4 - 1 - 2);
    CHECK_EQ(f.text().substr(f.editor.caretOffset(), 2), std::string("т"));
    f.press(Key::Right);
    CHECK_EQ(f.text().substr(f.editor.caretOffset(), 1), std::string(" "));
}

RIVET_TEST(sourceEditorNeverProducesInvalidUtf8) {
    Fixture f("яя😀");
    f.press(Key::End);
    f.press(Key::Backspace);
    CHECK_EQ(f.text(), std::string("яя"));
    f.press(Key::Left);
    f.press(Key::Delete);
    CHECK_EQ(f.text(), std::string("я"));
    CHECK(!TextBuffer::needsNormalization(f.text()));
}

RIVET_TEST(sourceEditorShiftArrowsExtendAndCollapse) {
    Fixture f("abcdef");
    f.editor.setCaretOffset(2);
    f.press(Key::Right, mods(true));
    f.press(Key::Right, mods(true));
    CHECK(f.editor.selection() == (SourceEditor::Selection{2, 4}));
    f.press(Key::Right); // collapses to the end of the selection
    CHECK(f.editor.selection() == (SourceEditor::Selection{4, 4}));
    f.press(Key::Left, mods(true));
    f.press(Key::Left, mods(true));
    f.press(Key::Left, mods(true));
    CHECK(f.editor.selection() == (SourceEditor::Selection{1, 4}));
    f.press(Key::Left); // collapses to the beginning
    CHECK_EQ(f.editor.caretOffset(), std::size_t{1});
}

RIVET_TEST(sourceEditorWordAndLineMovement) {
    Fixture f("привет мир, hello_world x\nsecond");
    f.editor.setCaretOffset(0);
    f.press(Key::Right, mods(false, true));
    CHECK_EQ(f.editor.caretOffset(), std::string("привет").size());
    f.press(Key::Right, mods(false, true));
    CHECK_EQ(f.editor.caretOffset(), std::string("привет мир").size());
    f.press(Key::Right, mods(false, true)); // skips ", " then the word
    CHECK_EQ(f.editor.caretOffset(), std::string("привет мир, hello_world").size());
    f.press(Key::Left, mods(false, true));
    CHECK_EQ(f.editor.caretOffset(), std::string("привет мир, ").size());
    // Cmd+Right / Cmd+Left: line end / start.
    f.press(Key::Right, mods(false, false, true));
    CHECK_EQ(f.editor.caretOffset(), f.text().find('\n'));
    f.press(Key::Right, mods(false, true)); // across the line break
    CHECK_EQ(f.editor.caretOffset(), f.text().find('\n') + 1);
    f.press(Key::Left, mods(false, false, true));
    CHECK_EQ(f.editor.caretOffset(), f.text().find('\n') + 1);
    f.press(Key::Down, mods(false, false, true));
    CHECK_EQ(f.editor.caretOffset(), f.text().size());
    f.press(Key::Up, mods(true, false, true));
    CHECK(f.editor.selection() == (SourceEditor::Selection{0, f.text().size()}));
    f.press(Key::Home);
    CHECK_EQ(f.editor.caretOffset(), std::size_t{0});
}

RIVET_TEST(sourceEditorVerticalKeepsColumnAcrossShortLines) {
    Fixture f("abcdefgh\nxy\nabcdefgh");
    f.editor.setCaretOffset(6);
    f.press(Key::Down);
    CHECK_EQ(f.editor.caretOffset(), std::size_t{9 + 2}); // end of "xy"
    f.press(Key::Down);
    CHECK_EQ(f.editor.caretOffset(), std::size_t{9 + 3 + 6}); // column 6 again
    f.press(Key::Down);
    CHECK_EQ(f.editor.caretOffset(), f.text().size());
    f.press(Key::Up);
    f.press(Key::Up);
    f.press(Key::Up);
    CHECK_EQ(f.editor.caretOffset(), std::size_t{0});
}

RIVET_TEST(sourceEditorBackspaceAndDeleteVariants) {
    Fixture f("one two three\nfour");
    f.editor.setCaretOffset(13); // end of line 0
    f.press(Key::Backspace, mods(false, true)); // delete word left
    CHECK_EQ(f.text(), std::string("one two \nfour"));
    f.press(Key::Backspace, mods(false, false, true)); // to line start
    CHECK_EQ(f.text(), std::string("\nfour"));
    f.press(Key::Backspace, mods(false, false, true)); // at line start of line 0: nothing before
    CHECK_EQ(f.text(), std::string("\nfour"));
    f.editor.setCaretOffset(1);
    f.press(Key::Backspace); // joins lines
    CHECK_EQ(f.text(), std::string("four"));
    f.editor.setCaretOffset(0);
    f.press(Key::Delete, mods(false, true));
    CHECK_EQ(f.text(), std::string());
    const std::size_t before = f.sink.edits.size();
    f.press(Key::Delete);
    f.press(Key::Backspace);
    CHECK_EQ(f.sink.edits.size(), before); // keys on empty text are not edits
    Fixture g("abc def");
    g.editor.setCaretOffset(1);
    g.press(Key::Delete, mods(false, false, true)); // to line end
    CHECK_EQ(g.text(), std::string("a"));
    g.editor.setSelection(0, 1);
    g.press(Key::Backspace);
    CHECK_EQ(g.text(), std::string());
}

RIVET_TEST(sourceEditorEnterKeepsIndentation) {
    Fixture f("  - item");
    f.press(Key::End);
    f.press(Key::Enter);
    CHECK_EQ(f.text(), std::string("  - item\n  "));
    CHECK_EQ(f.editor.caretOffset(), f.text().size());
    CHECK(f.sink.edits.back().kind == SourceEditKind::Newline);
    // Enter in the middle of the indentation only copies what is left of the caret.
    Fixture g("\t\tx");
    g.editor.setCaretOffset(1);
    g.press(Key::Enter);
    CHECK_EQ(g.text(), std::string("\t\n\t\tx"));
    // Enter replaces a selection.
    Fixture h("ab cd");
    h.editor.setSelection(2, 3);
    h.press(Key::Enter);
    CHECK_EQ(h.text(), std::string("ab\ncd"));
}

RIVET_TEST(sourceEditorTabInsertsSpacesAndIndentsSelection) {
    Fixture f("a\nb\n\nc");
    f.press(Key::Tab);
    CHECK_EQ(f.text(), std::string("    a\nb\n\nc"));
    f.editor.setSelection(0, f.text().size());
    f.press(Key::Tab);
    CHECK_EQ(f.text(), std::string("        a\n    b\n\n    c")); // empty line stays empty
    CHECK_EQ(f.editor.selection().end, f.text().size());
    f.press(Key::Tab, mods(true));
    CHECK_EQ(f.text(), std::string("    a\nb\n\nc"));
    f.press(Key::Tab, mods(true));
    CHECK_EQ(f.text(), std::string("a\nb\n\nc"));
    const std::size_t editsBefore = f.sink.edits.size();
    f.press(Key::Tab, mods(true)); // nothing left to remove: no edit
    CHECK_EQ(f.sink.edits.size(), editsBefore);
    // Outdent without a selection acts on the caret line; a tab is one level.
    Fixture g("\tx\n  y");
    g.editor.setCaretOffset(2);
    g.press(Key::Tab, mods(true));
    CHECK_EQ(g.text(), std::string("x\n  y"));
}

RIVET_TEST(sourceEditorIndentSelectionExcludesLastLineWhenSelectionEndsAtItsStart) {
    Fixture f("a\nb\nc");
    f.editor.setSelection(0, 4); // "a\nb\n": ends at the start of "c"
    f.press(Key::Tab);
    CHECK_EQ(f.text(), std::string("    a\n    b\nc"));
    CHECK(f.sink.edits.back().kind == SourceEditKind::Indent);
    CHECK_EQ(f.sink.edits.back().offset, std::size_t{0});
    CHECK(f.editor.selection() == (SourceEditor::Selection{4, 12}));
}

RIVET_TEST(sourceEditorCtrlTabIsNotConsumed) {
    Fixture f("x");
    KeyEvent event = key(Key::Tab);
    event.modifiers.control = true;
    CHECK(!f.editor.onKey(event));
    CHECK_EQ(f.text(), std::string("x"));
}

RIVET_TEST(sourceEditorClipboardCopyCutPaste) {
    Fixture f("hello world");
    f.editor.setSelection(0, 5);
    CHECK(f.editor.onKey(chr("c", mods(false, false, true))));
    CHECK_EQ(f.clipboard.content, std::string("hello"));
    CHECK_EQ(f.text(), std::string("hello world"));
    CHECK(f.editor.onKey(chr("x", mods(false, false, true))));
    CHECK_EQ(f.text(), std::string(" world"));
    CHECK(f.sink.edits.back().kind == SourceEditKind::Cut);
    f.editor.setCaretOffset(f.text().size());
    CHECK(f.editor.onKey(chr("v", mods(false, false, true))));
    CHECK_EQ(f.text(), std::string(" worldhello"));
    CHECK(f.sink.edits.back().kind == SourceEditKind::Paste);
    CHECK_EQ(f.editor.caretOffset(), f.text().size());
    f.editor.setSelection(0, 1); // paste over a selection
    f.editor.onKey(chr("v", mods(false, false, true)));
    CHECK_EQ(f.text(), std::string("helloworldhello"));
    f.editor.onKey(chr("a", mods(false, false, true)));
    CHECK(f.editor.selection() == (SourceEditor::Selection{0, f.text().size()}));
}

RIVET_TEST(sourceEditorPasteNormalizesLineEndingsAndUtf8) {
    Fixture f;
    f.clipboard.content = "a\r\nb\rc\xFF" "d";
    f.editor.onKey(chr("v", mods(false, false, true)));
    CHECK_EQ(f.text(), std::string("a\nb\nc\xEF\xBF\xBD" "d"));
    CHECK_EQ(f.sink.edits.back().insert, f.text()); // the sink sees the normalized text
    CHECK_EQ(f.editor.buffer().lineCount(), std::size_t{3});
}

RIVET_TEST(sourceEditorCutKeepsTextWhenClipboardFails) {
    Fixture f("keep");
    f.clipboard.failSet = true;
    f.editor.setSelection(0, 4);
    f.editor.onKey(chr("x", mods(false, false, true)));
    CHECK_EQ(f.text(), std::string("keep"));
}

RIVET_TEST(sourceEditorLeavesShellShortcutsAlone) {
    Fixture f("x");
    CHECK(!f.editor.onKey(chr("z", mods(false, false, true))));
    CHECK(!f.editor.onKey(chr("Z", mods(true, false, true))));
    CHECK(!f.editor.onKey(chr("s", mods(false, false, true))));
    CHECK(!f.editor.onKey(chr("f", mods(false, false, true))));
    CHECK(!f.editor.onKey(chr("1", mods(false, false, true))));
    CHECK(!f.editor.onKey(key(Key::Escape)));
    CHECK_EQ(f.text(), std::string("x"));
    CHECK(f.sink.edits.empty());
    f.editor.setFocused(false);
    CHECK(!f.editor.onKey(chr("a")));
}

RIVET_TEST(sourceEditorRefusedEditChangesNothing) {
    Fixture f("abc");
    f.sink.accept = false;
    f.editor.setCaretOffset(3);
    typeString(f.editor, "x");
    f.press(Key::Backspace);
    CHECK_EQ(f.text(), std::string("abc"));
    CHECK_EQ(f.editor.caretOffset(), std::size_t{3});
}

RIVET_TEST(sourceEditorMouseClickDragAndMultiClick) {
    Fixture f("hello big world\nsecond line here");
    f.click(3, 0);
    CHECK_EQ(f.editor.caretOffset(), std::size_t{3});
    f.now += std::chrono::seconds(2);
    f.click(5, 1, true); // shift-click extends
    CHECK(f.editor.selection() == (SourceEditor::Selection{3, 16 + 5}));
    f.now += std::chrono::seconds(2);

    f.editor.onMouse(pointer(PointerEventType::Down, Fixture::x(1), Fixture::y(0)));
    f.editor.onMouse(pointer(PointerEventType::Move, Fixture::x(4), Fixture::y(0)));
    f.editor.onMouse(pointer(PointerEventType::Up, Fixture::x(4), Fixture::y(0)));
    CHECK(f.editor.selection() == (SourceEditor::Selection{1, 4}));
    f.now += std::chrono::seconds(2);

    f.editor.onMouse(pointer(PointerEventType::Down, Fixture::x(7), Fixture::y(0)));
    f.editor.onMouse(pointer(PointerEventType::Up, Fixture::x(7), Fixture::y(0)));
    f.now += std::chrono::milliseconds(100);
    f.editor.onMouse(pointer(PointerEventType::Down, Fixture::x(7), Fixture::y(0)));
    f.editor.onMouse(pointer(PointerEventType::Up, Fixture::x(7), Fixture::y(0)));
    CHECK(f.editor.selection() == (SourceEditor::Selection{6, 9})); // "big"
    f.now += std::chrono::milliseconds(100);
    f.editor.onMouse(pointer(PointerEventType::Down, Fixture::x(7), Fixture::y(0)));
    f.editor.onMouse(pointer(PointerEventType::Up, Fixture::x(7), Fixture::y(0)));
    CHECK(f.editor.selection() == (SourceEditor::Selection{0, 16})); // whole first line incl. '\n'
    f.now += std::chrono::seconds(2); // a slow click starts over
    f.click(7, 0);
    CHECK(f.editor.selection() == (SourceEditor::Selection{7, 7}));
}

RIVET_TEST(sourceEditorClickPastEndAndInsideWideGlyph) {
    Fixture f("ab\n世界c");
    f.click(40, 0);
    CHECK_EQ(f.editor.caretOffset(), std::size_t{2});
    f.now += std::chrono::seconds(2);
    f.click(2, 1); // right half of the first wide glyph (cells 0-1) -> after it
    CHECK_EQ(f.editor.caretOffset(), std::size_t{3 + 3});
    f.now += std::chrono::seconds(2);
    f.click(0, 99); // below the text: last line
    CHECK_EQ(f.editor.caretLine(), std::size_t{1});
}

RIVET_TEST(sourceEditorDoubleClickWordOnCyrillic) {
    Fixture f("привет, мир");
    f.editor.onMouse(pointer(PointerEventType::Down, Fixture::x(2), Fixture::y(0)));
    f.editor.onMouse(pointer(PointerEventType::Up, Fixture::x(2), Fixture::y(0)));
    f.now += std::chrono::milliseconds(50);
    f.editor.onMouse(pointer(PointerEventType::Down, Fixture::x(2), Fixture::y(0)));
    CHECK(f.editor.selection() == (SourceEditor::Selection{0, std::string("привет").size()}));
}

RIVET_TEST(sourceEditorWheelScrollsAndTopOffsetTracks) {
    std::string text;
    for (int i = 0; i < 200; ++i) text += "line " + std::to_string(i) + "\n";
    Fixture f(text);
    f.editor.setCaretOffset(0);
    CHECK_EQ(f.editor.topVisibleOffset(), std::size_t{0});
    PointerEvent wheel = pointer(PointerEventType::Scroll, 100, 100, 0);
    wheel.scrollDelta = rivet::core::Point{0.0, 200.0}; // 10 lines
    CHECK(f.editor.onMouse(wheel));
    CHECK_NEAR(f.editor.scrollY(), 200.0, 1e-9);
    CHECK_EQ(f.editor.firstVisibleLine(), std::size_t{10});
    CHECK_EQ(f.editor.topVisibleOffset(), f.editor.buffer().lineStart(10));
    wheel.scrollDelta = rivet::core::Point{0.0, -9999.0};
    f.editor.onMouse(wheel);
    CHECK_NEAR(f.editor.scrollY(), 0.0, 1e-9);

    int scrolled = 0;
    f.editor.setOnScrolled([&] { ++scrolled; });
    f.editor.scrollToOffset(f.editor.buffer().lineStart(50) + 3);
    CHECK_EQ(f.editor.firstVisibleLine(), std::size_t{50});
    CHECK_EQ(scrolled, 1);
    f.editor.scrollToOffset(f.text().size() + 100); // clamped to the end
    CHECK(f.editor.scrollY() > 0.0);
}

RIVET_TEST(sourceEditorKeepsCaretVisible) {
    std::string text;
    for (int i = 0; i < 100; ++i) text += "line " + std::to_string(i) + "\n";
    Fixture f(text);
    f.editor.setCaretOffset(0);
    for (int i = 0; i < 30; ++i) f.press(Key::Down);
    CHECK(f.editor.caretLine() == 30);
    CHECK(f.editor.firstVisibleLine() <= 30);
    CHECK(f.editor.lastVisibleLine() >= 30);
    f.press(Key::PageDown);
    CHECK(f.editor.caretLine() > 30);
    CHECK(f.editor.lastVisibleLine() >= f.editor.caretLine());
    f.press(Key::Down, mods(false, false, true));
    CHECK_EQ(f.editor.caretOffset(), f.text().size());
    CHECK(f.editor.lastVisibleLine() == f.editor.buffer().lineCount() - 1);
    f.press(Key::PageUp);
    CHECK(f.editor.caretLine() < f.editor.buffer().lineCount() - 5);
}

RIVET_TEST(sourceEditorHorizontalScrollFollowsCaretOnLongLine) {
    Fixture f(std::string(300, 'x'));
    CHECK_NEAR(f.editor.scrollX(), 0.0, 1e-9);
    f.press(Key::End);
    CHECK(f.editor.scrollX() > 0.0);
    const double caretX = 6.0 + 7.0 * 300.0 - f.editor.scrollX();
    CHECK(caretX >= 0.0 && caretX <= f.editor.textRect().size.width);
    f.press(Key::Home);
    CHECK_NEAR(f.editor.scrollX(), 0.0, 1e-9);
}

RIVET_TEST(sourceEditorCoalescingHints) {
    Fixture f;
    typeString(f.editor, "ab cd");
    // a(new) b(merge) ' '(merge) c(new word after blank) d(merge)
    const auto& e = f.sink.edits;
    CHECK_EQ(e.size(), std::size_t{5});
    CHECK(!e[0].merge);
    CHECK(e[1].merge);
    CHECK(e[2].merge);
    CHECK(!e[3].merge);
    CHECK(e[4].merge);
    f.press(Key::Left); // a caret jump breaks the run
    f.press(Key::Right);
    typeString(f.editor, "e");
    CHECK(!f.sink.edits.back().merge);
    typeString(f.editor, "f");
    CHECK(f.sink.edits.back().merge);
    f.press(Key::Enter); // Enter breaks, and typing after it starts fresh
    CHECK(!f.sink.edits.back().merge);
    typeString(f.editor, "g");
    CHECK(!f.sink.edits.back().merge);
    f.press(Key::Backspace); // backspace runs merge with each other, not with typing
    CHECK(!f.sink.edits.back().merge);
    f.press(Key::Backspace);
    CHECK(f.sink.edits.back().merge);
    f.editor.setCaretOffset(0); // forward-delete run
    f.press(Key::Delete);
    CHECK(!f.sink.edits.back().merge);
    f.press(Key::Delete);
    CHECK(f.sink.edits.back().merge);
    f.clipboard.content = "zz"; // paste never merges and breaks the typing run
    typeString(f.editor, "k");
    f.editor.onKey(chr("v", mods(false, false, true)));
    CHECK(!f.sink.edits.back().merge);
    typeString(f.editor, "m");
    CHECK(!f.sink.edits.back().merge);
    f.editor.setSelection(0, 2); // a selection replacement is its own step
    typeString(f.editor, "q");
    CHECK(!f.sink.edits.back().merge);
    typeString(f.editor, "r");
    CHECK(f.sink.edits.back().merge);
    f.editor.applyExternalEdit(0, 0, "!"); // an external change (undo/redo) breaks the run
    typeString(f.editor, "s");
    CHECK(!f.sink.edits.back().merge);
}

RIVET_TEST(sourceEditorExternalEditRemapsCaretAndSelection) {
    Fixture f("0123456789");
    f.editor.setSelection(8, 3);
    CHECK(f.editor.applyExternalEdit(1, 2, "ab")); // same length: positions unchanged
    CHECK(f.editor.selection() == (SourceEditor::Selection{3, 8}));
    CHECK(f.editor.applyExternalEdit(0, 5, "")); // deletes across the selection start
    CHECK(f.editor.selection() == (SourceEditor::Selection{0, 3}));
    CHECK(!f.editor.applyExternalEdit(100, 0, "x"));
    CHECK(f.sink.edits.empty()); // never reported to the sink
}

RIVET_TEST(sourceEditorPaintCullsToVisibleLines) {
    std::string text;
    for (int i = 0; i < 5000; ++i) text += "line number " + std::to_string(i) + "\n";
    Fixture f(text, true);
    f.editor.scrollToOffset(f.editor.buffer().lineStart(2500));
    FakePaintContext context;
    f.editor.paint(context);
    // ~10 visible lines, each drawn once as text and once as a line number.
    CHECK(context.texts.size() >= 18 && context.texts.size() <= 26);
    bool sawLine2500 = false;
    bool sawFarLine = false;
    for (const auto& t : context.texts) {
        CHECK(t.font.monospace);
        if (t.text == "line number 2500") sawLine2500 = true;
        if (t.text == "line number 100" || t.text == "line number 4900") sawFarLine = true;
    }
    CHECK(sawLine2500);
    CHECK(!sawFarLine);
    CHECK_EQ(context.clipDepth, 0);
}

RIVET_TEST(sourceEditorPaintDrawsSelectionAndGutter) {
    Fixture f("alpha\nbeta", true);
    f.editor.setSelection(2, 8);
    FakePaintContext context;
    f.editor.paint(context);
    bool numberOne = false;
    for (const auto& t : context.texts) numberOne = numberOne || (t.text == "1" && t.align == TextAlign::Right);
    CHECK(numberOne);
    std::size_t selectionFills = 0; // one translucent blue rectangle per touched line
    for (const auto& fill : context.fills) {
        if (fill.color.b > 0.9 && fill.color.a < 0.5 && fill.color.a > 0.0) ++selectionFills;
    }
    CHECK_EQ(selectionFills, std::size_t{2});
}

RIVET_TEST(sourceEditorPaintSlicesLongLinesAndExpandsTabs) {
    Fixture f(std::string(100000, 'x') + "\n\ta");
    FakePaintContext context;
    f.editor.paint(context);
    std::size_t longest = 0;
    bool tabLine = false;
    for (const auto& t : context.texts) {
        longest = std::max(longest, t.text.size());
        if (t.text == "    a") tabLine = true;
    }
    CHECK(longest < 200); // only the visible columns are drawn
    CHECK(tabLine);
}

RIVET_TEST(sourceEditorLargeDocumentEditSmoke) {
    std::string big;
    while (big.size() < (5u << 20)) big += "# Заголовок\n\nTexte avec du texte, 😀 emoji and `code`.\n";
    Fixture f(big);
    f.editor.setCaretOffset(f.editor.buffer().lineStart(f.editor.buffer().lineCount() / 2));
    for (int i = 0; i < 300; ++i) typeString(f.editor, i % 10 == 9 ? "\n" : "я");
    CHECK(f.sink.edits.size() == 300);
    CHECK(!TextBuffer::needsNormalization(f.text()));
}
