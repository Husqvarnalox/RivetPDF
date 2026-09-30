# ADR-0008: Page model with stable page identity and assembled saves

Status: Accepted

Date: 2026-09-29

## Context

Phase 3 adds page editing: reorder, delete, rotate, crop, duplicate and
import/merge. Two constraints shape the design:

- PDFium page-tree edits are not transactional (`FPDF_MovePages` may leave
  the document "in an indeterminate state" on failure, and
  `FPDF_ImportPagesByIndex` inserts before copying), and the live document
  is what the viewer is rendering from on other threads. Editing PDFium's
  own document in place would make undo, cancellation and concurrent
  rendering unsafe.
- Everything keyed by "page" (tiles, text pages, links, thumbnails, the
  page selection, search hits, scroll anchors) must survive reorders and
  unrelated edits without being recomputed or re-pointed.

Page *index* is the wrong key (it changes on every move/delete/insert) and
so is the backend page index (pages of several source documents coexist).

## Decision

### Editor-level page model over immutable sources

`editor::PageModel` (src/editor) is the session-owned, mutable, ordered list
of pages. The opened PDFs are never mutated. Each `PageEntry` is:

- `id` (`core::PageId`) - the stable identity;
- `source` (`shared_ptr<pdf::PdfDocument>`) + `sourcePageIndex` - where the
  page comes from (the base document or an imported one);
- `view` (`pdf::PdfPageView`: absolute rotation + crop box in the source
  page's user space) - how it is presented;
- `mediaBox` / `nativeView` - the source page's own metadata, kept so crops
  can be validated and reset without a backend call;
- `contentRevision` - identity of the page's content as presented.

The model is main-thread owned and never empty. Every mutation is
transactional (validated first, applied all-or-nothing, O(N), one publish).
After each mutation it publishes an immutable `PageModelSnapshot` and
invokes the change observer synchronously with a `PageModelChange` (the diff
of previous/current snapshots: `removed`, `added`, `contentChanged`,
`orderChanged`). Worker jobs (render, text, search, links, copy, save,
extract) capture a snapshot on the main thread and never read live session
state; the snapshot keeps every source document alive through its entries.

### PageId semantics

`PageId` values are minted by the model's `IdGenerator<PageIdTag>` and are
**never reused**:

| Operation | Effect on ids |
| --- | --- |
| Move / reorder | unchanged |
| Rotate / crop / reset crop | unchanged (only `view` and `contentRevision` change) |
| Duplicate | the copy gets a **new** id, inserted directly after its original with the original's current view |
| Insert / import / merge | every inserted page gets a **new** id and its native view |
| Delete | ids are retired, never reissued |
| Undo | restores the **original** ids, positions, views and revisions; redo re-applies the same result and never mints (the ids created by the first `execute` are replayed) |
| Rebase after save | one-for-one, same order, **same ids** |

Because ids are stable, `PageSelection` (set of ids + active page + range
anchor) survives reorders untouched and needs a policy only for removals
(`PageSelection::applyChange`).

`contentRevision` is minted from a model-wide monotonic counter by rotate
and crop, and undo/redo restore the exact `(view, contentRevision)` pair they
replaced. A `(PageId, contentRevision)` pair therefore denotes exactly one
view forever. `render::TileKey` carries `contentRevision`, and text pages
and links are cached by the same pair, so reorder/delete/duplicate
invalidate nothing and rotate/crop invalidate only the affected page; an
undone rotation hits the cache again. `DocumentSession::revision()` (the
tile-cache epoch) is bumped only by `markModified()` for changes *outside*
the page model; page edits do not touch it.

### Commands

All page operations are `PageCommand`s (`MovePagesCommand`,
`DeletePagesCommand`, `RotatePagesCommand`, `DuplicatePagesCommand`,
`InsertPagesCommand`, `CropPagesCommand`) run by the session's single
`CommandStack`. A multi-page operation is one command and one undo step,
applied through one model mutation (one snapshot, one notification). See
ARCHITECTURE.md section 10 for undo/dirty semantics.

### Assembly: materializing the model into a PDF

Saving and extracting convert a snapshot to a `pdf::PdfAssemblyRequest`
(`PageModelSnapshot::toAssemblyRequest`) and call
`PdfEngine::assembleDocument(request, IPdfByteSink&)`:

- **`PreserveBase` (Save)**: document-level structure (outline, metadata,
  forms, page-label tree, encryption) comes from the base document; its
  pages are reused in place where possible.
- **`Fresh` (Extract)**: a brand-new document of the listed pages only; no
  document-level structure is carried over.

The PDFium implementation (`src/pdf/pdfium/PdfiumAssembly.cpp`; `FPDF_*`
stays confined there per ADR-0003) never edits the live documents. It works
on a private *working* document that is closed on every path:

1. PreserveBase: `FPDF_LoadCustomDocument` over the base's shared
   `PdfiumFileSource` (an open descriptor, so the working copy reads exactly
   the bytes the live document was opened from even if the path was replaced
   meanwhile) with the stored password. Fresh: `FPDF_CreateNewDocument`.
2. The first occurrence of each base page is reused in place (so outline and
   link destinations, form fields and the structure tree survive); every
   other page (duplicates, pages of other documents) is appended with
   `FPDF_ImportPagesByIndex`, one call per source document.
3. `FPDF_MovePages` brings the final order to the front; `FPDFPage_Delete`
   removes leftovers from the end backwards.
4. `FPDFPage_SetRotation` / `FPDFPage_SetCropBox` make each page's effective
   `/Rotate` and `/CropBox` match its requested view; the result is re-read
   and verified.
5. `FPDF_SaveAsCopy(..., FPDF_NO_INCREMENTAL)` streams into the sink through
   an `FPDF_FILEWRITE` adapter that records the first sink failure and
   refuses later blocks (PDFium ignores a failure of the final flush).

The whole assembly runs inside one acquisition of the process-wide PDFium
call gate (ADR-0006); the sink runs under it and must not call back into the
PDF layer. A non-incremental save writes only objects reachable from the
trailer, so a deleted page's content is dropped (new objects created by
imports are always written, which is why no page is imported and then
deleted). Known limits are listed in the `PdfiumAssembly.cpp` header comment
(page labels stay attached to positions, dangling destinations to deleted
pages, /Producer rewrite on imports, non-deterministic output).

Page labels are only meaningful while the model is the identity over the base
pages (`PageModelSnapshot::isIdentityOrder`); otherwise the UI falls back to
positional numbers. Destinations (outline, links) resolve to the *first*
entry presenting the source page, so a duplicate never steals them, and to
nothing once the page is deleted.

## Consequences

- Undo/redo, cancellation and rendering never contend with PDFium edits:
  editing is pure model manipulation; PDFium mutation is confined to a
  throwaway working document during save/extract.
- Caches are keyed by stable identity, so most edits are free for the
  renderer; correctness of that relies on the `(PageId, contentRevision)`
  invariant above.
- Every save rewrites the file (non-incremental); see ADR-0009/ADR-0010.
- The model holds references to imported documents for as long as any entry,
  undo command or in-flight job needs them.
- Content editing (text/objects) is not covered; it is expected to build on
  the same command foundation.
