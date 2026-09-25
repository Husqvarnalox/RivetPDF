# tests/pdf fixtures

Deterministic geometry PDFs used by `TestPdfiumRender.cpp`. Each file is under
1 KB and is committed; tests never generate fixtures at build or run time.

## Regenerating

`make_fixtures.py` (pure Python stdlib, no randomness or timestamps) rebuilds
every fixture byte-identically:

```
python3 tests/pdf/fixtures/make_fixtures.py
```

Verify with `shasum -a 256 *.pdf` against the committed files.

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
