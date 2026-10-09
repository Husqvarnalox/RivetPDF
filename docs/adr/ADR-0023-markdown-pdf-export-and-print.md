# ADR-0023: Markdown export to PDF and printing

Status: Accepted

Date: 2026-10-10

## Context

Markdown tabs can be viewed and edited (ADR-0019 to ADR-0022) but neither
exported nor printed. The preview is laid out by `rivet_markdown`'s pure
layout engine against a platform text measurer (CoreText on macOS) and
painted through `ui::PaintContext`. A printable artifact must be a real PDF
(selectable text, clickable links, vector rules) and must not depend on a
web engine, PDFKit or any platform PDF writer (ADR-0002, ADR-0003).

Constraints: PDFium is the only PDF backend and `FPDF_*` stays inside
`src/pdf/pdfium/`; the portable layers must keep building without PDFium
and on Linux; the fonts available for new PDF text are the bundled
Arimo/Tinos/Cousine subsets (ADR-0016).

## Decision

**1. A portable document-build description in `rivet_pdf`.**
`pdf/PdfBuild.hpp` describes a NEW document as Rivet-owned data: pages
with a size in points and an ordered list of items in page user space
(y-up, origin bottom-left): text runs (bundled font, size, color,
baseline origin, UTF-8 text), filled/stroked rectangles, images
(`PdfImageData`), plus link rectangles with external URIs. `validatePdfBuild`
enforces limits (pages, items, text bytes, finite geometry, allowed URI
schemes). `PdfEngine::buildDocument(const PdfBuildDocument&, IPdfByteSink&)`
materializes it; the null engine reports `NotAvailable`.

**2. The PDFium builder.** `src/pdf/pdfium/PdfiumBuilder.cpp` creates the
document with `FPDF_CreateNewDocument`, one `FPDFPage_New` per page, loads
each bundled font once per document with `FPDFText_LoadFont` (TrueType,
embedded), emits text objects (`FPDFPageObj_NewTextObj` + `FPDFText_SetText`
in UTF-16), rectangles (`FPDFPageObj_CreateNewRect`), images
(`FPDFPageObj_NewImageObj` + bitmap/JPEG), link annotations
(`FPDFPage_CreateAnnot(LINK)` + `FPDFAnnot_SetRect` + `FPDFAnnot_SetURI`),
generates content and streams with `FPDF_SaveAsCopy` to the sink. The whole
build runs under one acquisition of the PDFium call gate, like an assembly.
The builder is strict: `validatePdfBuild` rejects text a bundled font does
not cover, so the replacement policy (below) lives in the portable exporter,
not in the backend.

**3. Portable bundled-font metrics.** `pdf/BundledFontMetrics.hpp` reads
`head`, `hhea`, `hmtx` and `cmap` (formats 4 and 12) of the bundled TrueType
programs at first use and exposes `advance(codepoint)`, ascent, descent and
line gap in font units. These are the same horizontal metrics PDFium takes
from the embedded font, so a layout measured with them matches the written
PDF exactly; no platform text engine is involved. Available in every build
(no PDFium needed), so layout and pagination are unit-tested in `p-off`.

**4. Export layout = preview layout with a different measurer.**
`app/MarkdownPdfExport` (portable, no platform code) implements
`markdown::ITextMeasurer` over the bundled metrics (Sans for body and
headings, Sans Bold for bold and headings, Mono for code; italic is
rendered upright, strike-through as a rule over the run), runs
`layoutMarkdown` at the page content width with the export typography
(A4 or US Letter, 1-inch margins, body 11 pt), and paginates with
`markdown::paginateLayout`: page cuts are chosen only between lines
(never through a line), an item taller than a page is split at the page
edge, decorations are clipped to each page. Each page's runs, decorations
and images are translated into build items; links with allowed external
schemes (`UrlPolicy`) become link annotations. Characters no bundled font
covers are replaced by `?` before the build and counted; the count is
reported to the user (never silently dropped). Images come from the tab's
already-decoded `MarkdownImageStore` bitmaps; images not loaded yet or
blocked are exported as their placeholder box.

**5. Shell flow.** `FileCommand::ExportPdf` ("Export as PDF…", Markdown
tabs only) shows the save panel, snapshots the tab's source text, and on a
worker parses, lays out, builds and writes through `core::io::AtomicFileWriter`
(same atomic replacement as Save, ADR-0010); completion returns through
`IMainThreadDispatcher` and the status bar reports the result and any
replaced-character count. Editing stays enabled during an export (the
export works on its snapshot). Printing a Markdown tab exports to a private
temp file, opens it with the engine and prints it through the unchanged
`PrintCoordinator`/`PrintSpooler` pipeline (`printDocument` entry); the temp
file is removed when the job ends. Without a PDF backend both commands are
disabled with a status message.

## Consequences

- Export output is deterministic for a given source and font set except for
  PDFium's random `/ID`.
- Exported text is limited to the bundled subsets' coverage (Latin, Greek,
  Cyrillic); other scripts are replaced by `?` and reported. Extending
  coverage is a font-bundle change (ADR-0016), not an architecture change.
- The preview (platform fonts) and the export (bundled fonts) can break
  lines differently; the export is laid out on its own and is internally
  consistent.
- Internal anchors (`#heading`) are not exported as links (PDFium exposes no
  public API for destinations); outline/bookmarks are not written for the
  same reason.
- The builder is a general facility: later phases (OCR text layers, visual
  signatures, comparison reports) can reuse `PdfBuild` items.
