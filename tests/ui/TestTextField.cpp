// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "ui/TextField.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::ui::KeyEvent;
using rivet::ui::Key;
using rivet::ui::ModifierFlags;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::TextField;
using rivet::ui::testing::FakePaintContext;

namespace {

PointerEvent makeClick(double x, double y = 12.0, bool shift = false) {
    PointerEvent event;
    event.type = PointerEventType::Down;
    event.position = Point{x, y};
    event.button = 1;
    event.modifiers.shift = shift;
    return event;
}

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

ModifierFlags commandMods() {
    ModifierFlags modifiers;
    modifiers.command = true;
    return modifiers;
}

ModifierFlags controlMods() {
    ModifierFlags modifiers;
    modifiers.control = true;
    return modifiers;
}

// Unique_ptr because Widget is non-copyable (move-only ownership in the tree).
std::unique_ptr<TextField> makeField(const std::string& text, double width = 100.0) {
    auto field = std::make_unique<TextField>();
    field->setFrame(Rect{0.0, 0.0, width, 24.0});
    field->setText(text);
    return field;
}

// "abж" is 4 bytes: a(1) b(1) ж(2, D0 B6). FakePaintContext measures 7 px per
// byte, so per-code-point prefix widths are [0, 7, 14, 28] at bytes [0, 1, 2, 4].
template <typename T>
void primePaint(T& field) {
    FakePaintContext context;
    field->paint(context);
}

} // namespace

RIVET_TEST(setTextResetsCaretAndClearsSelection) {
    auto field = makeField("");
    field->setFocused(true);
    field->setText("abж");
    CHECK_EQ(field->caret(), std::size_t{4});

    // Extend a selection (Home first: the caret starts at the end), then
    // replace the text.
    CHECK_EQ(field->onKey(makeKey(Key::Home)), true);
    CHECK_EQ(field->onKey(makeKey(Key::Right, {}, shiftMods())), true);
    CHECK_EQ(field->onKey(makeKey(Key::Right, {}, shiftMods())), true);
    CHECK_EQ(field->selection(), (TextField::Selection{0, 2}));

    field->setText("x");
    CHECK_EQ(field->text(), "x");
    CHECK_EQ(field->caret(), std::size_t{1});
    CHECK_EQ(field->selection(), (TextField::Selection{1, 1}));
}

RIVET_TEST(typedCharactersInsertAtCaret) {
    auto field = makeField("");
    field->setFocused(true);

    int changes = 0;
    field->setOnTextChanged([&changes](const std::string&) { ++changes; });

    CHECK_EQ(field->onKey(makeKey(Key::Character, "x")), true);
    CHECK_EQ(field->text(), "x");
    CHECK_EQ(field->caret(), std::size_t{1});
    CHECK_EQ(changes, 1);

    // Direct multi-byte input advances the caret by whole code points.
    CHECK_EQ(field->onKey(makeKey(Key::Character, "ж")), true);
    CHECK_EQ(field->text(), "xж");
    CHECK_EQ(field->caret(), std::size_t{3});
    CHECK_EQ(changes, 2);
}

RIVET_TEST(caretMovesAcrossUtf8Boundaries) {
    auto field = makeField("abж");
    field->setFocused(true);

    // End -> 4; each Left steps back one code point: 2 (before ж), 1, 0.
    CHECK_EQ(field->onKey(makeKey(Key::Left)), true);
    CHECK_EQ(field->caret(), std::size_t{2});
    CHECK_EQ(field->onKey(makeKey(Key::Left)), true);
    CHECK_EQ(field->caret(), std::size_t{1});
    CHECK_EQ(field->onKey(makeKey(Key::Left)), true);
    CHECK_EQ(field->caret(), std::size_t{0});
    CHECK_EQ(field->onKey(makeKey(Key::Left)), true);
    CHECK_EQ(field->caret(), std::size_t{0}); // stays at start

    // Each Right steps forward: 1, 2, 4 (past ж), 4 (stays at end).
    CHECK_EQ(field->onKey(makeKey(Key::Right)), true);
    CHECK_EQ(field->caret(), std::size_t{1});
    CHECK_EQ(field->onKey(makeKey(Key::Right)), true);
    CHECK_EQ(field->caret(), std::size_t{2});
    CHECK_EQ(field->onKey(makeKey(Key::Right)), true);
    CHECK_EQ(field->caret(), std::size_t{4});
    CHECK_EQ(field->onKey(makeKey(Key::Right)), true);
    CHECK_EQ(field->caret(), std::size_t{4});

    CHECK_EQ(field->onKey(makeKey(Key::Home)), true);
    CHECK_EQ(field->caret(), std::size_t{0});
    CHECK_EQ(field->onKey(makeKey(Key::End)), true);
    CHECK_EQ(field->caret(), std::size_t{4});
}

RIVET_TEST(backspaceDeletesWholeCodePoints) {
    auto field = makeField("abж");
    field->setFocused(true);

    int changes = 0;
    field->setOnTextChanged([&changes](const std::string&) { ++changes; });

    CHECK_EQ(field->onKey(makeKey(Key::Backspace)), true);
    CHECK_EQ(field->text(), "ab");
    CHECK_EQ(field->caret(), std::size_t{2});
    CHECK_EQ(changes, 1);

    // Backspace at 0 is a no-op (but still consumed).
    CHECK_EQ(field->onKey(makeKey(Key::Home)), true);
    CHECK_EQ(field->onKey(makeKey(Key::Backspace)), true);
    CHECK_EQ(field->text(), "ab");
    CHECK_EQ(field->caret(), std::size_t{0});
    CHECK_EQ(changes, 1);
}

RIVET_TEST(deleteForwardAndNoOpAtEnd) {
    auto field = makeField("abж");
    field->setFocused(true);

    // Caret starts at the end; move Home so Delete-forward has something to
    // delete (the first code point).
    CHECK_EQ(field->onKey(makeKey(Key::Home)), true);
    CHECK_EQ(field->onKey(makeKey(Key::Delete)), true);
    CHECK_EQ(field->text(), "bж");
    CHECK_EQ(field->caret(), std::size_t{0});

    // Delete at the end is a no-op ("bж" is 3 bytes: b + 2-byte ж).
    CHECK_EQ(field->onKey(makeKey(Key::End)), true);
    CHECK_EQ(field->onKey(makeKey(Key::Delete)), true);
    CHECK_EQ(field->text(), "bж");
    CHECK_EQ(field->caret(), std::size_t{3});
}

RIVET_TEST(shiftArrowExtendsAndCollapsesSelection) {
    auto field = makeField("abж");
    field->setFocused(true);

    CHECK_EQ(field->onKey(makeKey(Key::Home)), true); // caret to start
    CHECK_EQ(field->onKey(makeKey(Key::Right, {}, shiftMods())), true);
    CHECK_EQ(field->onKey(makeKey(Key::Right, {}, shiftMods())), true);
    CHECK_EQ(field->selection(), (TextField::Selection{0, 2}));

    // Arrow without shift moves the caret and collapses the selection.
    CHECK_EQ(field->onKey(makeKey(Key::Right)), true);
    CHECK_EQ(field->caret(), std::size_t{4});
    CHECK_EQ(field->selection(), (TextField::Selection{4, 4}));
}

RIVET_TEST(selectAllAndTypingReplacesSelection) {
    auto field = makeField("abж");
    field->setFocused(true);

    int changes = 0;
    field->setOnTextChanged([&changes](const std::string&) { ++changes; });

    CHECK_EQ(field->onKey(makeKey(Key::Character, "a", controlMods())), true);
    CHECK_EQ(field->selection(), (TextField::Selection{0, 4}));

    CHECK_EQ(field->onKey(makeKey(Key::Character, "a", commandMods())), true);
    CHECK_EQ(field->selection(), (TextField::Selection{0, 4}));
    CHECK_EQ(changes, 0); // selection only: no text change

    CHECK_EQ(field->onKey(makeKey(Key::Character, "z")), true);
    CHECK_EQ(field->text(), "z");
    CHECK_EQ(field->caret(), std::size_t{1});
    CHECK_EQ(field->selection(), (TextField::Selection{1, 1}));
    CHECK_EQ(changes, 1);
}

RIVET_TEST(enterAndEscapeFireCallbacks) {
    auto field = makeField("hi");
    field->setFocused(true);

    int enters = 0;
    int escapes = 0;
    field->setOnEnter([&enters] { ++enters; });
    field->setOnEscape([&escapes] { ++escapes; });

    CHECK_EQ(field->onKey(makeKey(Key::Enter)), true);
    CHECK_EQ(enters, 1);
    CHECK_EQ(field->onKey(makeKey(Key::Escape)), true);
    CHECK_EQ(escapes, 1);
}

RIVET_TEST(clickRequestsFocusAndPlacesCaret) {
    auto field = makeField("abж");
    primePaint(field);

    int focusRequests = 0;
    field->setOnFocusRequested([&focusRequests] { ++focusRequests; });

    // Click at text x=10 -> nearest boundary byte 1 (widths 0/7/14/28).
    CHECK_EQ(field->onMouse(makeClick(16.0)), true);
    CHECK_EQ(focusRequests, 1);
    CHECK_EQ(field->caret(), std::size_t{1});

    // Fires again even when already "requested" before: the host decides.
    CHECK_EQ(field->onMouse(makeClick(36.0)), true);
    CHECK_EQ(focusRequests, 2);
    CHECK_EQ(field->caret(), std::size_t{4}); // nearest boundary to text x=30

    // Shift+click extends from the anchor.
    CHECK_EQ(field->onMouse(makeClick(16.0)), true);
    CHECK_EQ(field->onMouse(makeClick(36.0, 12.0, true)), true);
    CHECK_EQ(field->selection(), (TextField::Selection{1, 4}));
}

RIVET_TEST(dragSelectExtendsUntilMouseUp) {
    auto field = makeField("abж");
    primePaint(field);

    CHECK_EQ(field->onMouse(makeClick(16.0)), true);
    CHECK_EQ(field->selection(), (TextField::Selection{1, 1}));

    PointerEvent move;
    move.type = PointerEventType::Move;
    move.position = Point{36.0, 12.0};
    move.button = 1;
    CHECK_EQ(field->onMouse(move), true);
    CHECK_EQ(field->selection(), (TextField::Selection{1, 4}));

    PointerEvent up;
    up.type = PointerEventType::Up;
    up.position = Point{36.0, 12.0};
    up.button = 1;
    CHECK_EQ(field->onMouse(up), true);

    // After the up, moves no longer change the selection.
    CHECK_EQ(field->onMouse(move), false);
    CHECK_EQ(field->selection(), (TextField::Selection{1, 4}));
}

RIVET_TEST(typingWithCommandModifierIsNotConsumed) {
    auto field = makeField("ab");
    field->setFocused(true);

    CHECK_EQ(field->onKey(makeKey(Key::Character, "p", commandMods())), false);
    CHECK_EQ(field->text(), "ab");

    CHECK_EQ(field->onKey(makeKey(Key::Character, "c", controlMods())), false);
    CHECK_EQ(field->text(), "ab");

    // Other control shortcuts pass through too.
    CHECK_EQ(field->onKey(makeKey(Key::Left, {}, controlMods())), false);
}

RIVET_TEST(unfocusedFieldIgnoresAllKeys) {
    auto field = makeField("ab");

    int enters = 0;
    field->setOnEnter([&enters] { ++enters; });

    CHECK_EQ(field->onKey(makeKey(Key::Character, "x")), false);
    CHECK_EQ(field->onKey(makeKey(Key::Left)), false);
    CHECK_EQ(field->onKey(makeKey(Key::Enter)), false);
    CHECK_EQ(field->text(), "ab");
    CHECK_EQ(enters, 0);
}

RIVET_TEST(horizontalScrollFollowsCaret) {
    // 20 characters * 7 px = 140 px of text; visible width 80 - 2*6 = 68 px.
    auto field = makeField("01234567890123456789", 80.0);
    field->setFocused(true);
    FakePaintContext context;

    // setText put the caret at the end; painting scrolls it into view.
    field->paint(context);
    CHECK_GT(field->horizontalScrollOffset(), 0.0);

    // Home scrolls back to 0.
    CHECK_EQ(field->onKey(makeKey(Key::Home)), true);
    field->paint(context);
    CHECK_NEAR(field->horizontalScrollOffset(), 0.0, 1e-9);

    // End scrolls right again.
    CHECK_EQ(field->onKey(makeKey(Key::End)), true);
    field->paint(context);
    CHECK_GT(field->horizontalScrollOffset(), 0.0);
}

RIVET_TEST(preferredSizePadsMeasuredText) {
    FakePaintContext context;
    auto field = makeField("ab");
    const rivet::core::Size size = field->preferredSize(context);
    CHECK_NEAR(size.width, 14.0 + 2.0 * TextField::kHorizontalPadding, 1e-9);
    CHECK_NEAR(size.height, TextField::kMinHeight, 1e-9);
}
