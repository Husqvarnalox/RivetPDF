# ADR-0015: Text editing strategy

Status: Accepted (Phase 5)

Date: 2026-10-03

## Context

PDF text is a sequence of positioned glyph runs, not paragraphs. Editing
"a paragraph" needs (1) a conservative reconstruction of blocks from text
objects, (2) a way to write new text as REAL page content (never an overlay
that leaves the old glyphs underneath), (3) a deterministic layout that is
identical on screen and in the saved file, and (4) honest limits.

## Decision

### Text objects and blocks

PDFium creates one text object per text-showing operator. Rivet groups
consecutive (content-order) top-level text objects into lines and lines into
blocks (`editor/TextBlocks`), deterministically and conservatively:

- objects join a line when they share the rotation of their text matrix
  (within 0.5 degrees), have the same effective font size (within 5 %),
  their baselines are within 25 % of the size of each other, and the
  horizontal gap along the baseline is below 1.5 x the size (and not
  negative by more than 0.5 x the size);
- lines join a block when they follow each other in content order, share
  rotation and size, are left-aligned within 1 x size, and the baseline step
  is between 0.8 x and 2.2 x the size (consistent within 25 % across the
  block);
- objects produced by a text block edit (`blockTag != 0`) always form
  exactly their edit's block (authoritative grouping);
- anything else (invisible text, Type3, unmappable glyphs, clipped text)
  stays a single-object block with its capability class (ADR-0014).

Content order (not geometric sorting) keeps the result deterministic and
avoids merging columns.

### Editing existing text: replace in place, as content

Committing an edit of block B produces a `PdfTextBlockEdit` with
`members` = B's objects, the new text, B's font (`FromObject`, member 0),
effective size, fill color, a rigid placement at member 0's baseline origin
and rotation, wrap width = B's width (multi-line blocks) or none (single
line), line advance = B's measured baseline step (or 1.2 x size).

The backend lays the text out (below) and writes line k into member k with
`FPDFText_SetText` (same font, same object: its z-position, render mode,
marked content and graphics state are kept), moves it to line k's origin
and sets the size; members beyond the line count are removed; extra lines
become new text objects cloned from member 0's font and color, appended on
top of the page. If the font cannot encode the whole new text (round trip
of `FPDFText_SetText` + `FPDFTextObj_GetText` differs, Type3, or glyphs
missing), ALL members are removed and every line is a new object in the
bundled fallback font chosen by ADR-0016 (`fontSubstituted` is reported and
the UI says so). The old glyphs never remain under the new text. Text re-set in the block's own full embedded font is dry-run through the backend at command build time (`PdfDocument::checkContentEdits`), so an edit the font cannot write is refused before it reaches the command stack.

Editing an already edited block updates the same `PdfTextBlockEdit` (same
tag). Moving an unedited block transforms its members (`PdfObjectEdit`);
moving an edited block changes its placement.

### Add Text

Add Text creates a block edit without members: bundled font (Sans/Serif/
Mono, Regular/Bold), size, color, placement at the click point (first
baseline one ascent below it), wrap width 0 (or the dragged box width).
Created objects are appended on top of the page.

### Layout (`pdf::layoutTextBlock`)

Deterministic greedy whitespace wrapping with the font's real advances
(supplied by the backend from the font that will actually be written), hard
breaks on CR/LF, U+FFFD for ill-formed UTF-8, long words broken between code
points but never before a combining mark. No hyphenation, kerning, bidi,
justification or complex shaping (Arabic, Indic, ... are not supported for
editing; their text is still rendered and preserved when not edited).
Alignment is left only.

### Overflow policy

Nothing is ever clipped or silently dropped. A block grows downwards as far
as its text needs; when the edited block is taller than the original, the
editor reports "text extends below the original block" in the status bar
and keeps the outline of the new extent. Text outside the page is still
written (it is the user's content) and reported likewise.

### Geometric scaling vs font size

Text blocks have no scale handles: dragging a block's side handle changes
its wrap width (reflow); font size is a property (toolbar). Images are
scaled geometrically (corner handles, Shift keeps aspect ratio).

### Inline editor

An on-page editor (Rivet `TextArea`) is positioned over the block in
display space with an opaque background while editing: caret, selection,
Backspace/Delete, arrows, Home/End, Cmd+A, Return for new lines, UTF-8
(Latin, Cyrillic, Greek, ...), multi-line. Its rendering is the platform's
approximation; the committed result is the real PDF render. Esc cancels,
clicking outside or Cmd+Return commits. One commit = one undo step.
Save/close/quit commit pending text first (`commitPendingEdits`).

### Character indices

Edits mint a new `contentRevision`; text selection, search hits and link
caches keyed by the old revision are dropped, so no stale character index
survives an edit.

### Annotations

Annotations are independent of content and do not move with edited or
moved text (markup over edited text keeps its quads). Documented.

## Consequences

- Real content edits, deterministic layout, identical screen/save result.
- Conservative grouping sometimes yields smaller blocks than a human would
  draw; it never merges unrelated columns.
- Per-glyph styling inside a block (mixed bold/italic runs) is replaced by
  the block's first style on edit (documented limitation).
