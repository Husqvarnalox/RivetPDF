# ADR-0011: Annotation model, identity and rendering split

Status: Accepted (integrated with Phase 4; manual QA pass pending)

Date: 2026-10-02

## Context

Phase 4 adds annotations (highlight, underline, strikeout, sticky notes,
ink, rectangle/ellipse/line/arrow shapes, stamps): view existing ones,
create, select, restyle, move/resize, delete, undo/redo, save, reopen.

Constraints that shape the model:

- Annotations need an identity that survives move/resize/restyle,
  undo/redo, page reorder and save, and is never reused. A position in the
  page's `/Annots` array is not an identity (it shifts when an entry is
  removed), nor is an engine handle or a vector index.
- Page operations (ADR-0008) must keep working: reorder, rotate, crop,
  duplicate, delete and import address pages by `PageId`, and rotate/crop
  change only the page's view, never its content.
- PDFium can render existing annotations (with their own appearance
  streams) but its write API cannot express every edit, and the live
  documents must never be mutated (ADR-0009).
- Tiles are cached by `(PageId, revision)`; editing an annotation must not
  flush the tile cache nor show a stale appearance.
- Large documents: nothing may be loaded eagerly for every page.

## Decision

### Identity

`core::AnnotationId` (a `StrongId`) is minted by the editor and never
reused within a session:

- Rivet-created annotations get an id when the creating command is built,
  so redo recreates the same id.
- Existing annotations get an id from a registry keyed by
  `(PageId, /Annots index of the page's current source)`. The id is minted
  on first sight and kept for the session. Deleted ids are never reissued,
  and undo restores the same ids.
- Move, resize, restyle and contents edits keep the id. A duplicated page
  gets fresh ids for all its annotations. An imported page gets fresh ids
  (new `PageId`).
- After a save (rebase, ADR-0009) ids are carried over. The writer reports
  where each written annotation landed in the new file, and the registry is
  re-keyed accordingly (ADR-0012).

### Geometry storage

Annotation data (`pdf::PdfAnnotationData`) is stored in the PDF user space
of the page's source, like the crop box (ADR-0008). Rotate and crop then
never rewrite annotations. Everything above the editor sees only
display-space geometry of the page's current view (points, top-left
origin, y-down, rotation applied). All conversions go through
`pdf/PdfPageGeometry` (`userToDisplay`, `displayToUser`, matrices), wrapped
by the editor's annotation geometry helpers. No other code converts.

### Page state

`PageEntry` gains:

- `annotations`: a `shared_ptr<const PageAnnotationState>`, where null means
  the source page's annotations are untouched. The state holds:
  - `suppressed`: sorted `/Annots` indices of the source page that the
    raster must not draw, either because they were deleted or because they
    are superseded by a Rivet-drawn version. A suppressed annotation's
    `/Popup` entry is suppressed with it.
  - `overlay`: the annotations Rivet draws itself, in z-order, each with
    `{AnnotationId, PdfAnnotationData, optional fileIndex}`. `fileIndex` is
    set when the annotation already exists in the source file at that index
    (after a save), and that index is then also suppressed.
- `rasterRevision`: identifies what the page raster shows (view plus
  suppressed set). It is minted from the same model-wide counter as
  `contentRevision`. It changes on rotate/crop together with
  `contentRevision`, and on its own when the suppressed set changes; undo
  and redo restore it. Tiles are keyed by `rasterRevision`. Text and links
  stay keyed by `contentRevision`, so annotation edits never invalidate
  text extraction or the text selection.

States are immutable and shared. Commands swap state pointers, so undo is
O(changed pages) and snapshots stay cheap.

### Existing annotations: lazy, per page, read from the file

`PdfDocument::annotations(pageIndex)` returns the page's `/Annots` as
Rivet-owned data, read from the document as stored in its file. The PDFium
backend reads from a private document instance over the same file source,
never from the live (rendered) one, whose dictionaries PDFium mutates while
rendering (generated appearances, Text `/Rect` normalization). The editor's
annotation service loads pages lazily on a worker, under the PDFium call
gate. Results are delivered on the main thread with an alive guard and
generation checks. They are cached per `(source document, page index)` in a
bounded LRU.

The cache holds only loaded originals. Edits live in the page model, so
eviction never loses an edit.

An existing annotation is editable when Rivet understands its kind and
could read it faithfully. The editable kinds are:

- Highlight, Underline, StrikeOut.
- Text (note).
- Ink.
- Square and Circle.
- A Stamp with a standard name.
- Rivet's own Ink-encoded Line and Arrow.

Everything else is preserved and drawn by the raster, but is not selectable
(third-party Line, FreeText, Link, Widget, Popup, file attachments,
over-limit data, custom stamps).

### Rendering split

- Untouched originals, including every unsupported kind, are drawn by the
  PDFium raster (`FPDF_ANNOT`) with their own appearance streams.
- Created and edited annotations are drawn by Rivet as a vector overlay
  above the tiles. The overlay uses the same appearance description
  (`pdf::buildAppearance`) that the writer turns into the saved `/AP`, so
  what is edited is what gets saved.
- Suppressed originals are hidden from the raster. The view-aware render
  call takes a list of `/Annots` indices. For the duration of one gate
  acquisition, the backend sets the Hidden bit on them, renders, and
  restores the previous `/F`. PDFium reads `/F` per render, so this is
  exact, and nothing is ever drawn twice.
- After a save, Rivet-written annotations stay overlay-drawn (`fileIndex`
  set, index suppressed). The raster of the rebased page is then identical
  to the one before the save, so tiles stay valid: no flush, no flash.

### Page operations

- Reorder moves entries, and their annotations go with them.
- Rotate and crop change the view only. Annotations are re-projected for
  display. Crop policy: annotations outside the new crop box are kept,
  clipped and invisible, and reappear when the crop is reset. Nothing is
  silently destroyed.
- Duplicate copies the state with fresh ids for overlay annotations, and
  the registry mints fresh ids for originals under the new `PageId`.
- Delete removes the entry. Undo restores the entry, and with it the state,
  ids and revisions.
- Import gives new entries with a null state. Their originals are carried by
  the page import.

### Limits

These limits are untrusted-input bounds (`pdf/PdfAnnotation.hpp`):

- Items read per page.
- Quads per markup.
- Ink strokes and points.
- Contents and author length.

Over-limit annotations are treated as opaque. The editor refuses to create
past the limits. Contents are never logged.

## Consequences

- Saving rewrites every edited annotation from Rivet's model (ADR-0012).
  Custom appearances of edited third-party annotations are replaced by
  Rivet's.
- An annotation-only edit changes one page's `rasterRevision` (when it
  touches originals) or nothing raster-related (when it only touches
  Rivet-drawn annotations). The overlay repaints on the model change.
- The overlay draws with CoreGraphics while the raster uses PDFium. Minor
  anti-aliasing differences between an edited annotation on screen and the
  same annotation after reopen are expected.
- Reading goes through PDFium's public annotation API, not a parser of our
  own, so some malformed values are coerced the way PDFium coerces them: a
  non-numeric `/QuadPoints` entry reads as 0, and a short `/InkList` entry
  becomes a short stroke. Such annotations stay editable with the coerced
  geometry. Structural problems (non-dictionary entries, dangling
  references, missing `/Rect`, over-limit data) make an annotation opaque
  or skip it, without failing the page.
- Ink reduction (Ramer–Douglas–Peucker) runs in windows of 1024 points. The
  tolerance guarantee still holds, and adversarial strokes such as zigzags
  stay near-linear instead of quadratic.
