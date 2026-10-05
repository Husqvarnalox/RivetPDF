# ADR-0021: Source text as authoritative state for Markdown

Status: Accepted

Date: 2026-10-05

## Context

Markdown editing must store its source text in a way that:
- Survives undo/redo without losing edits or diverging from the source file.
- Preserves the file's line ending style (LF, CRLF) and BOM on save.
- Is efficient and auditable: the source is the only state that matters; the parsed model is derived
  and never regenerated (ADR-0019).

## Decision

**Source text is authoritative.** The `MarkdownTabState` owns the UTF-8 source and never regenerates
it from an AST.

**Text encoding and normalization:**
- UTF-8 only, strictly validated (no overlong forms, no surrogates). Non-UTF-8 files are rejected as
  `InvalidDocument` (controlled error, file not loaded).
- **Line endings in memory: always LF** (U+000A). On open, every CRLF and every lone CR is converted to
  LF, exactly as the editor's `ui::TextBuffer::normalize` does, so source offsets in the editor buffer
  and in `MarkdownTabState` are identical. The dominant line-ending style (CRLF only when CRLF
  terminators outnumber LF and lone-CR terminators) and the BOM are recorded and re-applied on save.
- UTF-8 BOM is detected on open and re-emitted on save if it was present.
- Mixed line endings (CRLF mixed with LF or lone CR in the same file) are normalized to the detected
  dominant style on save; a flag records this for UI feedback.

**Editing model:**
- All mutations go through a single `CommandStack` (depth 100, same infrastructure as PDF page editing).
- One command type: `TextEditCommand(offset, removeLength, insert)` — replaces a byte range with new text.
- Commands are fully reversible and coalesce by edit shape, not by time: the source editor marks an edit
  as mergeable when it continues the previous typing / backspace / delete-forward run at the caret
  (a word typed after whitespace starts a new undo step), and `TextEditCommand::tryMerge` folds it into
  the previous command when the ranges are adjacent. There is no time window.
- A command bumps a monotonic `revision()` counter; caches (parser, layout) key on revision.

**Dirty tracking:**
- Every command and undo/redo has a `stateId()` (like PDF page editing, ADR-0009).
- `isDirty()` = `stateId() != savedStateId()`. Undoing back to the saved state is clean again.
- Saving sets `savedStateId_` without clearing the undo stack (undo stays available).
- A failed save keeps the document dirty (the source stays in memory); editing is unlocked again so the
  user can keep working or retry.

**Editing lock:**
- While a save is in flight, `execute/undo/redo` are refused (the shell reports this).
- View, search, and selection keep working.
- Save unlocks on completion (success or failure), see `FileController::handleMarkdownSaveCompleted`.

**TextBuffer contract:**
- `MarkdownTabState::applyEdit(offset, removeLength, insert)` is the single mutation point.
- It returns `false` (and leaves the source untouched) for out-of-range offsets and for offsets that
  fall inside a multi-byte UTF-8 sequence.
- `onChanged()` callback fires after every mutation, so views key their caches on `revision()`.

**Parser invocation:**
- The shell is responsible for parsing. It does NOT happen on every keystroke.
- Parsing runs asynchronously on a worker (see ADR-0022 for the live-preview coordination).
- `MarkdownTabState` remains unaware of parsing; it only stores text.

**Deferred on save:**
- Undo history survives save (ADR-0009 phase 3 limitation noted in ARCHITECTURE.md is specific to PDF;
  Markdown's text-only model makes this simpler).
- Future: compressed checkpoints in the undo stack to save space.

## Consequences

- **No round-trip lossiness.** Source text is never rewritten; a user's formatting (line length, spacing,
  marker style) is preserved.
- **Simple semantics.** Line endings and BOMs are the shell's concern, not the editor's; the editor
  works in LF and UTF-8.
- **Undo/redo work exactly like PDF editing.** The same `CommandStack`, `stateId`, and dirty-tracking
  primitives apply.
- **File I/O is straightforward.** Read: decode and validate UTF-8; write: encode with the saved
  line ending and BOM.
- **No AST regeneration.** Parsing is stateless and deterministic (ADR-0019); the same source always
  produces the same model, so caching and diffing are predictable.

## Rejected alternatives

- **Store AST + regenerate source on save**: defeats the point of having authoritative source. A single
  lossless AST node (e.g., markdown syntax variant, spacing) would require tracking; regeneration bugs
  would silently corrupt formatting.
- **Preserve exact source bytes with an edit log**: more complex; a direct source + command stack is
  simpler and reuses PDF's proven undo/redo model.
- **Strip line endings and BOM on load, discard on save**: loses user intent; files with CRLF should
  stay CRLF.
