# ADR-0017: Content regeneration, display materialization and save semantics

Status: Accepted (Phase 5)

Date: 2026-10-03

## Context

PDFium writes page content with `FPDFPage_GenerateContent`, which rewrites
every content stream that contains a modified ("dirty") object from the
parsed objects, and appends new objects in a new stream. Reading its
generator (`core/fpdfapi/edit/cpdf_pagecontentgenerator.cpp`, revision
40ecb4f) shows what a REWRITTEN stream loses:

- shading objects (`sh`), inline images, Type3-font text;
- fill/stroke colors outside DeviceGray/DeviceRGB (CMYK, ICC, Separation,
  Pattern);
- text state other than Tm/Tf/Tr/TJ (Tc, Tw, Tz, Ts);
- ExtGState entries other than ca/CA/BM (soft masks, ...);
- text clipping modes (path clips are kept, per object).

Untouched streams are copied verbatim; marked content is preserved; shared
streams/resources are cloned before modification.

## Decision

### Regeneration fidelity probe

Before a page's objects become editable, the backend probes the SOURCE page:
the page is imported into a scratch document from a private never-rendered
copy, its object count is checked against the page as stored (a second
private copy renders the reference: an import that drops unreadable
content would shift indices and is refused), then the imported page is
rendered at low resolution, every top-level object is marked dirty,
`GenerateContent` runs, the document is serialized, re-parsed and rendered
again, and object count/types/bounds and pixels are compared (exact match
required, small tolerance for anti-aliasing noise only). Any difference, or
any object type known to be lost (shading, inline image, Type3 text), makes
the page `regenerationSafe = false` with a short reason; its objects are
read-only. The probe runs once per (document, page) lazily on the content
service's worker and is cached. Conservative by design: if a full rewrite
is faithful, any partial rewrite is too.

### One apply function, two users

`applyContentEdits(FPDF_DOCUMENT, FPDF_PAGE, edits, fontCache)` in
`src/pdf/pdfium` collects the top-level object handles FIRST, then mutates:
removals, transforms, image replacements, text block edits (layout with the
real font advances), new objects appended with `FPDFPage_InsertObject`
(never inserted mid-list), then `FPDFPage_GenerateContent`. Because new
objects are only appended and modified objects keep their stream, the
in-memory object order equals the order after re-parsing; the function
returns the origin of every resulting object in that order.

- Display: a per-page materializer imports the source page from a private
  never-rendered copy of the source document into a scratch document,
  applies the edits, serializes and re-parses it (so the screen shows what
  will be saved), verifies the predicted object count, and renders / extracts
  text / lists objects from that page. Small LRU keyed by the edits pointer.
  The live document is never modified.
- Save: the assembly (ADR-0009/0012) applies the edits to each output page
  after its view and before its annotation edits, keeping `/Annots`
  untouched by content edits, and reports the origins per page
  (`PdfAssembledPageContent`). The whole assembly stays one gate
  acquisition.

### Save mode: full rewrite + atomic replace

Saving stays a full rewrite (`FPDF_SaveAsCopy` with `FPDF_NO_INCREMENTAL`)
into a temporary file atomically renamed over the target (ADR-0010).
Incremental update was considered: it would keep unchanged object numbers
but the pipeline already rewrites the document for page operations, PDFium
cannot express all page-tree changes incrementally, and atomic replacement
is required either way. Any content edit invalidates existing digital
signatures (signatures are out of scope until Phase 6; documented).

### Preservation

Kept on save: annotations (including links and form widgets), AcroForm,
outline, metadata, page labels, page properties (boxes, /Rotate as set by
the view), unknown objects in untouched streams, clipping of untouched and
regenerated objects (path clips), marked content. Content of pages whose
probe failed is never rewritten (they cannot be edited).

### Rebase after save

The writer's per-page origin report re-keys the ObjectId registry (source
index -> new index, created -> new index) and the rebased entries carry no
content edits (the saved file is the new source).

### Printing

Printing a document with content or annotation edits prints from a
temporary assembled copy (same writer), so it matches the saved file.

## Consequences

- Editing never silently loses content: unsafe pages are read-only.
- Display is the re-parsed result of the same writer code: WYSIWYG.
- Some pages (CMYK artwork, shadings, Type3) are not editable; the UI says
  why.
