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
