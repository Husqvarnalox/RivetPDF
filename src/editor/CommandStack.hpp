#pragma once

#include "editor/Command.hpp"

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
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
//
// State ids (dirty tracking): every history position has a stateId().
// The initial state is 0; each successful execute() mints a fresh id from a
// monotonic counter (never reused), undo() returns to the id of the state
// below, redo() to the redone command's id. So "undo back to the saved
// state" matches the saved id again, while a NEW command executed after an
// undo gets a new id and can never falsely match. Evicting the oldest
// entries and clear() keep the current id (the document did not change).
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

    // Identity of the current history position (see class comment).
    std::uint64_t stateId() const;

    // Reason for the most recent failed execute()/undo()/redo() when the
    // command reported one (Command::failure()); reset by every successful
    // operation.
    const std::optional<core::Error>& lastError() const { return lastError_; }

    // Optional observer, invoked after any stack mutation (execute/undo/redo/
    // clear). Never invoked from within a command. Must remain valid until the
    // stack is destroyed or replaced.
    void setOnChanged(std::function<void()> callback);

private:
    void trimToMaxDepth();
    void notifyChanged();

    struct Entry {
        std::unique_ptr<Command> command;
        std::uint64_t stateId = 0; // state reached by applying `command`
    };

    std::size_t maxDepth_;
    std::deque<Entry> undo_; // back = most recent
    std::deque<Entry> redo_; // back = next to redo
    // State below the oldest undo entry (0 until something was evicted).
    std::uint64_t baseStateId_ = 0;
    std::uint64_t lastStateId_ = 0;
    std::optional<core::Error> lastError_;
    std::function<void()> onChanged_;
};

} // namespace rivet::editor
