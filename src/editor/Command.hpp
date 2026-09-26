#pragma once

#include "core/Error.hpp"

#include <optional>
#include <string_view>

namespace rivet::editor {

// A single undoable operation. execute() runs the change the first time;
// redo() re-runs it after an undo (default: delegate to execute()).
class Command {
public:
    virtual ~Command() = default;

    // Human-readable operation name (for menus, logging). Must stay valid for
    // the lifetime of the command; static strings are expected.
    virtual std::string_view name() const = 0;

    // Returns false if the command could not be applied; a failed command is
    // never pushed onto the stack.
    virtual bool execute() = 0;

    virtual bool undo() = 0;

    virtual bool redo() { return execute(); }

    // Why the last execute()/undo()/redo() returned false (nullopt when the
    // command does not report reasons). Read by CommandStack before a
    // failed command is destroyed (see CommandStack::lastError).
    virtual std::optional<core::Error> failure() const { return std::nullopt; }
};

} // namespace rivet::editor
