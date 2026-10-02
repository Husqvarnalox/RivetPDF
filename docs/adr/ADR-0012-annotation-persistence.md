# ADR-0012: Annotation persistence through the page assembly

Status: Accepted (phase4-annotations branch; pending integration into main)

Date: 2026-10-02

## Context

Annotation edits must be saved through the existing Phase 3 pipeline
(ADR-0009, ADR-0010). That pipeline is:

- A main-thread snapshot.
- Background assembly on a private PDFium working document, under the global
  call gate.
- Atomic replacement.
- Rebase onto the reopened file.

The live documents are never mutated.

PDFium's public write API (rev 40ecb4f) imposes constraints:

- `FPDFPage_CreateAnnot` cannot create Line, Polygon, PolyLine or Caret.
  It writes only `/Type` and `/Subtype`, with no `/P`, `/F` or `/NM`.
- `FPDFAnnot_SetStringValue` is the only generic writer and always writes a
  string. Name, number and array values cannot be written: no `/Name
  /Approved`, no `/LE`, no `/IT`, no `/BS`. No key can be removed.
- `FPDFAnnot_SetColor` fails once an appearance stream exists. It always
  writes `/CA` from the alpha component.
- PDFium does not apply `/CA` when drawing. Opacity takes effect only through
  the appearance's own ExtGState (`/GS gs`). `FPDFAnnot_SetAP` adds that
  ExtGState only when `/CA < 1` and gives the stream no other resources, so
  it has no fonts. `FPDFAnnot_AppendObject`, which builds the appearance from
  page objects and so supports fonts, works for Ink and Stamp only.
- `FPDFPage_RemoveAnnot` removes only the array entry. Later indices shift,
  and a separate `/Popup` entry stays behind.
- `FPDF_ImportPagesByIndex` deep-clones `/Annots`. Objects are mapped once per
  call, so two copies of the same page imported in ONE call share their
  annotation objects (and `/Annots` array, if indirect).
- No public API exposes object numbers. `/NM` can be written as a string.

## Decision

### Edits are expressed per assembled page

`PdfAssemblyPage::annotationEdits`
(`shared_ptr<const PdfPageAnnotationEdits>`) carries two lists:

- `removeIndices`: indices into the source page's `/Annots`. These are the
  page state's `suppressed` set, popups included.
- `create`: the page's overlay annotations in z-order.

The page model builds these from the page states in `toAssemblyRequest`, for
both Save and Extract.

### Applied after the view, on the working document only

For each output page that has edits, after `applyViews`:

1. Remove the listed indices, in descending order.
2. Append each `create` entry as a new annotation.
3. Verify the final `/Annots` count.

The resulting index of every created annotation is reported back
(`PdfAssembledPageAnnotations`) through the save job into
`DocumentSession::rebaseOnto`. The editor uses it to re-key its id registry
and to keep the written annotations overlay-drawn (ADR-0011).

### Editing an existing annotation means remove plus re-create

Rivet never modifies an existing annotation dictionary in place. An edited
original is removed together with its popup and written anew from Rivet's
model. The new annotation keeps:

- `/Contents`, `/T` and `/NM`.
- Colour, opacity and width.
- Geometry.

It loses:

- Its custom appearance.
- Rich text (`/RC`).
- Its popup.
- Unknown private keys.

Replies (`/IRT`) that pointed at it keep pointing at the old, no longer
listed dictionary. This is documented, accepted lossiness. It is the price
of never patching dictionaries that the public API cannot fully rewrite,
and of never mutating dictionaries possibly shared between imported copies.
Untouched annotations are written exactly as PDFium carries them through
(pages reused in place or imported).

### How each kind is written

Every created annotation gets:

- `/Rect`.
- `/F 4` (Print).
- `/C`, plus `/IC` for a filled shape, with `/CA` = opacity, set BEFORE any
  appearance exists.
- `/Border [0 0 w]` for stroked kinds.
- `/NM`: a Rivet-generated UUID, or the original's `/NM`.
- `/M`.
- `/Contents` and `/T` when present.

The appearance is:

- Highlight, Underline and StrikeOut: `/QuadPoints` in PDFium/Acrobat
  order (TL, TR, BL, BR) and `/Rect` = the quads' bounds. The appearance
  comes from `SetAP` with `pdf::appearanceContentStream(buildAppearance(d))`.
  Opacity comes through `/GS gs`, so translucent highlights stay readable.
  The PDFium API cannot express a Multiply blend.
- Text (note): a Rivet icon via `SetAP`. `/Name` is not written, so it
  defaults to Note.
- Ink: `/InkList` via `AddInkStroke` and `/Rect` = the stroke bounds plus
  half the width, before the appearance (`SetAP`).
- Square and Circle: `SetAP` stroke, plus fill when `/IC` is set.
- Line and Arrow: PDFium cannot create `/Line`, so they are written as `/Ink`
  annotations:
  - The strokes are the segment, plus the two arrow-head wings for an
    arrow.
  - They carry the private string key `/RivetShape (Line)` or
    `/RivetShape (Arrow)`, plus `/Subj` for other viewers' comment lists.
  - Other viewers see a correct ink drawing.
  - Rivet reads them back as Line/Arrow, but only when the marker and the
    stroke structure both match.
- Stamp:
  - `/Subtype /Stamp`, with the frame and label built with `AppendObject`
    (path objects plus a Helvetica-Bold text object, fitted to the box and
    rotated so the label reads upright on the page's view).
  - `/Name` cannot be written as a name, so it is omitted (default Draft).
    The standard name is stored in `/Subj`, which the comment lists of
    other viewers show.
  - The rotation relative to user space is stored in the private string key
    `/RivetRotation`.
  - On read, the name is taken from:
    1. a standard `/Name`;
    2. else `/Subj` when it is a standard name;
    3. else the stamp is custom and opaque.

Content streams are pure ASCII, with numbers formatted locale-independently.

### Duplicates are never imported twice in one call

The assembly splits import groups so that a source page index occurs at
most once per `FPDF_ImportPagesByIndex` call. Every imported copy therefore
owns its annotation objects, and per-copy edits cannot leak into another
copy. The cost is that resources are copied again for duplicated pages
only.

### Rebase

`DocumentWriteResult`'s rebase target carries the annotation report. For
each page with a non-null state, `rebaseOnto` performs these steps:

1. Remap the registry ids of surviving originals:
   `newIndex = oldIndex - |removed indices below it|`.
2. Set each overlay annotation's `fileIndex` to its reported index.
3. Set the new `suppressed` to exactly those indices.
4. Keep `rasterRevision`.

When no report is available, or it does not match, the state falls back to
null with a fresh `rasterRevision` (correct, at the cost of one re-render).

## Consequences

- Saved files are valid PDFs that any viewer renders correctly through the
  generated appearances. Semantic fidelity in other viewers is reduced
  where PDFium's API cannot write names: Line and Arrow show up as ink,
  stamps show up as "Draft" with a correct appearance and a `/Subj` naming
  the real stamp.
- Highlights use normal blending at reduced opacity, not Multiply.
- A future PDFium API, or a vetted low-level writer, can upgrade these
  encodings without changing the model: only the writer and reader change.
