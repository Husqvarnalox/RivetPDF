// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "ui/TextArea.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::ui::Key;
using rivet::ui::KeyEvent;
using rivet::ui::ModifierFlags;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::TextArea;
using rivet::ui::testing::FakePaintContext;

namespace {

// The fake measures 7 points per UTF-8 byte, so a 82-point wide area has a
// 70-point text width: 10 ASCII characters (or 5 Cyrillic ones) per line.
constexpr double kWidth = 82.0;
constexpr double kHeight = 66.0; // three 18-point lines of view height

KeyEvent makeKey(Key key, std::string text = {}, ModifierFlags modifiers = {}) {
    KeyEvent event;
    event.key = key;
    event.text = std::move(text);
    event.modifiers = modifiers;
    return event;
}

ModifierFlags shiftMods() {
    ModifierFlags modifiers;
    modifiers.shift = true;
    return modifiers;
}

ModifierFlags commandMods(bool shift = false) {
    ModifierFlags modifiers;
    modifiers.command = true;
    modifiers.shift = shift;
    return modifiers;
}

PointerEvent makePointer(PointerEventType type, double x, double y, bool shift = false) {
    PointerEvent event;
    event.type = type;
    event.position = Point{x, y};
    event.button = 1;
    event.modifiers.shift = shift;
    return event;
}

// A focused, framed area that has been painted once (glyph widths measured).
std::unique_ptr<TextArea> makeArea(const std::string& text = {}, double width = kWidth,
                                   double height = kHeight) {
    auto area = std::make_unique<TextArea>("Type here");
    area->setFrame(Rect{0.0, 0.0, width, height});
    area->setFocused(true);
    area->setText(text);
    FakePaintContext context;
    area->paint(context);
    return area;
}

void type(TextArea& area, const std::string& text) {
    for (std::size_t at = 0; at < text.size();) {
        std::size_t next = at + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
        CHECK_EQ(area.onKey(makeKey(Key::Character, text.substr(at, next - at))), true);
        at = next;
    }
}

} // namespace

RIVET_TEST(textAreaTypesAndDeletesMultiByteTextByCodePoint) {
    auto area = makeArea();
    int changes = 0;
    std::string last;
    area->setOnTextChanged([&](const std::string& text) {
        ++changes;
        last = text;
    });
    type(*area, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"); // "Привет"
    CHECK_EQ(area->text(), std::string("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(12));
    CHECK_EQ(changes, 6);

    CHECK_EQ(area->onKey(makeKey(Key::Backspace)), true);
    CHECK_EQ(area->text().size(), static_cast<std::size_t>(10));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(10));
    CHECK_EQ(last, area->text());

    // Left twice, Delete removes the whole code point ahead.
    area->onKey(makeKey(Key::Left));
    area->onKey(makeKey(Key::Left));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(6));
    area->onKey(makeKey(Key::Delete));
    CHECK_EQ(area->text(), std::string("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB5")); // "Прие"
    CHECK_EQ(area->caret(), static_cast<std::size_t>(6));

    // 4-byte code point (emoji) is one step.
    type(*area, "\xF0\x9F\x98\x80");
    CHECK_EQ(area->caret(), static_cast<std::size_t>(10));
    area->onKey(makeKey(Key::Left));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(6));
}

RIVET_TEST(textAreaEnterInsertsNewlineAndBackspaceJoinsLines) {
    auto area = makeArea();
    type(*area, "ab");
    CHECK_EQ(area->onKey(makeKey(Key::Enter)), true);
    type(*area, "cd");
    CHECK_EQ(area->text(), std::string("ab\ncd"));
    CHECK_EQ(area->lineCount(), static_cast<std::size_t>(2));
    CHECK_EQ(area->caretLine(), static_cast<std::size_t>(1));
    CHECK(area->lineRange(0) == (TextArea::Selection{0, 2}));
    CHECK(area->lineRange(1) == (TextArea::Selection{3, 5}));

    area->onKey(makeKey(Key::Home));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(3));
    area->onKey(makeKey(Key::Backspace)); // deletes the '\n'
    CHECK_EQ(area->text(), std::string("abcd"));
    CHECK_EQ(area->lineCount(), static_cast<std::size_t>(1));
}

RIVET_TEST(textAreaSpacePlusMinusKeysInsertText) {
    auto area = makeArea();
    area->onKey(makeKey(Key::Character, "a"));
    area->onKey(makeKey(Key::Space));
    area->onKey(makeKey(Key::Minus));
    ModifierFlags shift = shiftMods();
    area->onKey(makeKey(Key::Plus, {}, shift));
    CHECK_EQ(area->text(), std::string("a -+"));
}

RIVET_TEST(textAreaWrapsAtSpacesAndFallsBackToCodePointBreaks) {
    auto area = makeArea("hello world foo");
    CHECK_EQ(area->lineCount(), static_cast<std::size_t>(2));
    CHECK(area->lineRange(0) == (TextArea::Selection{0, 6}));  // "hello "
    CHECK(area->lineRange(1) == (TextArea::Selection{6, 15})); // "world foo"

    // A word longer than a line breaks between code points.
    area->setText("abcdefghijklmnop");
    FakePaintContext longWord;
    area->paint(longWord);
    CHECK_EQ(area->lineCount(), static_cast<std::size_t>(2));
    CHECK(area->lineRange(0) == (TextArea::Selection{0, 10}));
    CHECK(area->lineRange(1) == (TextArea::Selection{10, 16}));

    // Cyrillic is 14 points per code point: 5 per line, never splitting one.
    area->setText("\xD0\xB0\xD0\xB1\xD0\xB2\xD0\xB3\xD0\xB4\xD0\xB5\xD0\xB6"); // 7 letters
    FakePaintContext context;
    area->paint(context);
    CHECK_EQ(area->lineCount(), static_cast<std::size_t>(2));
    CHECK(area->lineRange(0) == (TextArea::Selection{0, 10}));
    CHECK(area->lineRange(1) == (TextArea::Selection{10, 14}));
}

RIVET_TEST(textAreaPaintsWrappedLinesTopAlignedAndPlaceholderWhenEmpty) {
    auto area = std::make_unique<TextArea>("Type here");
    area->setFrame(Rect{0.0, 0.0, kWidth, kHeight});
    FakePaintContext empty;
    area->paint(empty);
    CHECK_EQ(empty.texts.size(), static_cast<std::size_t>(1));
    CHECK_EQ(empty.texts[0].text, std::string("Type here"));

    area->setText("hello world foo");
    FakePaintContext context;
    area->paint(context);
    CHECK_EQ(context.texts.size(), static_cast<std::size_t>(2));
    CHECK_EQ(context.texts[0].text, std::string("hello "));
    CHECK_EQ(context.texts[1].text, std::string("world foo"));
    CHECK_NEAR(context.texts[0].rect.minY(), TextArea::kPadding, 1e-9);
    CHECK_NEAR(context.texts[1].rect.minY(), TextArea::kPadding + TextArea::kLineHeight, 1e-9);
    CHECK_NEAR(context.texts[0].rect.size.height, TextArea::kLineHeight, 1e-9);
}

RIVET_TEST(textAreaUpDownMoveByXAcrossWrappedLines) {
    auto area = makeArea("hello world foo"); // lines "hello " / "world foo", caret at end (x = 63)
    CHECK_EQ(area->caretLine(), static_cast<std::size_t>(1));
    area->onKey(makeKey(Key::Up));
    CHECK_EQ(area->caretLine(), static_cast<std::size_t>(0));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(6)); // clamped to the end of the short line
    area->onKey(makeKey(Key::Down));
    CHECK_EQ(area->caretLine(), static_cast<std::size_t>(1));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(15)); // sticky x restored

    // Hard lines of different length.
    auto second = makeArea("ab\ncdefgh");
    second->onKey(makeKey(Key::Up));
    CHECK_EQ(second->caret(), static_cast<std::size_t>(2));
    second->onKey(makeKey(Key::Down));
    CHECK_EQ(second->caret(), static_cast<std::size_t>(9));

    // Up on the first line goes to the start, Down on the last to the end.
    second->onKey(makeKey(Key::Up));
    second->onKey(makeKey(Key::Up));
    CHECK_EQ(second->caret(), static_cast<std::size_t>(0));
    second->onKey(makeKey(Key::Down));
    second->onKey(makeKey(Key::Down));
    CHECK_EQ(second->caret(), static_cast<std::size_t>(9));
}

RIVET_TEST(textAreaHomeEndStayOnTheVisualLine) {
    auto area = makeArea("hello world foo");
    area->onKey(makeKey(Key::Up)); // first visual line, caret at 6 (displayed at its end)
    area->onKey(makeKey(Key::Home));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(0));
    area->onKey(makeKey(Key::End));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(6));
    CHECK_EQ(area->caretLine(), static_cast<std::size_t>(0)); // not on the next line

    // Cmd+Up/Down jump to the document ends.
    area->onKey(makeKey(Key::Down, {}, commandMods()));
    CHECK_EQ(area->caret(), static_cast<std::size_t>(15));
    area->onKey(makeKey(Key::Up, {}, commandMods(true))); // Shift extends
    CHECK_EQ(area->caret(), static_cast<std::size_t>(0));
    CHECK(area->selection() == (TextArea::Selection{0, 15}));
}

RIVET_TEST(textAreaShiftSelectionAndDelete) {
    auto area = makeArea("one\ntwo");
    area->onKey(makeKey(Key::Left, {}, shiftMods()));
    area->onKey(makeKey(Key::Left, {}, shiftMods()));
    CHECK(area->selection() == (TextArea::Selection{5, 7}));
    area->onKey(makeKey(Key::Up, {}, shiftMods()));
    CHECK(area->selection() == (TextArea::Selection{1, 7})); // x of offset 5 (one glyph in) maps to offset 1 above

    int changes = 0;
    area->setOnTextChanged([&](const std::string&) { ++changes; });
    area->onKey(makeKey(Key::Backspace));
    CHECK_EQ(changes, 1);
    CHECK_EQ(area->selection().begin, area->selection().end);
    CHECK_EQ(area->text(), std::string("o"));

    // Typing replaces a selection.
    area->onKey(makeKey(Key::Home, {}, shiftMods()));
    type(*area, "\xD0\xAF"); // Я
    CHECK_EQ(area->text(), std::string("\xD0\xAF"));
}

RIVET_TEST(textAreaSelectAllReplacesEverything) {
    auto area = makeArea("a\nb\nc");
    CHECK_EQ(area->onKey(makeKey(Key::Character, "a", commandMods())), true);
    CHECK(area->selection() == (TextArea::Selection{0, 5}));
    CHECK_EQ(area->onKey(makeKey(Key::Delete)), true);
    CHECK_EQ(area->text(), std::string());
    // Other command combinations pass through unconsumed.
    CHECK_EQ(area->onKey(makeKey(Key::Character, "c", commandMods())), false);
}

RIVET_TEST(textAreaMaxBytesTruncatesAtCodePointBoundary) {
    auto area = makeArea();
    area->setMaxBytes(5);
    type(*area, "ab");
    type(*area, "\xD0\x96"); // Ж: 4 bytes now
    CHECK_EQ(area->text().size(), static_cast<std::size_t>(4));
    // One more 2-byte code point would reach 6 bytes: rejected whole.
    area->onKey(makeKey(Key::Character, "\xD0\x96"));
    CHECK_EQ(area->text().size(), static_cast<std::size_t>(4));
    // ASCII still fits in the last byte.
    area->onKey(makeKey(Key::Character, "z"));
    CHECK_EQ(area->text().size(), static_cast<std::size_t>(5));

    // A multi-code-point insertion is cut at a boundary, not mid-sequence.
    auto other = makeArea();
    other->setMaxBytes(5);
    other->onKey(makeKey(Key::Character, "\xD0\x96\xD0\x96\xD0\x96")); // 6 bytes
    CHECK_EQ(other->text(), std::string("\xD0\x96\xD0\x96"));

    // setText and setMaxBytes truncate too.
    other->setText("\xD0\x96\xD0\x96\xD0\x96");
    CHECK_EQ(other->text().size(), static_cast<std::size_t>(4));
    other->setMaxBytes(3);
    CHECK_EQ(other->text(), std::string("\xD0\x96"));

    // Replacing a selection frees its bytes.
    auto third = makeArea("abcde");
    third->setMaxBytes(5);
    third->onKey(makeKey(Key::Character, "a", commandMods()));
    third->onKey(makeKey(Key::Character, "xyz"));
    CHECK_EQ(third->text(), std::string("xyz"));
}

RIVET_TEST(textAreaCommitEscapeAndFocusGating) {
    auto area = makeArea("x");
    int commits = 0;
    int escapes = 0;
    area->setOnCommit([&] { ++commits; });
    area->setOnEscape([&] { ++escapes; });
    CHECK_EQ(area->onKey(makeKey(Key::Enter, {}, commandMods())), true);
    CHECK_EQ(commits, 1);
    CHECK_EQ(area->text(), std::string("x")); // Cmd+Enter does not insert
    CHECK_EQ(area->onKey(makeKey(Key::Escape)), true);
    CHECK_EQ(escapes, 1);
    CHECK_EQ(area->onKey(makeKey(Key::Enter)), true);
    CHECK_EQ(area->text(), std::string("x\n"));
    CHECK_EQ(commits, 1);

    area->setFocused(false);
    CHECK_EQ(area->onKey(makeKey(Key::Character, "q")), false);
    CHECK_EQ(area->text(), std::string("x\n"));
}

RIVET_TEST(textAreaClickDragAndShiftClickSelect) {
    auto area = makeArea("abcdefgh\nxyz");
    int focusRequests = 0;
    area->setOnFocusRequested([&] { ++focusRequests; });
    const double y = TextArea::kPadding + 4.0; // first line
    const double x0 = TextArea::kPadding;

    CHECK_EQ(area->onMouse(makePointer(PointerEventType::Down, x0 + 7.0 * 3 + 1.0, y)), true);
    CHECK_EQ(focusRequests, 1);
    CHECK_EQ(area->caret(), static_cast<std::size_t>(3));
    CHECK_EQ(area->onMouse(makePointer(PointerEventType::Move, x0 + 7.0 * 6, y)), true);
    CHECK(area->selection() == (TextArea::Selection{3, 6}));
    CHECK_EQ(area->onMouse(makePointer(PointerEventType::Up, x0 + 7.0 * 6, y)), true);
    CHECK_EQ(area->onMouse(makePointer(PointerEventType::Move, x0 + 7.0, y)), false); // no longer dragging

    // Shift-click extends from the anchor; a click on the second line maps there.
    area->onMouse(makePointer(PointerEventType::Down, x0 + 7.0 * 2, y + TextArea::kLineHeight, true));
    CHECK_EQ(area->selection().begin, static_cast<std::size_t>(3));
    CHECK_EQ(area->selection().end, static_cast<std::size_t>(9 + 2));
}

RIVET_TEST(textAreaScrollsToKeepCaretVisibleAndHandlesWheel) {
    auto area = makeArea(); // three visible lines
    for (int i = 0; i < 5; ++i) area->onKey(makeKey(Key::Enter));
    CHECK_EQ(area->lineCount(), static_cast<std::size_t>(6));
    // Content 108 points, view 54: caret on the last line -> offset 54.
    CHECK_NEAR(area->scrollOffset(), 54.0, 1e-9);

    area->onKey(makeKey(Key::Up, {}, commandMods()));
    CHECK_NEAR(area->scrollOffset(), 0.0, 1e-9);

    PointerEvent wheel = makePointer(PointerEventType::Scroll, 20.0, 20.0);
    wheel.scrollDelta = Point{0.0, 18.0};
    CHECK_EQ(area->onMouse(wheel), true);
    CHECK_NEAR(area->scrollOffset(), 18.0, 1e-9);
    wheel.scrollDelta = Point{0.0, 500.0};
    area->onMouse(wheel);
    CHECK_NEAR(area->scrollOffset(), 54.0, 1e-9); // clamped
    wheel.scrollDelta = Point{0.0, -500.0};
    area->onMouse(wheel);
    CHECK_NEAR(area->scrollOffset(), 0.0, 1e-9);

    // Text that fits does not consume the wheel.
    auto small = makeArea("hi");
    CHECK_EQ(small->onMouse(wheel), false);

    // Painted lines follow the scroll offset.
    area->onKey(makeKey(Key::Down, {}, commandMods()));
    FakePaintContext context;
    area->paint(context);
    CHECK_NEAR(area->scrollOffset(), 54.0, 1e-9);
    CHECK_NEAR(context.texts.empty() ? 0.0 : context.texts.back().rect.minY(), 0.0, 1e-6);
}

RIVET_TEST(textAreaWorksBeforeFirstFrameAndPaint) {
    TextArea area("hint");
    area.setFocused(true);
    CHECK_EQ(area.onKey(makeKey(Key::Character, "a")), true);
    CHECK_EQ(area.onKey(makeKey(Key::Enter)), true);
    CHECK_EQ(area.onKey(makeKey(Key::Character, "\xD0\x96")), true);
    CHECK_EQ(area.onKey(makeKey(Key::Up)), true);
    CHECK_EQ(area.onKey(makeKey(Key::Down)), true);
    CHECK_EQ(area.lineCount(), static_cast<std::size_t>(2)); // hard breaks only, no wrapping
    CHECK_EQ(area.onMouse(makePointer(PointerEventType::Down, 5.0, 5.0)), false); // zero-size frame
    FakePaintContext context;
    area.paint(context); // zero frame: must not crash
    CHECK_EQ(area.text(), std::string("a\n\xD0\x96"));
}
