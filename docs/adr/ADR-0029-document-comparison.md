# ADR-0029: Document comparison

Status: Accepted

Date: 2026-10-16

## Decision

"Compare With…" opens a comparison tab showing two documents side by side
with synchronized scrolling (two `PdfViewport`s, one `ViewerState` each,
page-index locked). Text of each page pair (i, i) is extracted through the
existing `TextService`, tokenized into words with their char boxes, and
diffed with Myers' O(ND) algorithm (portable `editor/TextDiff`, bounded:
20000 words per page, otherwise per-line fallback). Deletions are
highlighted red in the left document, insertions green in the right; a
change list in the sidebar navigates between differences. Extra pages are
reported as inserted/deleted pages. Diffing runs on workers with
cancellation; results arrive on the main thread with generation guards.
No visual (raster) diff in this version.

## Consequences

Deviations from the decision text, made while implementing it:

- **Saved files, not live sessions.** Both documents are opened from disk as
  secondary `DocumentSession`s owned by the comparison tab. The left side is
  therefore the active document as last saved; unsaved edits are not
  compared (the status bar says so). This keeps the comparison independent
  of the editing session (no locking, no snapshot of a mutating model) and
  lets the tab release the second document by dropping its state.
- **Change list inside the tab.** The change list is part of the comparison
  view (`ComparisonHostView`, reusing `OutlinePanel`) instead of a new
  `SidebarController` mode; the tab hides the PDF sidebar like a Markdown
  tab does. This avoids touching the sidebar that other work extends.
- **Extra diff bound.** Besides the 20000-word bound, `DiffLimits` caps the
  edit distance (default 3000) so a page of two unrelated texts cannot cost
  O(D^2) trace memory; beyond it the page falls back per line, then to one
  replaced region. The fallback used is reported (`approximatePages`).
- **Scroll sync, zoom not.** Scrolling is synchronized by page index and the
  fraction within the page; zoom stays independent per side (each opens
  fit-to-width). Pages the other document lacks leave it where it is.
- **No text selection or links** in the comparison viewports (the bridges
  only provide highlights).
- **Extra pages** are one change per run of trailing pages ("pages 4-5 only
  in the left document") and tint the whole page.
- **Passwords:** an encrypted document fails the comparison with a message;
  there is no password prompt in this tab.
- **Not exercised manually:** the UI was verified by automated tests only.
