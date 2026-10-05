// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "editor/Command.hpp"
#include "editor/CommandStack.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace rivet::app {

// Which kind of document a tab hosts. Chosen from the file extension only
// (never by sniffing content): see documentKindForPath().
enum class DocumentKind : std::uint8_t { Pdf, Markdown };

// .md / .markdown / .mdown (case-insensitive) -> Markdown; everything else is
// treated as a PDF (the historical behavior: any file the user picks with the
// PDF chooser or passes on the command line is handed to the PDF engine).
DocumentKind documentKindForPath(const std::filesystem::path& path);
// True for the Markdown extensions above (used by Save As to keep them).
bool hasMarkdownExtension(const std::filesystem::path& path);
// The Markdown extensions without the dot, lower case (dialog filters).
inline constexpr std::string_view kMarkdownExtensions[] = {"md", "markdown", "mdown"};

enum class LineEnding : std::uint8_t { LF, CRLF };

// How a Markdown tab presents its document. Rendered is the default.
enum class MarkdownDisplayMode : std::uint8_t { Rendered, Source, Split };

// A file's bytes decoded for the text editor.
struct DecodedText {
    std::string text;            // UTF-8, BOM dropped, every CRLF turned into LF
    LineEnding lineEnding = LineEnding::LF; // style to write back on save
    bool hadBom = false;         // re-emitted on save
    bool mixedLineEndings = false; // file mixed CRLF and bare LF (normalized on save)
};

// Strict UTF-8 validation (no overlong forms, no surrogates, <= U+10FFFF).
bool isValidUtf8(std::string_view bytes);

// Decodes a file's bytes. Non-UTF-8 input (including UTF-16 with a BOM) is a
// controlled InvalidDocument error: the bytes are never reinterpreted.
core::Result<DecodedText> decodeMarkdownBytes(std::string_view bytes);
// Inverse of decode: LF -> `lineEnding`, BOM prepended when `bom`.
std::string encodeMarkdownBytes(std::string_view text, LineEnding lineEnding, bool bom);

// Files larger than this are refused with a controlled error (the source is
// held in memory and parsed as a whole).
inline constexpr std::uint64_t kMaxMarkdownFileBytes = 64ull * 1024 * 1024;

// Everything one Markdown tab owns (portable; no PDF, no UI). Main thread
// only, except that load() runs on a worker (it builds a fresh state).
//
// Source text: UTF-8 with LF line endings in memory regardless of the file's
// style; the file's style and BOM are re-applied by encode()/save.
//
// Edits: ALL source changes go through commands on commands() (see
// TextEditCommand) so undo/redo and dirty tracking stay exact. revision() is
// a monotonic counter bumped by every source mutation (execute, undo, redo):
// views key their caches on it. Dirty tracking uses the command stack's state
// ids like DocumentSession: undoing back to the saved state is clean again.
class MarkdownTabState {
public:
    explicit MarkdownTabState(std::filesystem::path path, DecodedText decoded = {});
    ~MarkdownTabState();

    MarkdownTabState(const MarkdownTabState&) = delete;
    MarkdownTabState& operator=(const MarkdownTabState&) = delete;

    // Worker-safe: reads the file (bounded by kMaxMarkdownFileBytes), decodes
    // it and returns a fresh state. `isCancelled` (optional) is polled between
    // chunks. Errors: NotFound / PermissionDenied / Io, InvalidDocument
    // (not UTF-8), OutOfMemory-style "too large" reported as InvalidArgument.
    static core::Result<std::unique_ptr<MarkdownTabState>> load(
        const std::filesystem::path& path, const std::function<bool()>& isCancelled = {});

    const std::filesystem::path& path() const { return path_; }
    void setPath(std::filesystem::path path) { path_ = std::move(path); }

    const std::string& source() const { return source_; }
    LineEnding lineEnding() const { return lineEnding_; }
    bool hasBom() const { return hasBom_; }
    bool hadMixedLineEndings() const { return mixedLineEndings_; }
    // The bytes a save writes for the current source.
    std::string encode() const { return encodeMarkdownBytes(source_, lineEnding_, hasBom_); }

    std::uint64_t revision() const { return revision_; }
    // Revision at the last save (the load counts as saved).
    std::uint64_t savedRevision() const { return savedRevision_; }

    MarkdownDisplayMode mode() const { return mode_; }
    void setMode(MarkdownDisplayMode mode);

    // --- Editing ------------------------------------------------------------
    // Runs `command` (it mutates the source through applyEdit()). False when
    // the command failed or editing is locked (a save is in flight).
    bool execute(std::unique_ptr<editor::Command> command);
    bool undo();
    bool redo();
    bool canUndo() const { return !editingLocked_ && commands_.canUndo(); }
    bool canRedo() const { return !editingLocked_ && commands_.canRedo(); }
    editor::CommandStack& commands() { return commands_; }
    const editor::CommandStack& commands() const { return commands_; }

    // Replaces [offset, offset+removeLength) of the source by `insert` (both
    // must lie on UTF-8 boundaries; the caller is the command). Bumps the
    // revision and notifies. Returns false when the range is invalid. Meant
    // for commands only: it does not touch the history.
    bool applyEdit(std::size_t offset, std::size_t removeLength, std::string_view insert);

    // --- Dirty / save --------------------------------------------------------
    bool isDirty() const { return commands_.stateId() != savedStateId_; }
    // The current history position becomes the saved one.
    void markSaved();
    // While a save is in flight the written snapshot must stay the saved
    // state: execute/undo/redo refuse (the shell reports it).
    void setEditingLocked(bool locked) { editingLocked_ = locked; }
    bool isEditingLocked() const { return editingLocked_; }

    // Fired (main thread, synchronously) after any observable change: source
    // mutation, mode change, dirty flip. Views and the shell repaint from it.
    void setOnChanged(std::function<void()> onChanged) { onChanged_ = std::move(onChanged); }
    // For TextEditCommand::tryMerge(): an edit folded into an existing history
    // entry still has to wake the views (the command stack is not involved).
    void notifyEdited() { notifyChanged(); }

private:
    void notifyChanged() {
        if (onChanged_) onChanged_();
    }

    std::filesystem::path path_;
    std::string source_;
    LineEnding lineEnding_ = LineEnding::LF;
    bool hasBom_ = false;
    bool mixedLineEndings_ = false;
    std::uint64_t revision_ = 0;
    std::uint64_t savedRevision_ = 0;
    std::uint64_t savedStateId_ = 0;
    MarkdownDisplayMode mode_ = MarkdownDisplayMode::Rendered;
    bool editingLocked_ = false;
    editor::CommandStack commands_;
    std::function<void()> onChanged_;
};

// The one text-edit command: replaces a byte range with new text, undoable.
// The removed text is captured on execute(), so the command is value-complete.
class TextEditCommand final : public editor::Command {
public:
    TextEditCommand(MarkdownTabState& state, std::size_t offset, std::size_t removeLength,
                    std::string insert, std::string_view name = "Edit Text");

    std::string_view name() const override { return name_; }
    bool execute() override;
    bool undo() override;

    // Folds a FOLLOWING edit (offsets in the source as it is now, i.e. after
    // this command ran) into this command and applies it to the state, so one
    // undo reverts both. Only for the newest history entry of a state that is
    // not the saved one (the caller checks; otherwise the dirty state id would
    // not change). Supported shapes: inside or touching the inserted text
    // (typing, backspace within it), directly after it (typing on, forward
    // delete) and directly before it (backspace before it). Returns false and
    // changes nothing otherwise. Notifies the state like any edit.
    bool tryMerge(std::size_t offset, std::size_t removeLength, std::string_view insert);

private:
    MarkdownTabState& state_;
    std::size_t offset_;
    std::size_t removeLength_;
    std::string insert_;
    std::string removed_;
    std::string name_;
};

} // namespace rivet::app
