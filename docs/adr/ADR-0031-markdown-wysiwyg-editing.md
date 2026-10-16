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
