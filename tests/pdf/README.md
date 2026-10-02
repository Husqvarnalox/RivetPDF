# tests/pdf fixtures

Deterministic PDFs used by `TestPdfiumRender.cpp` (geometry) and
`TestPdfiumText.cpp` (text extraction). Each file is under 2 KB and is
committed; the committed-file tests never generate fixtures at build or run
time. The Phase 5 content-editing tests are the exception: they build their
PDFs in memory (see "Generated content-editing fixtures" below).

## Regenerating

`make_fixtures.py` (pure Python stdlib, no randomness or timestamps) rebuilds
every fixture byte-identically - both the geometry fixtures and the text
fixtures:

```
python3 tests/pdf/fixtures/make_fixtures.py
```

Verify with `shasum -a 256 *.pdf` against the committed files; the committed
bytes must not change.

## The fixtures

All geometry fixtures share one US-Letter page (MediaBox `0 0 612 792`) and
one content stream: a large solid black rectangle covering the top-left
quadrant of the displayed page (display space x 0..306, y 0..396) plus a
20x20-point black square in each of the other three display quadrants, placed
so that at scale 1 each lands in exactly one of the three non-origin tiles of
the 2x2 grid of 512x512 device tiles covering the page.

| file           | /Rotate | displayed size | big black quadrant lands in |
| -------------- | ------- | -------------- | --------------------------- |
| `corners.pdf`  | absent  | 612x792        | top-left                    |
| `rot90.pdf`    | 90      | 792x612        | top-right                   |
| `rot180.pdf`   | 180     | 612x792        | bottom-right                |
| `rot270.pdf`   | 270     | 792x612        | bottom-left                 |

`/Rotate` is clockwise per the PDF spec (see PDF 32000-1:2008, 7.4.2), so the
black quadrant moves top-left -> top-right -> bottom-right -> bottom-left as
the quarter-turn increases. `corrupted.pdf` is deterministic garbage: the
first 100 bytes of a valid minimal PDF (header plus the start of the catalog
object, cut mid-token) followed by repeated ASCII filler, with no xref, no
trailer and no `%%EOF`; it exercises the `InvalidDocument` path.
`alpha.pdf` drops the corner geometry and fills a rectangle through an
ExtGState with `/ca 0.5`, producing a deterministic mid-gray blend for
straight-alpha testing.

## The text fixtures

The `text-*.pdf` files feed `TestPdfiumText.cpp` (text extraction) and are
described in detail in the `make_fixtures.py` module docstring, which is the
authoritative generator for every fixture here. In short, each is a single
US-Letter page (MediaBox `0 0 612 792`, crop box equal to the media box) with
standard-14 Type1 fonts, no compression and no timestamps:

| file               | /Rotate | content                                                        |
| ------------------ | ------- | -------------------------------------------------------------- |
| `text-basic.pdf`   | absent  | Helvetica 24 pt "Hello Rivet" (baseline at user (72, 720)) and Helvetica 9.5 pt "9.5 pt tail" (baseline at user (72, 650)) |
| `text-cyrillic.pdf`| absent  | Times-Roman 18 pt "Привет Rivet 123" (baseline at user (72, 700)); "Привет" is byte-coded 0x01..0x06 through a `/Differences` array of `/uniXXXX` glyph names, which PDFium maps to the exact Unicode code points |
| `text-multiline.pdf` | absent | four Helvetica 14 pt lines at user baselines y = 720, 700, 680, 660, the last being "Hello, world!  Two  spaces." (PDFium's extractor collapses duplicate spaces) |
| `text-rot90.pdf`   | 90      | Helvetica 20 pt "Rotated" (baseline at user (72, 720)); pins the user-space -> displayed-page rotation mapping for text boxes |

Text extraction reports geometry in DISPLAYED-PAGE space (points, origin at
the top-left of the displayed page, y-down, `/Rotate` applied) - the same
space as `renderPage`'s `pageRectPoints`. Generated characters (PDFium's
`\r\n` line breaks between text lines) extract with zero-area boxes. The
fixtures exist to pin: the display transform (especially the turn-1/turn-3
directions, cross-checked against the rot90 render fixture), exact Cyrillic
code-point extraction via glyph-name encodings, line-break handling, and row
clustering across font sizes.

## Generated content-editing fixtures

`PdfFixtures.hpp` (header-only, `rivet::test::pdffix`) builds the PDFs of the
Phase 5 content-editing integration tests (`tests/app/TestContentIntegration*.cpp`,
`tests/perf/PerfContentPdfium.cpp`) at test time from raw PDF syntax. Nothing is
copied from a third-party document: every byte is written by `buildPdf` (a
valid xref table over the given object bodies) from strings in the header, so
there is no licence or provenance question and nothing to commit. The output
is deterministic (no timestamps, no randomness, no compression).

| generator            | content                                                        |
| -------------------- | -------------------------------------------------------------- |
| `richPdf()`          | two 300x400 pages. Page 1: a filled red rectangle, a 4x4 RGB image XObject (ASCIIHex, 16 distinct colours), "Hello World" (Helvetica 18) and a three-line paragraph (Helvetica 12), plus a Square and a URI Link annotation over the text. Page 2: a green rectangle and "Page Two" |
| `cmykPdf()`          | one 200x200 page whose only content is CMYK-filled (`0 0 0 1 k`) plus a text line: the regeneration fidelity probe refuses it, so every object is read-only (ADR-0017) |
| `denseContentPdf(n)` | one page with `n` small filled rectangles and `n / 10` short Helvetica 8 text lines on a grid; used by the stress and perf probes |
| `blueJpeg()`         | the bytes of an 8x8 solid-blue baseline JPEG produced once with macOS `sips` (quality 60, 776 bytes) from a generated blank image; embedded as a byte array so tests need no image tooling and no decoder |

## Rendering contract

`PdfDocument::renderPage` receives `pageRectPoints` in displayed-page space
(points, origin at the top-left of the displayed page, y-down) and a scale in
device pixels per point. PDFium's `FPDF_RenderPageBitmapWithMatrix` applies
the caller's matrix on top of the page display matrix - the matrix that
already folds in `/Rotate`, the CropBox offset and the user-space y-flip - so
the caller matrix operates in displayed-page space: a plain scale plus tile
offset (a = d = scale, e = -minX * scale, f = -minY * scale, no flip). The
clip rect is the full bitmap. The bitmap is `FPDFBitmap_BGRA`, straight
(non-premultiplied) alpha, and is pre-filled with opaque white
(`0xFFFFFFFF`) so empty page regions come out white and every pixel is
alpha 255 today; a future render mode producing semi-transparent content can
feed the compositor without conversion.
