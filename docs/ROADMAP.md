# Rivet roadmap: Phase 6 and beyond

Phases 1-5 (viewer, page editing, file lifecycle, annotations, content
editing) and the first Markdown milestones (MD1-MD3: parser, native
rendering, source editing with live preview) are closed; see
[ARCHITECTURE.md](ARCHITECTURE.md) for their status. This document is the
plan for the next phases. Every workstream lands under the same rules as
before: an ADR for each architectural decision, portable layers that build
on Windows/Linux, PDFium as the only PDF backend, the four local build
gates (`p-off`, `p-on`, `p-asan`, `p-tsan`) green with `-Werror`, CI green.

## Phase 6: a complete editor (in progress)

Workstreams, in the order they are scheduled. Each is independently
shippable and documented in the README status table when it lands.

| # | Workstream | ADR | Status |
| --- | --- | --- | --- |
| 6A | **Markdown export to PDF and printing.** A vector PDF (real text, bundled fonts, images, links) built from the same layout engine as the preview; printing a Markdown tab prints that document through the existing print pipeline. | [ADR-0023](adr/ADR-0023-markdown-pdf-export-and-print.md) | In progress |
| 6B | **PDF forms (AcroForm).** View, fill (text, check boxes, radio buttons, combo and list boxes) and save form fields with undo/redo; values are Rivet-owned edits applied at assembly time, never mutations of the live document. | ADR-0024 (planned) | Planned |
| 6C | **Signatures.** Visual signatures (drawn, typed in a bundled font, or an image) placed as a stamp-like annotation; display of existing digital signatures (signer, time, byte-range coverage) read through the backend. Cryptographic signing is out of scope until a crypto dependency is approved. | ADR-0025 (planned) | Planned |
| 6D | **Redaction.** True removal: text objects, images and paths intersecting the redaction area are deleted from the page content (images cropped), annotations under it removed, and an opaque box written in their place; applied through the Phase 5 content pipeline. | ADR-0026 (planned) | Planned |
| 6E | **OCR.** A portable `ITextRecognizer` interface implemented by the platform layer (macOS: Vision) that produces an invisible text layer (render mode 3) through the content pipeline, so scanned pages become searchable and selectable. | ADR-0027 (planned) | Planned |
| 6F | **Reduce file size.** Image recompression and downsampling on Save As through the content image-replacement path; unreferenced objects are already dropped by non-incremental saves. | ADR-0028 (planned) | Planned |
| 6G | **Encryption.** Remove a password on Save As (backend `RemoveSecurity`). Adding a password requires an encrypting writer PDFium does not have; deferred until a crypto dependency is approved. | - | Planned |
| 6H | **Document comparison.** Two documents side by side with text-level differences (word diff per page pair) highlighted in both. | ADR-0029 (planned) | Planned |

## Phase 7: Markdown authoring

| # | Workstream | Status |
| --- | --- | --- |
| MD5 | WYSIWYG editing in the Rendered view (caret, selection and typing mapped back to source edits through the source map; the source stays authoritative per ADR-0021). | Planned |
| MD6 | Mermaid diagrams and math (lightweight native renderers; no web engine). | Planned |

## Not scheduled

Windows and Linux desktop shells. The portable layers are built and tested
on Ubuntu/GCC in CI so the shells can be added without reworking the core.
