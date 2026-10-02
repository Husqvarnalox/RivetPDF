# ADR-0014: Page content object model, identity and capabilities

Status: Accepted (Phase 5)

Date: 2026-10-03

## Context

Phase 5 edits page CONTENT (text, images, vector paths), not just page
structure (ADR-0008) or annotations (ADR-0011). Constraints:

- PDFium is the only backend; its page-object handles (`FPDF_PAGEOBJECT`)
  belong to a loaded page of one `FPDF_DOCUMENT` and die with it. Live
  documents are shared with rendering and must never be mutated (ADR-0009).
- Identity must survive move/edit/undo/redo/page reorder/save and never be
  reused; an index in the page's object list is not an identity (it shifts on
  delete/insert), nor is a handle.
- Large documents: nothing may be extracted eagerly for every page.
- Every edit must be reflected identically on screen and in the saved file.

## Decision

### Model: source page + immutable edit description

A page's content is the SOURCE page (page `sourcePageIndex` of
`PageEntry::source`, as stored in its file) transformed by an immutable
`pdf::PdfPageContentEdits` value held by the page model
(`PageEntry::contentEdits`, null = untouched). The edit description is
portable data in the source page's user space (`src/pdf/PdfContent.hpp`):

- `PdfObjectEdit` per source object: remove, extra user-space transform
  (move/resize), image replacement;
- `PdfTextBlockEdit`: new text (Add Text) or the replacement of a set of
  existing text objects (`members`), with text, font reference, effective
  size, color, rigid placement, wrap width and line advance.

Commands swap the page's edit pointer (old <-> new), exactly like
annotation states (ADR-0011): undo/redo are pointer swaps, edits are never
lost by cache eviction, duplicating a page shares the (immutable) edits.
Each content command mints a new `contentRevision` and `rasterRevision`
for the edited pages only, which invalidates that page's tiles, text cache,
search results and links, and nothing else.

### Backend contract

`PdfDocument::pageContent(pageIndex, edits)` lists the TOP-LEVEL objects of
the page with `edits` applied (`PdfPageContent`): type, origin, z-index,
matrix, bounds, rotated quad, clip/marked-content flags, text + font
information + effective size + render mode, colors, image pixel size, path
segment count, form child count. Render and text extraction take the same
edits (`renderPage(..., edits, ...)`, `textPage(..., edits)`). Inside
`src/pdf/pdfium` one function applies an edit description to an
`FPDF_PAGE`; it is used both by the per-page materializer (display) and by
the assembly (save) — ADR-0017.

Objects inside Form XObjects are not listed individually: a top-level form
is one object (movable/deletable, never flattened, its inside read-only).

### Coordinate spaces

The backend speaks PDF user space only. The editor maps to display space
(points, origin top-left of the displayed page, y-down, view rotation and
crop applied) through the existing centralized `PdfPageView` mapping
(`userToDisplay`, `displayToUser`, `userToDisplayMatrix`). Object matrices
are composed with that matrix; pointer deltas are mapped back with the
inverse linear part. Tests cover translation, scale, rotation, skew, page
/Rotate, CropBox offsets and nested transforms.

### Identity

`core::ObjectId` (StrongId), minted by the page model, never reused:

- existing objects: registry `(PageId, source object index)` -> ObjectId,
  minted on first sight (ContentService, mirrors AnnotationService);
- objects created by a text block edit: the block's `tag` IS the ObjectId
  value of the block; the first created line of a new block is registered
  under it;
- a text block's identity is the ObjectId of its first member (edited
  blocks keep it; their reused members keep their own ids);
- edits never change a reused object's origin, so moved/edited objects keep
  their ObjectId; delete + undo restores the same pointer and hence the
  same ids; a duplicated page has a new PageId and therefore new ids;
- after a save the writer reports every written object's origin
  (`PdfAssembledPageContent`), and the registry is re-keyed to the new
  source indices (ADR-0017). Text-block identity across a save is exact for
  the block's first object and best-effort for the grouping (blocks are
  reconstructed from the saved content).

### Lazy extraction and caching

`ContentService` (rivet_editor) loads `pageContent` on its own
`SerialExecutor` only for pages the content tools look at (visible page
while a content tool is active), keeps an LRU of extracted pages keyed by
(source document, page, edits pointer), drops results that belong to a
previous base or a dead document, and resolves display-space views +
reconstructed text blocks + capabilities per page on the main thread.
Eviction loses nothing: the edits live in the page model.

### Capabilities

Per object / text block, a class and a human-readable reason:

| Class | Meaning | Typical causes |
| --- | --- | --- |
| FullyEditable | text can be retyped in its own font; move, delete | embedded non-subset or standard-14 font with Unicode mapping |
| Replaceable | text can be retyped, but the block will be re-set in a bundled substitute font if its font cannot encode the new text | subset-embedded or non-embedded non-standard font |
| MoveOnly | move/delete only | Type3 font, unmappable glyphs, skewed or non-uniform text matrix, text with a clip path, top-level Form XObject, shading |
| ReadOnly | nothing | regeneration probe failed for the page (ADR-0017), page truncated (> 20000 objects), invisible text (render mode 3, OCR layers), objects of unknown type |

Images: move/resize/replace/delete unless the page is read-only. Paths:
move/resize/delete (geometric scaling of the path's matrix; stroke widths
scale with it). Annotations (including widgets and links) are not page content
objects and are never listed, selected or deleted by content tools.

### Security

No JavaScript, no actions. Limits (`PdfContent.hpp`): objects per page,
text block length, blocks per page, font size range, replacement image side
/ pixel count / encoded size, all checked with overflow-safe arithmetic
before any allocation. Edits are validated (`pdf::validate`) before they are
applied. Logs never contain document text, image data or passwords.

## Consequences

- One edit description drives display and save (WYSIWYG by construction).
- The page model stays the single source of truth; dirty tracking stays the
  CommandStack checkpoint.
- Objects inside forms, and pages whose content PDFium cannot regenerate
  faithfully, are read-only — honest instead of lossy.
- Annotations sit in their own layer and are not moved with content (they
  stay where they are; documented, ADR-0015).
