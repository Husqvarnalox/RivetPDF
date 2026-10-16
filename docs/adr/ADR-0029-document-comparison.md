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
