// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "platform/PlatformKit.hpp"
#include "ui/UiTypes.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Scripted QA harness (debug/QA feature, portable).
//
// When the environment variable RIVET_QA_SCRIPT=<file> is set, the platform
// entry point creates a QaSession: it swaps the interactive dialogs and
// alerts for non-interactive doubles (answers come from the script), wraps
// the appearance so the script can switch light/dark, and then runs the
// script line by line on the main thread, waiting for the shell to be idle
// between steps. Without the variable nothing here is constructed: normal
// behaviour is untouched.
//
// Script format: one command per line, `#` starts a comment line, blank
// lines are ignored, `${NAME}` expands an environment variable (an unset
// variable is a parse error). Coordinates are logical points in the root
// widget's space (the window content area below any in-window menu strip).
//
//   open <path>                 open a document (new tab)
//   cmd <Menu>/<Item>           run a menu command (fails when disabled)
//   assert-enabled <Menu>/<Item>, assert-disabled <Menu>/<Item>
//   click|dblclick|rclick <x> <y> [mods]
//   drag <x1> <y1> <x2> <y2> [mods]
//   scroll <x> <y> <dx> <dy>    wheel scroll (positive dy = content moves up)
//   clickpage|dblclickpage <page> <ux> <uy> [mods]
//   dragpage <page> <ux1> <uy1> <ux2> <uy2> [mods]
//                               PDF user-space points (origin bottom-left)
//                               on 1-based <page>, mapped through the
//                               viewport so scenarios are zoom-independent
//   key <combo>                 e.g. cmd+b, shift+tab, escape, return
//                               (cmd/primary = Cmd on macOS, Ctrl elsewhere)
//   type <utf8 text>            \n = Return, \t = Tab, \\ = backslash
//   wait <ms> | idle
//   shot <path.png>             relative paths resolve against $RIVET_QA_OUT
//   resize <w> <h> | theme light|dark|system
//   answer save|open <path>     queue the answer of the next save/open dialog
//   answer confirm yes|no       ... of the next confirmation alert
//   answer text <text>          ... of the next text prompt
//   answer savechanges save|dontsave|cancel
//   answer review review|discard|cancel
//   assert-status <substring>   status-bar text contains <substring>
//   assert-find <substring>     find-bar result text contains <substring>
//   assert-clipboard <substring>
//   assert-pages <n> | assert-tabs <n>
//   log-state | log <text> | quit
//
// Output goes to stderr with a `[qa]` prefix; the process exit code is 0
// when every step passed, 1 when a step or assert failed, 2 for a script
// that does not parse or cannot be read.

namespace rivet::app {

class ShellController;

namespace qa {

enum class StepKind : std::uint8_t {
    Open,
    Cmd,
    AssertEnabled,
    AssertDisabled,
    Click,
    DoubleClick,
    RightClick,
    Drag,
    Scroll,
    ClickPage,
    DoubleClickPage,
    DragPage,
    Key,
    Type,
    Wait,
    Idle,
    Shot,
    Resize,
    Theme,
    Answer,
    AssertStatus,
    AssertFind,
    AssertClipboard,
    AssertPages,
    AssertTabs,
    LogState,
    Log,
    Quit,
};

struct Step {
    StepKind kind = StepKind::Log;
    int line = 0;           // 1-based source line
    std::string source;     // the (expanded) source line, for the log
    std::string text;       // path / menu path / combo / text / substring / theme / answer value
    std::string answerKind; // Answer: save | open | confirm | text | savechanges | review
    std::vector<double> numbers;
    int page = 0; // *page commands: 1-based page
    ui::ModifierFlags modifiers;
};

struct ParseError {
    int line = 0;
    std::string message;
};

struct Script {
    std::vector<Step> steps;
    std::vector<ParseError> errors;
};

// Environment lookup used for ${NAME} expansion (nullopt = unset).
using EnvLookup = std::function<std::optional<std::string>(std::string_view)>;

// Parses a whole script. Never throws; every malformed line becomes an error.
Script parseScript(std::string_view source, const EnvLookup& env);

// "cmd+shift+z", "escape", "a", "tab"... `primaryIsCommand` selects what
// cmd/primary means (Command on macOS, Control elsewhere).
std::optional<ui::KeyEvent> parseKeyCombo(std::string_view combo, bool primaryIsCommand);

// The key events typing `text` produces (one per code point; escapes as
// documented above).
std::vector<ui::KeyEvent> keyEventsForText(std::string_view text);

// Menu title normalization for `cmd`: lower case, a trailing "…" or "..."
// removed, surrounding blanks trimmed.
std::string normalizeMenuTitle(std::string_view title);

} // namespace qa

// Owns the doubles and the runner of one scripted run. Main thread only.
// Lifetime: create before the shell, install the doubles into the services
// handed to createShell(), start() after the shell exists, and destroy the
// session only after the shell is gone (the shell borrows the doubles).
class QaSession {
public:
    // Non-null when RIVET_QA_SCRIPT is set (a session whose script cannot be
    // read or parsed reports that and quits with exit code 2 on start()).
    static std::unique_ptr<QaSession> fromEnvironment();
    // A session over an in-memory script (tests).
    static std::unique_ptr<QaSession> fromSource(std::string_view source, std::string name);

    ~QaSession();
    QaSession(const QaSession&) = delete;
    QaSession& operator=(const QaSession&) = delete;

    // Replaces the file/save dialogs and the alert service with scripted
    // doubles and wraps the appearance (theme switching). Call before
    // createShell(services).
    void installDoubles(platform::ShellServices& services);

    // Starts running the script on the main thread through the services'
    // main dispatcher. `quit(exitCode)` is called once, on the main thread,
    // when the script ends (or on `quit`); the host then leaves its event
    // loop and returns exitCode() from main.
    void start(ShellController& shell, const platform::ShellServices& services, std::function<void(int)> quit);

    int exitCode() const;
    bool finished() const;

private:
    QaSession();
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace rivet::app
