#!/usr/bin/env python3
"""Regenerates the binary PDF fixtures in this directory, byte-identically.

Usage (from anywhere):

    python3 tests/pdf/fixtures/make_fixtures.py

Pure Python standard library, no randomness, no timestamps: every byte of
every fixture is a fixed function of this script's source text, so re-running
it reproduces the committed files exactly (verify with `shasum -a 256 *.pdf`).

All geometry fixtures share one page and one content stream:

  * MediaBox 0 0 612 792 (US Letter, user space y-up, origin bottom-left).
  * One large solid black rectangle covering the TOP-LEFT quadrant of the
    DISPLAYED page for /Rotate 0: display space (top-left origin, y-down)
    x [0,306], y [0,396]; drawn in user space as x [0,306], y [396,792].
  * One 20x20-point solid black square in each of the other three display
    quadrants, placed so that (at scale 1) each lands in exactly one of the
    three non-origin 512x512 device tiles of the 2x2 tile grid covering the
    612x792 page:
      - top-right quadrant, user (550,584)-(570,604)  -> display (550..570, 188..208),
        inside tile (1,0) = display x [512,612], y [0,512].
      - bottom-left quadrant, user (143,132)-(163,152) -> display (143..163, 640..660),
        inside tile (0,1) = display x [0,512], y [512,792].
      - bottom-right quadrant, user (540,132)-(560,152) -> display (540..560, 640..660),
        inside tile (1,1) = display x [512,612], y [512,792].

Fixtures written here (each < 1 KB):

  corners.pdf   the geometry above, /Rotate absent (page displays 612x792).
  rot90.pdf     same geometry, /Pages /Rotate 90 (displays 792x612; per the
                PDF spec /Rotate is clockwise, so the big black quadrant
                moves from top-left to TOP-RIGHT).
  rot180.pdf    same geometry, /Pages /Rotate 180 (612x792; black quadrant at
                BOTTOM-RIGHT).
  rot270.pdf    same geometry, /Pages /Rotate 270 (792x612; black quadrant at
                BOTTOM-LEFT).
  alpha.pdf     no corner geometry: one rectangle filled black through an
                ExtGState with /ca 0.5. Over the renderer's opaque white
                background this blends to a deterministic mid-gray
                (channel ~= 255 * (1 - 0.5)), for straight-alpha tests.
  corrupted.pdf deterministic garbage: the first 100 bytes of a valid minimal
                PDF (header + the start of the catalog object, cut mid-token)
                followed by repeated ASCII garbage and no xref, no trailer and
                no %%EOF. PDFium must reject it as malformed.

Text fixtures (each also < 2 KB; all one US-Letter page, MediaBox 0 0 612 792,
crop box equal to the media box, standard-14 Type1 fonts, no compression and
no timestamps):

  text-basic.pdf     no /Rotate; Helvetica 24 pt "Hello Rivet" with its
                     baseline at user (72, 720) and Helvetica 9.5 pt
                     "9.5 pt tail" with its baseline at user (72, 650).
                     Extraction must return both lines; the 'H' display box
                     must land just above the displayed baseline
                     (display y = 792 - 720 = 72, y-down).
  text-cyrillic.pdf  Times-Roman 18 pt "Привет Rivet 123" with its baseline
                     at user (72, 700). WinAnsi cannot encode Cyrillic, so
                     the font dictionary carries an /Encoding with a
                     /Differences array mapping single bytes 0x01..0x06 onto
                     the glyph names /uni041F /uni0440 /uni0438 /uni0432
                     /uni0435 /uni0442 (П р и в е т); the content string
                     spells "Привет" with those bytes and the rest of the
                     line through the WinAnsi base encoding. PDFium resolves
                     /uniXXXX names to Unicode (FreeType's Adobe glyph-name
                     mapper), so extraction returns the exact code points.
  text-multiline.pdf four Helvetica 14 pt lines at user baselines y = 720,
                     700, 680, 660, each in its own BT..ET block: three
                     prose lines with different word lengths plus
                     "Hello, world!  Two  spaces." with doubled interior
                     spaces and punctuation. PDFium inserts generated \r\n
                     pairs between the lines (zero-area char boxes), so the
                     fixture pins line-break handling and row clustering.
  text-rot90.pdf     /Pages /Rotate 90 (displays 792x612); Helvetica 20 pt
                     "Rotated" with its baseline at user (72, 720). With the
                     clockwise quarter turn the line runs VERTICALLY on the
                     displayed page: the first char's display box starts near
                     display (720, 72) and reading order walks downward.
                     This is the ground truth that pins the turn-1 mapping
                     of the user-space -> display-space transform (same
                     convention the rot90 render fixture pinned in Phase 1:
                     content at the user-space top-left lands top-right).

The committed .pdf files ARE the fixtures; tests never run this script. It
exists only so the bytes can be regenerated and audited.
"""

import pathlib

HERE = pathlib.Path(__file__).resolve().parent

# Shared page geometry, user space (y-up), MediaBox 612x792.
PAGE_W, PAGE_H = 612, 792

# Big quadrant: user-space x/y/w/h for the display-space top-left quadrant.
BIG_QUAD = (0, 396, 306, 396)
# 20x20 squares (user-space x, y of the lower-left corner); see module docstring.
MARKS = ((550, 584), (143, 132), (540, 132))


def content_stream() -> bytes:
    rects = [b"%d %d %d %d re f" % BIG_QUAD]
    rects += [b"%d %d 20 20 re f" % mark for mark in MARKS]
    body = b"\n".join([b"q", b"0 0 0 rg"] + rects + [b"Q"])
    return body + b"\n"


def alpha_content_stream() -> bytes:
    # Mid-page rectangle (user x 100..512, y 392..592) filled through the
    # ExtGState: display space x 100..512, y 200..400.
    body = b"\n".join([b"q", b"/GS0 gs", b"0 0 0 rg", b"100 392 412 200 re f", b"Q"])
    return body + b"\n"


def build_pdf(objects: list[bytes]) -> bytes:
    """Assembles numbered objects with an exact xref table and trailer."""
    header = b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
    body = b""
    offsets = []
    for number, obj in enumerate(objects, start=1):
        offsets.append(len(header) + len(body))
        body += b"%d 0 obj\n" % number + obj + b"\nendobj\n"
    xref_pos = len(header) + len(body)
    count = len(objects) + 1
    xref = b"xref\n0 %d\n0000000000 65535 f \n" % count
    xref += b"".join(b"%010d 00000 n \n" % offset for offset in offsets)
    trailer = b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        count,
        xref_pos,
    )
    return header + body + xref + trailer


def geometry_objects(rotate: int | None, resources: bytes = b"<< >>") -> list[bytes]:
    stream = content_stream()
    pages = b"<< /Type /Pages /Kids [3 0 R] /Count 1"
    if rotate is not None:
        pages += b" /Rotate %d" % rotate
    pages += b" >>"
    return [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        pages,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources "
        + resources
        + b" /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"endstream",
    ]


def write(name: str, data: bytes) -> None:
    path = HERE / name
    path.write_bytes(data)
    print("%-14s %5d bytes" % (name, len(data)))


# ---------------------------------------------------------------------------
# Text fixtures.
#
# Shared layout: object 1 catalog, object 2 pages (optional /Rotate), object 3
# page, object 4 content stream, object 5 the font. Text strings are plain
# single-byte PDF string literals; WinAnsi covers ASCII, and the Cyrillic
# fixture maps bytes 0x01..0x06 through /Differences glyph names.
# ---------------------------------------------------------------------------

# "Привет" in reading order, spelled with /Differences byte codes 0x01..0x06
# (see text-cyrillic.pdf in the module docstring).
CYRILLIC_CODES = (0x01, 0x02, 0x03, 0x04, 0x05, 0x06)
CYRILLIC_STRING = b"".join(b"\\%03o" % code for code in CYRILLIC_CODES)

HELVETICA_FONT = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"

TIMES_CYRILLIC_FONT = (
    b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding << /Type /Encoding "
    b"/BaseEncoding /WinAnsiEncoding /Differences [1"
    + b"".join(b" /uni%04X" % cp for cp in (0x041F, 0x0440, 0x0438, 0x0432, 0x0435, 0x0442))
    + b"] >> >>"
)


def text_objects(font: bytes, lines: list[bytes], rotate: int | None = None) -> list[bytes]:
    """One page with one Type1 font and one BT..ET block per text line.

    Each line is a full content-stream operator sequence like
    b"BT /F1 24 Tf 72 720 Td (Hello Rivet) Tj ET".
    """
    stream = b"\n".join(lines) + b"\n"
    pages = b"<< /Type /Pages /Kids [3 0 R] /Count 1"
    if rotate is not None:
        pages += b" /Rotate %d" % rotate
    pages += b" >>"
    return [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        pages,
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources "
        b"<< /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"endstream",
        font,
    ]


def text_page_content(label: bytes) -> bytes:
    stream = b"BT /F1 24 Tf 72 720 Td (" + label + b") Tj ET\n0 .6 1 rg 72 600 200 100 re f\n"
    return b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"endstream"


def link_annot(rect: tuple[int, int, int, int], dest: bytes) -> bytes:
    return (b"<< /Type /Annot /Subtype /Link /Rect [%d %d %d %d] /Border [0 0 0] /Dest " % rect) + dest + b" >>"


def uri_annot(rect: tuple[int, int, int, int], url: bytes) -> bytes:
    return (
        b"<< /Type /Annot /Subtype /Link /Rect [%d %d %d %d] /Border [0 0 0] /A << /S /URI /URI (%s) >> >>"
        % (rect + (url,))
    )


def page_object_numbers(page_count: int, annot_counts: list[int]) -> list[int]:
    """Object number of each page, given per-page annotation counts. Used to
    build /Dest references BEFORE the annots themselves exist."""
    nums = []
    nxt = 3
    for i in range(page_count):
        nums.append(nxt)
        nxt += 2 + annot_counts[i]
    return nums


def multi_page_objects(page_count: int, annots: dict[int, list[bytes]] | None = None,
                       catalog_extra: bytes = b"") -> tuple[list[bytes], list[int]]:
    """Catalog + Pages + (Page + content + annots) per page.

    Each page consumes 2 + annots_count objects, so page object numbers are
    COMPUTED (not stride-fixed) and also returned, so /Dest arrays can
    reference them: use page_object_numbers[i] (1-based object numbers)."""
    annots = annots or {}
    page_nums = page_object_numbers(
        page_count, [len(annots.get(i, [])) for i in range(page_count)])
    kids = [b"%d 0 R" % n for n in page_nums]
    objects: list[bytes] = [
        b"<< /Type /Catalog /Pages 2 0 R" + catalog_extra + b" >>",
        b"<< /Type /Pages /Kids [" + b" ".join(kids) + b"] /Count %d >>" % page_count,
    ]
    for i in range(page_count):
        page_obj = page_nums[i]
        annot_objs = annots.get(i, [])
        annot_refs = b""
        if annot_objs:
            annot_refs = b" /Annots [" + b" ".join(
                b"%d 0 R" % (page_obj + 2 + a) for a in range(len(annot_objs))
            ) + b"]"
        objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources "
                       b"<< /Font << /F1 2 0 R >> >> /Contents %d 0 R" % (page_obj + 1)
                       + annot_refs + b" >>")
        objects.append(text_page_content(b"Page %d of %d" % (i + 1, page_count)))
        for annot in annot_objs:
            objects.append(annot)
    return objects, page_nums


def text_line(font_size: float, x: float, y: float, text: bytes) -> bytes:
    return b"BT /F1 %s Tf %s %s Td (%s) Tj ET" % (
        ("%g" % font_size).encode(),
        ("%g" % x).encode(),
        ("%g" % y).encode(),
        text,
    )


def main() -> None:
    # corners.pdf / rot90 / rot180 / rot270: one content stream, varying /Rotate
    # on the (inherited) Pages node.
    write("corners.pdf", build_pdf(geometry_objects(None)))
    write("rot90.pdf", build_pdf(geometry_objects(90)))
    write("rot180.pdf", build_pdf(geometry_objects(180)))
    write("rot270.pdf", build_pdf(geometry_objects(270)))

    # alpha.pdf: adds an ExtGState object and its resource entry.
    stream = alpha_content_stream()
    alpha_objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources "
        b"<< /ExtGState << /GS0 5 0 R >> >> /Contents 4 0 R >>",
        b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"endstream",
        b"<< /Type /ExtGState /CA 0.5 /ca 0.5 >>",
    ]
    write("alpha.pdf", build_pdf(alpha_objects))

    # corrupted.pdf: deterministic malformation - valid header, then the start
    # of the catalog object cut mid-token, then ASCII garbage. No complete
    # object, no xref, no trailer, no %%EOF.
    prefix = build_pdf(geometry_objects(None))[:100]
    garbage = b"GARBAGE%%GARBAGE%%" * 20
    write("corrupted.pdf", prefix + b"\n" + garbage + b"\n")

    # text-basic.pdf: two Helvetica lines, no /Rotate. The 24 pt line's
    # displayed baseline is at y = 792 - 720 = 72 (display space, y-down).
    write("text-basic.pdf", build_pdf(text_objects(HELVETICA_FONT, [
        text_line(24.0, 72, 720, b"Hello Rivet"),
        text_line(9.5, 72, 650, b"9.5 pt tail"),
    ])))

    # text-cyrillic.pdf: "Привет" byte-coded through /Differences /uniXXXX
    # glyph names, " Rivet 123" through the WinAnsi base encoding.
    write("text-cyrillic.pdf", build_pdf(text_objects(TIMES_CYRILLIC_FONT, [
        text_line(18.0, 72, 700, CYRILLIC_STRING + b" Rivet 123"),
    ])))

    # text-multiline.pdf: four 14 pt lines; PDFium inserts generated \r\n
    # pairs between consecutive lines, so extraction sees line breaks with
    # zero-area boxes.
    write("text-multiline.pdf", build_pdf(text_objects(HELVETICA_FONT, [
        text_line(14.0, 72, 720, b"The quick brown fox jumps"),
        text_line(14.0, 72, 700, b"over the lazy dog today"),
        text_line(14.0, 72, 680, b"Third line has prose"),
        text_line(14.0, 72, 660, b"Hello, world!  Two  spaces."),
    ])))

    # text-rot90.pdf: one 20 pt line on a /Rotate 90 page; the display-space
    # boxes pin the turn-1 mapping of the user->display transform.
    write("text-rot90.pdf", build_pdf(text_objects(HELVETICA_FONT, [
        text_line(20.0, 72, 720, b"Rotated"),
    ], rotate=90)))

    # outline.pdf: 3 pages + a two-level outline tree. The font object is
    # appended last; page content streams reference it as FONT_OBJ 0 R.
    objects, page_nums = multi_page_objects(3)
    font_obj = len(objects) + 1
    objects.append(HELVETICA_FONT)
    objects = [o.replace(b"/F1 2 0 R", b"/F1 %d 0 R" % font_obj) for o in objects]
    base = len(objects)  # first free object number
    outlines_obj = base + 1
    bm1 = outlines_obj + 1
    bm2 = outlines_obj + 2
    bm3 = outlines_obj + 3
    bm4 = outlines_obj + 4
    objects.append(b"<< /Type /Outlines /First %d 0 R /Last %d 0 R /Count 4 >>" % (bm1, bm4))
    objects.append(b"<< /Title (Chapter 1) /Parent %d 0 R /Next %d 0 R /First %d 0 R "
                   b"/Last %d 0 R /Count 2 /Dest [%d 0 R /Fit] >>"
                   % (outlines_obj, bm4, bm2, bm3, page_nums[0]))
    objects.append(b"<< /Title (Section 1.1) /Parent %d 0 R /Next %d 0 R "
                   b"/Dest [%d 0 R /XYZ 72 720 0] >>" % (bm1, bm3, page_nums[1]))
    objects.append(b"<< /Title (Section 1.2) /Parent %d 0 R /Dest [%d 0 R /Fit] >>"
                   % (bm1, page_nums[2]))
    objects.append(b"<< /Title (Chapter 2) /Parent %d 0 R /Dest [%d 0 R /Fit] >>"
                   % (outlines_obj, page_nums[2]))
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R /Outlines %d 0 R >>" % outlines_obj
    write("outline.pdf", build_pdf(objects))

    # internal-links.pdf: two GoTo links on page 0 (to pages 1 and 2), one on
    # page 1 (to page 2). Rects are in user space (y-up): the first link's
    # [72 700 320 724] maps to display y 68..92 on the 612x792 page.
    # Layout first (2 annots on page 0, 1 on page 1), then the annots that
    # reference the computed page object numbers.
    nums = page_object_numbers(3, [2, 1, 0])
    objects, _ = multi_page_objects(3, annots={
        0: [link_annot((72, 700, 320, 724), b"[%d 0 R /Fit]" % nums[1]),
            link_annot((72, 600, 320, 624), b"[%d 0 R /Fit]" % nums[2])],
        1: [link_annot((72, 700, 320, 724), b"[%d 0 R /Fit]" % nums[2])],
    })
    font_obj = len(objects) + 1
    objects.append(HELVETICA_FONT)
    objects = [o.replace(b"/F1 2 0 R", b"/F1 %d 0 R" % font_obj) for o in objects]
    write("internal-links.pdf", build_pdf(objects))

    # external-link.pdf: one URI link over the first text line.
    objects, _ = multi_page_objects(1, annots={
        0: [uri_annot((72, 700, 320, 724), b"https://example.com/rivet")],
    })
    font_obj = len(objects) + 1
    objects.append(HELVETICA_FONT)
    objects = [o.replace(b"/F1 2 0 R", b"/F1 %d 0 R" % font_obj) for o in objects]
    write("external-link.pdf", build_pdf(objects))

    # page-labels.pdf: page 0 label "i" (lowercase roman), pages 1..3 labeled
    # with the decimal prefix "A-" -> "A-1", "A-2", "A-3".
    objects, _ = multi_page_objects(4, catalog_extra=b" /PageLabels << /Nums [0 << /S /r >> "
                                    b"1 << /S /D /P (A-) >> ] >>")
    font_obj = len(objects) + 1
    objects.append(HELVETICA_FONT)
    objects = [o.replace(b"/F1 2 0 R", b"/F1 %d 0 R" % font_obj) for o in objects]
    write("page-labels.pdf", build_pdf(objects))


if __name__ == "__main__":
    main()
