# ADR-0026: True redaction through the content pipeline

Status: Accepted

Date: 2026-10-16

## Context

Covering text with a black rectangle is not redaction: the text stays
extractable. Rivet already rewrites page content through
`PdfPageContentEdits` and the regeneration probe (ADR-0017).

## Decision

1. **Mark, review, apply.** A Redact tool marks rectangles (drag) or text
   (from the selection; one rect per line quad). Marks are a per-page
   immutable list on `PageEntry` (`redactionMarks`), drawn as red outlines,
   undoable, and do nothing to content until "Apply Redactions", which is
   one command converting all marks into content edits and clearing them.
   Apply asks for confirmation (irreversible after save).
2. **Content edits gain `redactions`.** `PdfPageContentEdits::redactions`
   = list of rects in page user space plus fill color (default black).
   `applyContentEdits` (one function for display and save) applies them
   after object edits:
   - text objects: glyphs whose box intersects a rect are removed. An
     object with surviving glyphs is replaced by one new text object per
     surviving contiguous run, same font handle, size, matrix and render
     mode, positioned at the run's original origin (char codes copied, no
     re-encoding). If the font cannot be reused, the whole object is
     removed (over-redaction is acceptable; under-redaction is not);
   - images: intersecting pixels are filled with the redaction color in
     the decoded bitmap and the image is re-embedded (removes the pixels,
     not just covers them); images whose decode fails are removed;
   - paths and shadings: removed when their bounds intersect;
   - form XObjects intersecting: removed whole;
   - an opaque filled rectangle in the fill color is appended per rect.
   Annotations (including form widgets and links) whose rect intersects are
   removed through the annotation edits of the same page.
3. Pages refused by the regeneration probe cannot be redacted (the tool
   shows why) - never a partial redaction. Text extraction of a redacted
   page is verified in tests to contain none of the removed characters.
4. Metadata: Apply offers "Also remove document metadata" (Info dict and
   XMP) applied at save.

## Consequences

- Over-redaction of partially overlapping glyph runs in unusual fonts.
- Redaction lands only on save; until then undo restores everything.

### Implementation notes (2026-10-16)

Where PDFium's public API cannot do what the decision describes, the
implementation removes more, never less:

- **Char codes are not readable.** PDFium has no API returning a text
  object's character codes, so a surviving run is re-set from its extracted
  Unicode with `FPDFText_SetText` in the same font handle, then read back
  through a fresh text page; a run that does not read back identically, a
  surviving glyph without a Unicode mapping, a Type 3 font, a clipped text
  object or render modes 4-7 remove the whole object. Glyphs PDFium's text
  page drops (e.g. duplicated "fake bold" glyphs) are not rewritten.
  Character spacing, word spacing, horizontal scaling and rise are not
  readable either: a rewritten run starts at its original first-glyph
  origin but its later glyphs may drift slightly.
- **Glyph hit test.** A glyph is removed when its tight or its loose box
  intersects an area. Any text object whose bounds intersect is rebuilt from
  its runs, even if no glyph hits.
- **Images.** Pixels are tested by the axis-aligned box of their page-space
  footprint (rotated images lose a little more at the edges). The decoded
  bitmap ignores the image's soft mask and decode details, so a redacted
  image is re-embedded opaque and in DeviceGray/DeviceRGB. The image is
  replaced in place: another placement of the same XObject on that page is
  redacted at the same image coordinates. Images over the size limits or
  that cannot be decoded are removed.
- **Removed objects leave no bytes behind.** Verified against the pinned
  PDFium: content regeneration rewrites `/Resources` with only the objects
  still drawn and a non-incremental save writes only reachable original
  objects, so removed images, form XObjects and the old content streams
  are not in the saved file (they stay when another page still uses them,
  which then shows them anyway).
- **Form fields.** A removed widget stays reachable from `/AcroForm`, so
  Apply clears the value of every base-document field none of whose
  widgets is still presented (the cleared value is written by the form
  filler, regenerating the appearance). Fields that cannot be cleared that
  way - read-only fields holding a value, push buttons and signatures,
  whose appearance would stay in the file - refuse the apply. Field names
  remain in the file. The field's own `/DV` and `/RV` (a default repeating
  the redacted value) are emptied with the value (`PdfFormValue::
  clearDefaults`, `FPDFAnnot_SetStringValue` on each widget dictionary). A
  field whose widget has a separate `/Parent` dictionary may hold an
  inherited `/DV`, `/RV` or `/V` that the public API cannot read or remove;
  the apply is refused ("a redacted area contains a form field whose default
  value cannot be removed") rather than risk under-redacting.
- **Annotations.** Annotations whose `/Rect` intersects are removed (popups
  with their parent). A page with more than `kMaxAnnotationsPerPage`
  entries (some unlisted) refuses the apply.
- **Metadata.** PDFium cannot edit `/Info` or the catalog's `/Metadata`.
  "Also remove document metadata" therefore rebuilds the saved file by
  importing the finished pages into a fresh document: like Extract, the
  outline, AcroForm, page labels, structure tree, named destinations and
  attachments are not carried over (the prompt says so). It is not offered
  for encrypted documents, whose encryption the rebuild would drop, and
  the backend refuses it for them. Page-level `/Metadata` streams stay.
- **Whole objects.** Paths are removed when their bounds (which ignore any
  clip) intersect: a page-sized background rectangle or a table grid is
  removed entirely.

### Security-review hardening (2026-10-16)

Rule: never under-redact; when PDFium's public API cannot remove
something, the apply is refused with a reason.

- **Page thumbnails.** PDFium cannot delete a page's `/Thumb`, which would
  survive the save and can show the unredacted page. The source-page load
  detects it (`FPDFPage_GetRawThumbnailData`) and the apply is refused for
  that page: "page has an embedded thumbnail Rivet cannot remove".
- **XFA.** A document with XFA form data (`PdfFormInfo::hasXfa`) refuses the
  apply: XFA datasets keep field values Rivet cannot clear.
- **Annotations.** A `/Rect` with a non-finite component counts as
  intersecting and is removed.
- **Images.** If bounds overlap but redaction changed no pixel, the image
  object is removed whole instead of being kept.
- **Confirmation text** lists what is not scanned: bookmark/outline titles,
  structure-tree alt/actual text, annotation comments elsewhere, and
  metadata unless its removal is chosen.
- **Unapplied marks** are not written. Save, Save As, Extract, Split and
  Print ask "N redaction marks are not applied. Continue without redacting?"
  (`confirmUnappliedRedactions`); declining, or having no alert service,
  aborts.
- **Undo after save.** `rebaseOnto` clears the command history, so Apply
  Redactions cannot be undone once a save succeeded; a second save writes
  the already-redacted base (covered by a round-trip test).
