# ADR-0025: Visual signatures and digital signature display

Status: Accepted

Date: 2026-10-16

## Context

Users need to sign documents. A cryptographic (PAdES/CMS) signature needs a
crypto dependency, which is not approved (decided 2026-10-16); PDFium can
read existing signatures (`fpdf_signature.h`) but cannot create them.

## Decision

1. **Visual signature = a stamp annotation with a Rivet-built appearance.**
   A new `AnnotationKind::Signature` reuses the annotation pipeline
   (ADR-0011/0012/0013): identity, overlay rendering from one
   `buildAppearance` description, persistence as a `/Stamp` annotation with
   generated `/AP` and `/Name /RivetSignature`. Sources: (a) drawn: ink
   strokes captured in a signature pad sheet, normalized to a unit box;
   (b) typed: UTF-8 text set in the bundled Serif Italic-free face at
   auto size (no new font: Tinos Regular); (c) image: PNG/JPEG decoded by
   the platform `IImageDecoder`, embedded as an image XObject in the `/AP`.
   Placement: a Sign tool places the chosen signature at the click, then it
   moves/resizes like any annotation; one undo step each.
2. **Saved signatures** live in a per-user store owned by the app layer
   (`app/SignatureStore`): a JSON-free, versioned binary/text file in the
   platform's application-data directory (new `platform::IAppDataLocation`),
   at most 8 entries, each capped (ink points, image bytes 2 MiB). Never
   logged.
3. **Digital signature display (read-only).** `PdfDocument::signatures()`
   returns `PdfSignatureInfo` per signature: signer name (`/Name`),
   reason, location, signing time (`/M` string), sub-filter, byte range and
   whether the byte range covers the whole file except the contents hole
   ("covers whole document") and whether the document was modified after
   signing (byte range end < file size). No cryptographic verification;
   the UI says "Signature present (not verified)". A Signatures sidebar
   mode lists them; clicking scrolls to the signature widget.
4. Saving a document that has digital signatures with any edit warns once
   that the signatures will no longer cover the document (non-incremental
   save invalidates them).

## Consequences

- Visual signatures are annotations: flattening/locking them is out of
  scope; a recipient can delete them.
- Cryptographic signing is a later ADR once a crypto dependency exists.

Implementation notes and deviations (Phase 6C):

- `/Name` is written as the PDF **string** `(RivetSignature)`, not the name
  `/RivetSignature`: PDFium's public API (`FPDFAnnot_SetStringValue`) only
  writes strings and Rivet does not patch objects outside it. The reader
  accepts the string; Rivet `/Stamp` annotations carrying it come back as
  `Signature` items.
- A reopened visual signature is **opaque**: listed and drawn as a
  Signature (move, resize, delete), not re-editable as strokes/text/image.
- Typed signatures are drawn as **vector glyph outlines** of the bundled
  Tinos Regular (parsed from its `glyf`/`loca`, bounded), so no font is
  embedded and the result does not depend on viewer fonts. Images are
  scaled to at most 1024 px per side before embedding.
- `fpdf_signature.h` has no getter for the signer `/Name` or `/Location`:
  both are **omitted** from `PdfSignatureInfo`. Listed are coverage, reason,
  signing time, sub-filter and DocMDP permission.
- PDFium lists only top-level `/Sig` fields; unsigned fields (no
  `/ByteRange` and no `/SubFilter`) are skipped. The signature's page and
  widget rectangle are matched by order against the top-level signature
  widgets, only when the counts agree; otherwise the page is unknown and the
  sidebar entry does not navigate.
- The sidebar navigates to the signature's **page** (resolved through the
  page model), not to the exact widget position.
- The save warning is an `IAlertService::confirm` ("Save Anyway" / Cancel),
  asked once per tab for a dirty session whose base document has
  signatures (Save, Save As and saves during close/quit).
- Saved signatures: `RVSG` binary format, version 1, FNV-1a checksum,
  at most 8 entries; written with `AtomicFileWriter`, directory and file
  owner-only on POSIX. A corrupt file is ignored (status message, never
  logged) and replaced by the next save. No app-data location (null
  service) = signatures are used but not remembered.
