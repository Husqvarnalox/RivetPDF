# ADR-0031: WYSIWYG editing in the Rendered view

Status: Accepted

Date: 2026-10-16

## Context

Markdown tabs edit the source text (ADR-0021: the source is the only
authoritative state) and show a live preview (ADR-0022). Users also want to
type directly in the rendered document. The layout engine already carries a
source map from laid-out runs back to source byte ranges
(`MarkdownSourceMap`).

## Decision

1. **The source stays authoritative.** Every WYSIWYG gesture is translated
   into a source edit (`MarkdownTabState` commands on the one
   `CommandStack`), then re-parsed and re-laid-out like any other edit. There
   is no second document model.
2. **Caret and selection live in source offsets.** The Rendered view keeps a
   caret as a source byte offset and maps it to a layout position through the
   source map (and back for hit testing). Positions inside markup that is not
   rendered (e.g. `**`, link targets) snap to the nearest rendered text
   position. Up/Down move by visual line using the layout's line geometry.
3. **Typing** inserts at the caret's source offset inside the containing
   inline text run (escaped as needed: `*`, `_`, `` ` ``, `[`, `]`, `\`, `#`
   at line start, etc., so typed characters never change structure).
   Backspace/Delete remove one rendered character (a grapheme in source);
   deleting across a block boundary joins blocks by editing the source line
   break. Enter splits a paragraph (blank line), continues a list item
   (`- ` / `1. ` with renumbering left to the renderer) or exits an empty
   item; Shift+Enter inserts a hard break.
4. **Formatting commands** (Cmd/Ctrl+B, I, K for link, E for code, heading
   level 0-6, bullet/numbered/task list, quote) wrap or unwrap the selection's
   source range with the corresponding markup, chosen to round-trip through
   MD4C. Each is one undo step.
5. **Not editable in place** (read-only islands, edited in the Source view):
   tables, code blocks' fences/info string (their content is editable as
   plain text), raw HTML, images (selectable; Delete removes them), and
   Mermaid/math blocks (MD6). The caret skips over them.
6. **Latency.** Edits update the layout incrementally per block where the
   parse result permits (ADR-0022 async parse); the caret position is
   computed from the next layout, and until it lands the old layout shows the
   caret at the mapped position with the typed text drawn optimistically.

## Consequences

- Source formatting is preserved byte-for-byte outside the edited range.
- Some structures (tables, HTML) need the Source view.

## Implementation notes (MD5)

- **Provenance.** The parser records `TextSpan`s for every Text/Code inline
  and code-block text (MD4C's synthesized newlines in verbatim blocks map to
  the next source `\n`); spans whose text differs from the source (entities,
  escapes, soft breaks, tabs) are atomic, so the caret never lands inside
  them. The layout aggregates spans per run (`LayoutRun::spanBegin/spanEnd`).
- **Caret.** `markdown::CaretMap` (src/markdown/MarkdownCaret.*) is built
  from a layout; positions are source offsets in `MarkdownPreviewView`, and
  the run-start stop that coincides with the previous run's end is dropped,
  so typing at a style boundary continues the preceding style. Graphemes
  follow a compact extend/ZWJ/regional-indicator rule (`Grapheme.hpp`).
- **Edits** are pure (`markdown::editing`, src/markdown/MarkdownEditing.*)
  and become one `TextEditCommand`; consecutive typing merges like the
  Source pane (a word typed after a space starts a new undo step). Format
  commands work identically on the Source editor's selection.
- **Islands.** `SpecialBlock {None, Mermaid, DisplayMath}` on `Block` and
  `LayoutBlock::special`: fenced code whose info word is `mermaid`, fenced
  ```` ```math ````, or a paragraph wrapped in `$$ … $$` (converted to a
  code block with info `math` and the TeX as text). `isReadOnlyIsland()`
  covers these plus tables, raw HTML and image blocks; the caret map has no
  stops in them, Backspace/Delete next to an image selects it (a second press
  deletes it) and refuses to join into other islands. MD6 renderers paint
  island blocks by `special` without touching the editing code.
- **Latency.** Keys that need the new document/layout are queued and
  drained in `prepare()` once the layout of the current source exists;
  plain typing during a background parse is applied immediately and drawn
  optimistically over the stale layout.
- **Known limitations.** Escaping of optimistically typed text uses the
  snapshot's context; an empty paragraph cannot exist in Markdown, so Enter
  at a paragraph end leaves the caret on the blank line between blocks
  (drawn at its left edge) until text is typed; turning a list off merges
  its items' lines into one paragraph (toggling it back restores items per
  line); a leading typed space is inserted as `&#32;` so it survives.
