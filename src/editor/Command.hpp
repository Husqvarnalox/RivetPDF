#pragma once

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
};

} // namespace rivet::editor
