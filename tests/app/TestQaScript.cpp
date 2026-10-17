// SPDX-License-Identifier: MPL-2.0
// Scripted QA harness: the script parser, key combos and text typing.
#include "RivetTest.h"

#include "app/QaScript.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>

using namespace rivet;
using namespace rivet::app::qa;

namespace {

std::optional<std::string> fakeEnv(std::string_view name) {
    static const std::map<std::string, std::string, std::less<>> vars{{"FIX", "/tmp/fix"}, {"OUT", "shots"}};
    const auto it = vars.find(name);
    if (it == vars.end()) return std::nullopt;
    return it->second;
}

} // namespace

RIVET_TEST(qaParsesEveryCommand) {
    const Script script = parseScript(R"(# comment
open ${FIX}/long.pdf

cmd View/Zoom In…
click 10 20
dblclick 1.5 2.5 shift
rclick 3 4
drag 1 2 3 4 cmd+shift
scroll 100 200 0 -40
clickpage 2 72 720
dblclickpage 1 10 10
dragpage 3 1 2 3 4 alt
key cmd+shift+z
type hello world
wait 250
idle
shot ${OUT}/a.png
resize 940 600
theme Dark
answer save /tmp/out.pdf
answer confirm yes
answer savechanges dontsave
assert-status Saved
assert-find 1 of
assert-clipboard Rivet
assert-pages 4
assert-tabs 1
assert-enabled File/Save
assert-disabled Edit/Undo
assert-state scroll=0,0
log-state
log hello
quit
)",
                                        fakeEnv);
    CHECK(script.errors.empty());
    CHECK_EQ(script.steps.size(), std::size_t{31});
    CHECK(script.steps[0].kind == StepKind::Open);
    CHECK_EQ(script.steps[0].text, std::string("/tmp/fix/long.pdf"));
    CHECK_EQ(script.steps[0].line, 2);
    CHECK(script.steps[1].kind == StepKind::Cmd);
    CHECK_EQ(script.steps[1].text, std::string("View/Zoom In\xE2\x80\xA6"));
    CHECK(script.steps[3].kind == StepKind::DoubleClick);
    CHECK(script.steps[3].modifiers.shift);
    CHECK_EQ(script.steps[3].numbers.size(), std::size_t{2});
    CHECK(script.steps[5].kind == StepKind::Drag);
    CHECK(script.steps[5].modifiers.shift);
    CHECK(script.steps[5].modifiers.command || script.steps[5].modifiers.control);
    CHECK(script.steps[7].kind == StepKind::ClickPage);
    CHECK_EQ(script.steps[7].page, 2);
    CHECK_EQ(script.steps[7].numbers.size(), std::size_t{2});
    CHECK(script.steps[9].kind == StepKind::DragPage);
    CHECK_EQ(script.steps[9].page, 3);
    CHECK_EQ(script.steps[9].numbers.size(), std::size_t{4});
    CHECK(script.steps[9].modifiers.option);
    CHECK(script.steps[11].kind == StepKind::Type);
    CHECK_EQ(script.steps[11].text, std::string("hello world"));
    CHECK(script.steps[14].kind == StepKind::Shot);
    CHECK_EQ(script.steps[14].text, std::string("shots/a.png"));
    CHECK(script.steps[16].kind == StepKind::Theme);
    CHECK_EQ(script.steps[16].text, std::string("dark"));
    CHECK(script.steps[17].kind == StepKind::Answer);
    CHECK_EQ(script.steps[17].answerKind, std::string("save"));
    CHECK_EQ(script.steps[17].text, std::string("/tmp/out.pdf"));
    CHECK_EQ(script.steps[18].answerKind, std::string("confirm"));
    CHECK(script.steps[27].kind == StepKind::AssertState);
    CHECK_EQ(script.steps[27].text, std::string("scroll=0,0"));
    CHECK(script.steps[30].kind == StepKind::Quit);
}

RIVET_TEST(qaReportsMalformedLines) {
    const Script script = parseScript("bogus 1\n"
                                      "click 1\n"
                                      "click a b\n"
                                      "clickpage 0 1 1\n"
                                      "open ${NOPE}/x.pdf\n"
                                      "key cmd+\n"
                                      "theme purple\n"
                                      "answer confirm maybe\n"
                                      "cmd NoSlash\n"
                                      "wait -5\n"
                                      "resize 0 10\n"
                                      "open\n"
                                      "idle extra\n"
                                      "click 1 2 hyper\n"
                                      "log fine\n",
                                      fakeEnv);
    CHECK_EQ(script.errors.size(), std::size_t{14});
    CHECK_EQ(script.steps.size(), std::size_t{1});
    CHECK_EQ(script.errors.front().line, 1);
    CHECK_EQ(script.errors.back().line, 14);
}

RIVET_TEST(qaHandlesCrLfAndNoTrailingNewline) {
    const Script script = parseScript("log a\r\n\r\n  # indented comment\r\nquit", fakeEnv);
    CHECK(script.errors.empty());
    CHECK_EQ(script.steps.size(), std::size_t{2});
    CHECK_EQ(script.steps[1].line, 4);
}

RIVET_TEST(qaKeyCombos) {
    const auto b = parseKeyCombo("cmd+b", true);
    CHECK(b.has_value());
    CHECK(b->key == ui::Key::Character);
    CHECK_EQ(b->text, std::string("b"));
    CHECK(b->modifiers.command);
    CHECK(!b->modifiers.control);

    const auto ctrlB = parseKeyCombo("cmd+b", false);
    CHECK(ctrlB.has_value());
    CHECK(ctrlB->modifiers.control);
    CHECK(!ctrlB->modifiers.command);

    const auto redo = parseKeyCombo("cmd+shift+z", true);
    CHECK(redo.has_value());
    CHECK(redo->modifiers.shift);
    CHECK_EQ(redo->text, std::string("Z"));

    CHECK(parseKeyCombo("tab", true)->key == ui::Key::Tab);
    CHECK(parseKeyCombo("shift+tab", true)->modifiers.shift);
    CHECK(parseKeyCombo("Escape", true)->key == ui::Key::Escape);
    CHECK(parseKeyCombo("return", true)->key == ui::Key::Enter);
    CHECK(parseKeyCombo("space", true)->key == ui::Key::Space);
    CHECK(parseKeyCombo("cmd++", true)->key == ui::Key::Plus);
    CHECK(parseKeyCombo("cmd+-", true)->key == ui::Key::Minus);
    CHECK(parseKeyCombo("cmd+=", true)->key == ui::Key::Plus);
    CHECK(!parseKeyCombo("", true).has_value());
    CHECK(!parseKeyCombo("hyper+a", true).has_value());
    CHECK(!parseKeyCombo("ab", true).has_value());
}

RIVET_TEST(qaTypingProducesOneEventPerCodePoint) {
    const auto events = keyEventsForText("a \xC3\xA9\\n\\t\\\\x");
    CHECK_EQ(events.size(), std::size_t{7});
    CHECK(events[0].key == ui::Key::Character);
    CHECK(events[1].key == ui::Key::Space);
    CHECK_EQ(events[2].text, std::string("\xC3\xA9"));
    CHECK(events[3].key == ui::Key::Enter);
    CHECK(events[4].key == ui::Key::Tab);
    CHECK_EQ(events[5].text, std::string("\\"));
    CHECK_EQ(events[6].text, std::string("x"));
}

RIVET_TEST(qaMenuTitleNormalization) {
    CHECK_EQ(normalizeMenuTitle("Save As\xE2\x80\xA6"), std::string("save as"));
    CHECK_EQ(normalizeMenuTitle(" Save As... "), std::string("save as"));
    CHECK_EQ(normalizeMenuTitle("Undo"), std::string("undo"));
}
