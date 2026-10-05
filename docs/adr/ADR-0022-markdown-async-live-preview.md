# ADR-0022: Asynchronous live Markdown preview with debouncing

Status: Accepted

Date: 2026-10-05

## Context

Parsing and laying out Markdown every keystroke would block the main thread and hurt responsiveness.
A user typing fast should see text appear on screen instantly in the source editor, even if the
preview lags.

The preview must update asynchronously without dropping edits or showing stale layouts.

## Decision

**Live preview architecture:**

1. **Debounced parsing**: After each text mutation, `MarkdownParseCoordinator` arms a debounce wait
   (150 ms trailing, capped at 800 ms since the first pending edit). The wait is a worker job (it
   sleeps in short steps and checks cancellation), not a timer; when it ends it posts a wake-up to the
   main thread. If edits arrived meanwhile, it waits again for the remainder; once the user pauses
   (or the cap is reached) a parse is started.

2. **Snapshot and revision**: The parse job captures an immutable snapshot of the current source text
   and a monotonic `revision` ID. Parsing is a pure function of the snapshot; if the source changes
   while parsing is in flight, a later parse is started with a fresh snapshot and higher revision.
   Sources of at most 64 KB (`synchronousParseBytes`) are parsed synchronously on the main thread;
   larger ones are parsed on a worker.

3. **Main-thread delivery**: The parser runs on a worker; results are marshaled back to the main thread
   via `IMainThreadDispatcher::post()`. The view receives the parsed `MarkdownDocument` and the
   revision.

4. **Stale discard**: Several parses can be in flight at once. A result is applied only if its epoch
   (bumped on bind/unbind) is current, its revision equals the newest requested revision, and it is not
   older than the last applied one; anything else is dropped.

5. **Lifetime safety**: The parse job holds an `AsyncScope` token. When the tab closes or the shell
   tears down, the scope is cancelled. Workers poll the cancellation flag; cancelled jobs abort before
   returning results (no orphaned callbacks).

6. **View update**: When a fresh layout arrives, the preview view:
   - Keeps the scroll position by anchoring to a block ID and offset (if possible; relayout that
     changes line breaks drops selection).
   - Relayouts if the viewport width changed; reuses text metrics from `MeasureCache`. Layout runs on
     the main thread, lazily during paint (`ensureLayout`).
   - Repaints only visible blocks.

**Tuning:**
- Debounce window: 150 ms trailing (800 ms cap) (user perceived as "instant" after pausing).
- Relayout debounce during live resize: if a relayout took >12 ms, resizes space out relayouts by ≥120 ms
  (smooth drag without stuttering).

**Deferred features:**
- Collaborative editing / remote sync: not yet supported; snapshot-based parsing is single-device only.
- Incremental parsing: whole-document reparsing is fast for typical Markdown files; incremental parsing
  can be added later if needed (e.g., for >10 MiB files).

## Consequences

- **Responsive editing.** Text appears on screen immediately; the preview updates a few hundred
  milliseconds later when typing pauses.
- **Bounded blocking.** Large sources are parsed on workers; small ones (up to 64 KB) are parsed on the
  main thread, and layout always runs on the main thread during paint.
- **Predictable ordering.** A strictly monotonic revision ensures the view never shows an old parse
  result after a new one arrives (even if workers complete out of order).
- **Safe cancellation.** Closing a tab cancels its parse and debounce jobs; no zombie parses try to update
  a dead view.
- **Cache reuse.** `MeasureCache` persists across relayouts (same width) and across edits (same style,
  word), making repeated relayouts efficient.
- **Simple model.** No change tracking, diffs, or incremental updates; whole-document snapshots are
  sufficient for files up to the parse limit (64 MiB, ADR-0019).

## Rejected alternatives

- **Synchronous parse on every keystroke**: unresponsive, especially for large files or slow machines.
- **Background thread per document**: wastes OS threads; the shared `TaskScheduler` (Section 5,
  ARCHITECTURE.md) handles work distribution efficiently.
- **Incremental parsing with change events**: more complex; requires tracking edits and invalidating
  affected regions. Whole-document parsing is simpler and fast enough.
- **Push parsing results without revision guard**: risks showing stale output if the user keeps typing.
  Revision sequencing ensures freshness.
- **Store old parses and diff them**: adds memory overhead; snapshot-based approach is stateless and
  garbage-collects naturally.
