#pragma once

#include "editor/Command.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>

namespace rivet::editor {

// Bounded undo/redo history for one document. Not thread-safe: it belongs to
// the main thread, like the editing commands it holds.
//
// Semantics:
//   - execute() runs the command first; only a successful execute() is pushed,
//     and pushing clears the redo stack.
//   - undo()/redo() move entries between the two stacks without dropping them
//     (a failed Command::undo()/redo() leaves the stacks untouched and reports
//     false).
//   - The undo stack never grows beyond maxDepth: the OLDEST entries are
//     evicted as new ones arrive, so the earliest history is what is lost.
//   - clear() drops both stacks WITHOUT undoing the commands and without
//     invoking any Command method.
class CommandStack {
public:
    explicit CommandStack(std::size_t maxDepth = 100);

    CommandStack(const CommandStack&) = delete;
    CommandStack& operator=(const CommandStack&) = delete;

    // Executes the command; on success pushes it (clearing the redo stack).
    // Returns false and does NOT push if execute() fails (or if command is
    // null); ownership of a failed command is released and the object is
    // destroyed. Either way the caller's unique_ptr is consumed.
    bool execute(std::unique_ptr<Command> command);

    // false if !canUndo(); on success the command moves to the redo stack.
    bool undo();

    // false if !canRedo(); on success the command moves back to the undo stack.
    bool redo();

    bool canUndo() const;
    bool canRedo() const;

    std::size_t depth() const;

    // Drops both stacks (no callbacks on the commands themselves, no undo).
    void clear();

    // Shrinking the bound evicts the oldest undo entries immediately.
    void setMaxDepth(std::size_t maxDepth);

    std::size_t maxDepth() const;

    // Optional observer, invoked after any stack mutation (execute/undo/redo/
    // clear). Never invoked from within a command. Must remain valid until the
    // stack is destroyed or replaced.
    void setOnChanged(std::function<void()> callback);

private:
    void trimToMaxDepth();
    void notifyChanged();

    std::size_t maxDepth_;
    std::deque<std::unique_ptr<Command>> undo_; // back = most recent
    std::deque<std::unique_ptr<Command>> redo_; // back = next to redo
    std::function<void()> onChanged_;
};

} // namespace rivet::editor
