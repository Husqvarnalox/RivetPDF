// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/MarkdownTabState.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/SourceEditor.hpp"
#include "ui/Widget.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace rivet::app {

// What the pane needs from the shell (all shell-owned, outlive the pane).
struct MarkdownSourcePaneEnvironment {
    const platform::ShellServices* services = nullptr; // clipboard
    std::function<void(std::string)> setStatus;        // status bar message
    std::function<void(ui::Widget*)> setFocus;         // keyboard focus routing
};

// Binds one ui::SourceEditor to the active MarkdownTabState (the integration
// step places the pane for the Source and Split display modes).
//
// Single source of truth: the TAB STATE's source is authoritative. The editor
// keeps a mirror (its TextBuffer) that is updated by exactly the edits the
// state accepts:
//   - user edit  -> editor asks the sink -> a TextEditCommand runs on the
//     tab's command stack (revision++, dirty tracking, onChanged) -> the
//     editor then applies the same edit to its mirror;
//   - external change (undo/redo, anything else that moves state->revision())
//     -> stateChanged() diffs mirror vs source and applies the minimal
//     replacement to the mirror, putting the caret at the end of the change.
//
// Coalescing: the editor flags edits that continue the previous one (typing
// run, backspace run, forward-delete run, no caret jump / newline / paste /
// word-start-after-space in between). The pane folds such an edit into the
// previous command (TextEditCommand::tryMerge) only when that command is still
// the newest history entry, nothing else touched the state, and the state is
// not at its saved checkpoint (a merge would not change the state id, so the
// tab could never turn dirty). Otherwise a new command is pushed.
//
// Undo/redo are NOT handled here: the editor does not consume Cmd+Z, the
// shell's shortcut path runs tab->undo()/redo() and calls stateChanged().
//
// Per-tab view state (caret, selection, top line) is remembered per state
// pointer while bound elsewhere. Main thread only.
class MarkdownSourcePane final : public ui::Widget {
public:
    explicit MarkdownSourcePane(MarkdownSourcePaneEnvironment environment);
    ~MarkdownSourcePane() override;

    // Rebinds to `state` (nullptr = drop every pointer into the previous
    // state; the editor is emptied). Re-binding the same state is a refresh.
    void bind(MarkdownTabState* state);
    // Call after every observable state change (the host forwards it).
    void stateChanged();

    MarkdownTabState* state() const { return state_; }
    ui::SourceEditor& editor() { return *editor_; }
    const ui::SourceEditor& editor() const { return *editor_; }
    // Asks the shell to route keyboard focus to the editor.
    void focusEditor();

    void layout() override;

private:
    class Sink;
    class Clipboard;

    void resyncFromState();
    void rememberView();

    struct ViewState {
        std::size_t anchor = 0;
        std::size_t caret = 0;
        std::size_t topOffset = 0;
    };

    MarkdownSourcePaneEnvironment environment_;
    ui::SourceEditor* editor_ = nullptr; // owned as a child
    std::unique_ptr<Sink> sink_;
    std::unique_ptr<Clipboard> clipboard_;
    MarkdownTabState* state_ = nullptr;
    std::uint64_t syncedRevision_ = 0;
    bool inOwnEdit_ = false;
    std::unordered_map<const MarkdownTabState*, ViewState> views_;

    // Merge memory (see class comment).
    TextEditCommand* lastCommand_ = nullptr;
    std::uint64_t lastStateId_ = 0;
    std::uint64_t lastRevision_ = 0;

    friend class Sink;
};

std::unique_ptr<MarkdownSourcePane> createMarkdownSourcePane(MarkdownSourcePaneEnvironment environment);

} // namespace rivet::app
