# ADR-0024: AcroForm viewing, filling and saving

Status: Accepted

Date: 2026-10-16

## Context

Many PDFs carry interactive AcroForm fields (text, check box, radio button,
combo box, list box, push button, signature). Rivet renders their widget
appearances through the PDFium raster but cannot fill them. Filling must
follow the rules every editing feature follows: the live document is never
mutated, edits are Rivet-owned immutable data on the one `CommandStack`,
and they are applied at assembly time by the Phase 3 save pipeline
(background, atomic, editing lock, rebase). JavaScript and XFA stay
disabled (ADR-0007).

## Decision

**1. Fields are read through the backend into portable records.**
`pdf/PdfForms.hpp` declares `PdfFormField` (fully qualified name, type,
flags: read-only, required, multiline, password, comb, max length,
do-not-scroll; current value; choice options with export values and
selection; on-state export value per check box/radio widget; widgets with
source page index, `/Annots` index and rect in page user space; default
appearance font size where readable) and `PdfFormInfo` (fields, `hasXfa`,
`limitsExceeded`). `PdfDocument::formInfo()` reads them through a private
reader document with a form-fill environment created with JavaScript and
XFA disabled, under the process-wide PDFium call gate. Limits: 5000
fields, 20000 widgets, 64 KiB per value, 1000 options per choice field;
past them the form is read-only with a status reason. Signature fields are
listed (type `Signature`) but are never fillable (ADR-0025).

**2. Values are document-level edits keyed by field name.**
A field may have widgets on several pages and one value, so values are not
per-page state. `editor::FormValues` is an immutable
`std::map<std::string, PdfFormValue>` (text, checked export value, or the
selected option indices) shared by pointer on the page model snapshot,
swapped by `FormValuesCommand` on the one `CommandStack` (undo is O(1)).
Only fields of the base document are fillable; widgets on pages imported
from another document keep their original appearance and are read-only.
Deleting a page that holds a widget keeps the field value (the field may
have widgets elsewhere); a field whose every widget is deleted is dropped
from the save request.

**3. Display: overlay for edited fields, raster for the rest.**
Untouched fields are drawn by the PDFium raster with their own
appearances. A field with an edited value is drawn by Rivet as a vector
overlay (text in the widget rect with the field's font size or auto size
in the bundled Sans face, comb and multiline layout, check and radio
marks, the selected combo text) and its widgets are hidden in the raster
for that render, exactly like suppressed annotations (ADR-0011). After a
save and rebase the written appearances match, so the overlay is dropped
with the edits and no tile flush is needed beyond the edited widgets'
pages.

**4. Save: values applied by PDFium's form filler at assembly time.**
`PdfAssemblyRequest` gains `formValues`. The backend opens a form-fill
environment on the working document (JavaScript and XFA off), and for each
edited field: text - focus the widget, select all, replace selection;
choice - set the selected indices; check box/radio - toggle by a
synthesized click at the widget center when the state differs; then kills
focus so PDFium regenerates `/AP` and writes `/V`. The report lists the
fields it could not apply; a field that fails aborts the save with an
error (never a silent partial save). Format, keystroke and calculate
actions do not run (no JavaScript); values are written as typed, and the
UI states this once per document that has such actions.

**5. Interaction.** Form fields are live in the default (Select) tool, as
in other viewers: hovering a fillable widget shows a text or hand cursor,
clicking focuses it. A `FormLayer` (a `ViewportLayer` before the
annotation layer) owns focus, the inline editor (the existing `TextField`
or `TextArea` sized to the widget, masked for password fields), the
choice popup, Tab / Shift+Tab traversal in page then widget order, Space
to toggle, Enter to commit single-line text, Esc to cancel. Each committed
value change is one undo step. The View menu gains "Highlight Form
Fields"; the Edit/Forms menu gains "Reset Form" (one command restoring the
original values). Read-only and XFA documents show the reason in the
status bar.

## Consequences

- Forms reuse the save pipeline, rebase and editing lock unchanged.
- No computed fields or validation scripts; documented limitation.
- Imported pages' forms are view-only until a field-merge policy exists.
- Flattening (burning values into page content) is a separate later step;
  redaction (ADR-0026) removes widgets in its area.

Implementation notes (closest safe alternatives where the decision could not
be followed literally):

- Widget drawing: `FPDF_RenderPageBitmap` never draws widgets, so before
  forms Rivet showed none at all. Live rendering now draws them with
  `FPDF_FFLDraw` on a lazily created form environment (version 1, no JS
  platform); the device rectangle is rounded to whole pixels. Documents that
  carry no AcroForm (e.g. Extract output, whose widgets lose their field
  tree) still show no widgets.
- Option export values: the backend API exposes only option labels, so
  `PdfFormOption::exportValue` equals the label.
- The editing overlay draws edited values in the UI system font (not the
  bundled Sans face) and upright on rotated pages; the saved file uses the
  field's own appearance generated by PDFium.
- The portable layer has no cursor API: fillable widgets get a hover outline
  instead of a cursor change.
- A radio button cannot be toggled off once on (matches viewer behavior).
- Save failures name the fields through `PdfAssemblyRequest::formFailures`
  (names are untrusted and only shown, never logged); the save fails as a
  whole.
- Multiline values are stored with LF; PDFium's CR is normalized on read and
  when verifying the applied values.
- Reset Form (QA #7, October 2026) follows PDF 32000-1 12.7.5.3 instead of
  "restore the original values": every fillable field goes to its /DV, else
  to the empty value of its type (text "", check box off, list box no
  selection), so it is enabled on a freshly opened, filled form. /DV is read
  from the widget dictionary only (the public API cannot reach a separate
  parent field's /DV; such fields reset to empty). Radio groups and combo
  boxes without /DV keep their stored value: PDFium's form filler can turn
  neither off/empty. List boxes select the clicked row in place (QA #8);
  only combo boxes use the choice popup.
