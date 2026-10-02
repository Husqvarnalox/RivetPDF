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

Page-editing fixtures (views, assembly; all Helvetica, no compression, no
timestamps, MediaBox on every page dictionary):

  markers-5.pdf  5 US-Letter pages. Page i (1-based) shows Helvetica 48 pt
                 "PAGE-i" with its baseline at user (72, 700) - so page order
                 is verifiable by text extraction - and a 60x60 black block
                 at user x 72 + 100*(i-1), y 100..160 (display y 632..692),
                 so order is also verifiable visually.
  import-3.pdf   3 pages "IMPORT-1".."IMPORT-3" (Helvetica 36 pt, baseline
                 at user (72, 500)); page 2 is landscape (MediaBox
                 [0 0 792 612]), pages 1 and 3 are US Letter. Page i also
                 has a 40x40 black block at user x 400 + 80*(i-1), y 50..90.
  cropbox.pdf    3 pages with non-trivial boxes:
                   page 1: MediaBox [0 0 612 792], CropBox [36 36 576 756]
                           (displays 540x720). A 36x36 black block fills the
                           crop box's top-left corner (user 36..72 x
                           720..756 -> display 0..36 x 0..36), a 30x30 block
                           sits in the media box's bottom-left corner OUTSIDE
                           the crop box (user 0..30 x 0..30: only a media-box
                           view shows it), and Helvetica 24 pt "CROP" has its
                           baseline at user (100, 400).
                   page 2: the same page with /Rotate 90 (displays 720x540).
                   page 3: MediaBox [100 200 400 600] (non-zero origin,
                           300x400, no CropBox); a 30x30 block at user
                           100..130 x 570..600 (display 0..30 x 0..30) and
                           "OFFSET" (24 pt) at user (120, 400).

Annotation fixture (read/write/preservation tests; no compression, no
timestamps; every page MediaBox 0 0 612 792 with an empty content stream, so
the page is plain white and annotations are the only ink):

  annots.pdf     3 pages.
                   page 1 (/Rotate 0): /Annots (direct array), in order -
                     0 Highlight   Rect [100 700 300 720], 1 quad, /C [1 1 0],
                                   /CA 0.5, /Contents "Highlighted", AP
                     1 Underline   Rect [100 660 300 680], /C [0 .5 0], NO /AP
                     2 StrikeOut   Rect [100 620 300 640], /C [1 0 0], AP
                     3 Text note   Rect [400 700 420 720], /C [1 .8 0], AP,
                                   /Contents "Привет, мир" (UTF-16BE with
                                   BOM), /T (Tester), /NM (note-1), /Popup 4
                     4 Popup       /Parent 3, Rect [430 600 600 700]
                     5 Ink         2 strokes (3 points each), /C [0 0 1],
                                   /BS << /W 2 >>, AP stroked at 2
                     6 Square      Rect [300 500 400 560], /C [0 .5 0],
                                   /IC [.8 1 .8], /Border [0 0 3], AP
                     7 Circle      Rect [420 500 520 560], /C [1 0 1],
                                   /BS << /W 4 >>, AP stroked at 4
                     8 Line        third-party /Subtype /Line with /L and
                                   /LE [/None /OpenArrow]: must stay Other
                     9 Stamp       /Name /Approved, Rect [100 300 250 350],
                                   /C [0 .6 0], AP
                    10 FreeText    /DA + AP: unsupported, must stay Other
                    11 Squiggly    Rect [100 250 300 270], 1 quad, AP:
                                   unsupported, must stay Other
                   page 2 (/Rotate 90): /Annots is an INDIRECT array object
                     (so two imported copies of the page would share it
                     without per-copy import groups), one Square
                     Rect [100 100 200 160], /C [1 0 0], /Border [0 0 2].
                   page 3 (/CropBox [100 100 500 700]): one Square
                     Rect [150 150 250 250], /C [0 0 1], /Border [0 0 2].

Hostile-input fixtures (Phase 4 hardening; read/preserve tests, never edited by
Rivet; every page MediaBox 0 0 612 792 with a shared empty content stream):

  annots-many.pdf     1 page whose /Annots holds 4200 entries (> the 4096
                      Rivet reads in detail). Compact: every entry is the SAME
                      indirect Square annotation (/Rect [100 100 140 140],
                      /C [1 0 0], one shared /AP stream).
  annots-hostile.pdf  PDF 1.5 file (annotation objects live in a compressed
                      object stream so over-limit payloads stay tiny). 8 pages:
                        0 string limits: Text notes with /Contents of 65537
                          bytes (0), /T of 1025 bytes (1), /NM of 257 bytes
                          (2); a note with /Contents of exactly 65536 bytes
                          (3, editable boundary); a plain note (4).
                        1 geometry limits: Highlight with 4097 quads (0), with
                          exactly 4096 quads (1), with 7 /QuadPoints numbers
                          (2), with non-numeric /QuadPoints entries (3),
                          without /QuadPoints (4); Ink with one 10001-point
                          stroke (5), 257 strokes (6), empty /InkList (7),
                          /InkList [1 2 3] (8), a stroke of 3 numbers (9),
                          54000 points in total (10), and a valid Ink (11).
                        2 malformed /Annots entries: null, an integer, a
                          dangling reference, a string, then dictionaries
                          without /Rect, with an inverted /Rect, with huge and
                          non-numeric /Rect entries, without /Subtype, with an
                          unknown subtype, and a valid Square last (index 11).
                        3 /Annots is an integer (not an array).
                        4 /Annots is a dictionary (not an array).
                        5 relations and actions: a Text whose /Popup is itself
                          (0), a Popup whose /Parent is itself (1), a Text
                          whose /Popup lives on page 6 (2), an /IRT cycle
                          (3, 4), FileAttachment (5), Link with a /JavaScript
                          action (6), Link with a /Launch action (7), a
                          Widget (8).
                        6 the foreign Popup (index 0) page 5's annotation 2
                          points at, plus its parent Text (1).
                        7 text encoding: /Contents with an unpaired high
                          surrogate (0), an unpaired low surrogate (1) and a
                          valid supplementary-plane pair (2); all Text notes
                          with /C.

The committed .pdf files ARE the fixtures; tests never run this script. It
exists only so the bytes can be regenerated and audited.
"""

import hashlib
import pathlib
import struct

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


# ---------------- encrypted fixture (password.pdf) ----------------
#
# Deterministic PDF standard-security-handler R3 (RC4-128) encryption,
# implemented with hashlib's MD5 and a small RC4 - no timestamps, no
# randomness (the /ID is a fixed value). The user password is "rivet"; an
# owner password "owner-secret" also decrypts. PDFium must reject an empty or
# wrong password with FPDF_ERR_PASSWORD and accept "rivet".

PAD = (b"\x28\xBF\x4E\x5E\x4E\x75\x8A\x41\x64\x00\x4E\x56\xFF\xFA\x01\x08"
       b"\x2E\x2E\x00\xB6\xD0\x68\x3E\x80\x2F\x0C\xA9\xFE\x64\x53\x69\x7A")


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for byte in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(byte ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def padded(password: bytes) -> bytes:
    return (password + PAD)[:32]


def r3_owner_hash(owner_password: bytes, user_password: bytes) -> bytes:
    digest = hashlib.md5(padded(owner_password)).digest()
    for _ in range(50):
        digest = hashlib.md5(digest[:16]).digest()
    key = digest[:16]
    result = rc4(key, padded(user_password))
    for i in range(1, 20):
        result = rc4(bytes(b ^ i for b in key), result)
    return result


def r3_encryption_key(user_password: bytes, o_value: bytes, p: int, doc_id: bytes) -> bytes:
    digest = hashlib.md5(padded(user_password) + o_value +
                         struct.pack("<i", p) + doc_id).digest()
    for _ in range(50):
        digest = hashlib.md5(digest[:16]).digest()
    return digest[:16]


def r3_user_value(key: bytes, doc_id: bytes) -> bytes:
    import hashlib as _h
    value = rc4(key, _h.md5(PAD + doc_id).digest())
    for i in range(1, 20):
        value = rc4(bytes(b ^ i for b in key), value)
    return value + b"\x00" * 16  # 16 arbitrary bytes per the R3 algorithm


def object_key(key: bytes, obj_num: int, gen_num: int) -> bytes:
    return hashlib.md5(key + struct.pack("<i", obj_num)[:3] +
                       struct.pack("<i", gen_num)[:2]).digest()[:16]


def build_encrypted_pdf(objects: list[bytes], encrypt_obj_num: int,
                        encrypt_dict: bytes, doc_id: bytes, key: bytes) -> bytes:
    """build_pdf variant: RC4-encrypts every stream/byte-string with the
    per-object key and appends the /Encrypt dict + /ID to the trailer. The
    Encrypt dict itself and the trailer ID stay unencrypted (spec exemption)."""
    import re as _re
    header = b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n"
    body = b""
    offsets = []
    for number, obj in enumerate(objects, start=1):
        offsets.append(len(header) + len(body))
        if number == encrypt_obj_num:
            payload = obj
        else:
            payload = _encrypt_strings_and_streams(obj, key, number)
        body += b"%d 0 obj\n" % number + payload + b"\nendobj\n"
    xref_pos = len(header) + len(body)
    count = len(objects) + 1
    xref = b"xref\n0 %d\n0000000000 65535 f \n" % count
    xref += b"".join(b"%010d 00000 n \n" % offset for offset in offsets)
    id_hex = doc_id.hex().encode()
    trailer = (b"trailer\n<< /Size %d /Root 1 0 R /Encrypt %d 0 R /ID [<%s> <%s>] >>\n"
               b"startxref\n%d\n%%%%EOF\n" % (count, encrypt_obj_num, id_hex, id_hex, xref_pos))
    return header + body + xref + trailer


def _encrypt_strings_and_streams(obj: bytes, key: bytes, obj_num: int) -> bytes:
    """Encrypts the stream payload of one object and any literal/hex strings
    OUTSIDE the dictionary part. For our fixtures the only strings live in the
    dictionaries we deliberately keep string-free, so encrypting the stream
    suffices; strings inside dicts would need real PDF parsing."""
    import re as _re
    match = _re.search(rb"stream\r?\n", obj)
    if match is None:
        return obj
    head, tail = obj[:match.end()], obj[match.end():]
    end = tail.rfind(b"endstream")
    payload, rest = tail[:end], tail[end:]
    encrypted = rc4(object_key(key, obj_num, 0), payload)
    # The /Length must now describe the SAME length (RC4 keeps length).
    head = _re.sub(rb"/Length \d+", b"/Length %d" % len(encrypted), head)
    return head + encrypted + rest


# ---------------- page-editing fixtures ----------------
#
# Layout: object 1 catalog, object 2 pages, then (page dict, content stream)
# per page, then the shared Helvetica font last.


def boxed_pages_pdf(pages: list[tuple[bytes, bytes]]) -> bytes:
    """pages: (extra page-dict entries incl. /MediaBox, content stream)."""
    count = len(pages)
    font_obj = 3 + 2 * count
    kids = b" ".join(b"%d 0 R" % (3 + 2 * i) for i in range(count))
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [" + kids + b"] /Count %d >>" % count,
    ]
    for i, (entries, stream) in enumerate(pages):
        objects.append(b"<< /Type /Page /Parent 2 0 R " + entries +
                       b" /Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>"
                       % (font_obj, 3 + 2 * i + 1))
        objects.append(b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"endstream")
    objects.append(HELVETICA_FONT)
    return build_pdf(objects)


def labeled_content(size: int, x: int, y: int, label: bytes,
                    blocks: list[tuple[int, int, int, int]]) -> bytes:
    lines = [text_line(float(size), x, y, label), b"0 0 0 rg"]
    lines += [b"%d %d %d %d re f" % block for block in blocks]
    return b"\n".join(lines) + b"\n"


def markers_pdf() -> bytes:
    pages = []
    for i in range(1, 6):
        stream = labeled_content(48, 72, 700, b"PAGE-%d" % i, [(72 + 100 * (i - 1), 100, 60, 60)])
        pages.append((b"/MediaBox [0 0 612 792]", stream))
    return boxed_pages_pdf(pages)


def import_pdf() -> bytes:
    pages = []
    for i in range(1, 4):
        media = b"/MediaBox [0 0 792 612]" if i == 2 else b"/MediaBox [0 0 612 792]"
        stream = labeled_content(36, 72, 500, b"IMPORT-%d" % i, [(400 + 80 * (i - 1), 50, 40, 40)])
        pages.append((media, stream))
    return boxed_pages_pdf(pages)


def cropbox_pdf() -> bytes:
    crop_stream = labeled_content(24, 100, 400, b"CROP", [(36, 720, 36, 36), (0, 0, 30, 30)])
    crop_entries = b"/MediaBox [0 0 612 792] /CropBox [36 36 576 756]"
    offset_stream = labeled_content(24, 120, 400, b"OFFSET", [(100, 570, 30, 30)])
    return boxed_pages_pdf([
        (crop_entries, crop_stream),
        (crop_entries + b" /Rotate 90", crop_stream),
        (b"/MediaBox [100 200 400 600]", offset_stream),
    ])


# ---------------- annotation fixture ----------------


def annots_pdf() -> bytes:
    objects: list[bytes] = [b""] * 9  # 1 catalog, 2 pages, 3-5 pages, 6-8 content, 9 page-2 /Annots

    def add(obj: bytes) -> int:
        objects.append(obj)
        return len(objects)

    def stream(content: bytes, bbox: tuple[float, float, float, float]) -> int:
        dict_ = b"<< /Type /XObject /Subtype /Form /BBox [%s] /Length %d >>" % (
            b" ".join(b"%g" % v for v in bbox), len(content))
        return add(dict_ + b"\nstream\n" + content + b"\nendstream")

    def box(rect: tuple[float, float, float, float]) -> bytes:
        return b"[" + b" ".join(b"%g" % v for v in rect) + b"]"

    def quad(rect: tuple[float, float, float, float]) -> bytes:
        l, b, r, t = rect  # PDFium/Acrobat order: TL TR BL BR
        return b"[%g %g %g %g %g %g %g %g]" % (l, t, r, t, l, b, r, b)

    def annot(subtype: bytes, rect, extra: bytes, ap: int | None, flags: int = 4) -> int:
        entry = b"<< /Type /Annot /Subtype /" + subtype + b" /Rect " + box(rect) + b" /F %d " % flags + extra
        if ap is not None:
            entry += b" /AP << /N %d 0 R >>" % ap
        return add(entry + b" >>")

    annots1: list[int] = []

    r = (100, 700, 300, 720)
    annots1.append(annot(b"Highlight", r, b"/QuadPoints " + quad(r) + b" /C [1 1 0] /CA 0.5 /Contents (Highlighted)",
                         stream(b"1 1 0 rg 100 700 200 20 re f", r)))
    r = (100, 660, 300, 680)
    annots1.append(annot(b"Underline", r, b"/QuadPoints " + quad(r) + b" /C [0 0.5 0]", None))
    r = (100, 620, 300, 640)
    annots1.append(annot(b"StrikeOut", r, b"/QuadPoints " + quad(r) + b" /C [1 0 0]",
                         stream(b"1 0 0 RG 1 w 100 630 m 300 630 l S", r)))
    r = (400, 700, 420, 720)
    note_ap = stream(b"1 0.8 0 rg 400 700 20 20 re f", r)
    note_num = len(objects) + 1
    contents = "\ufeffПривет, мир".encode("utf-16-be").hex().upper().encode()
    annots1.append(annot(b"Text", r, b"/C [1 0.8 0] /Contents <" + contents + b"> /T (Tester) /NM (note-1) "
                         b"/Popup %d 0 R" % (note_num + 1), note_ap))
    annots1.append(add(b"<< /Type /Annot /Subtype /Popup /Parent %d 0 R /Rect [430 600 600 700] /F 0 /Open false >>"
                       % note_num))
    r = (99, 499, 221, 561)
    annots1.append(annot(b"Ink", r, b"/InkList [[100 500 130 540 160 560] [180 560 200 520 220 500]] "
                         b"/C [0 0 1] /BS << /W 2 >>",
                         stream(b"0 0 1 RG 2 w 100 500 m 130 540 l 160 560 l S 180 560 m 200 520 l 220 500 l S", r)))
    r = (300, 500, 400, 560)
    annots1.append(annot(b"Square", r, b"/C [0 0.5 0] /IC [0.8 1 0.8] /Border [0 0 3]",
                         stream(b"0.8 1 0.8 rg 0 0.5 0 RG 3 w 301.5 501.5 97 57 re B", r)))
    r = (420, 500, 520, 560)
    annots1.append(annot(b"Circle", r, b"/C [1 0 1] /BS << /W 4 >>",
                         stream(b"1 0 1 RG 4 w 422 502 m 518 502 l 518 558 l 422 558 l h S", r)))
    r = (95, 395, 305, 455)
    annots1.append(annot(b"Line", r, b"/L [100 400 300 450] /LE [/None /OpenArrow] /C [0 0 0]",
                         stream(b"0 0 0 RG 1 w 100 400 m 300 450 l S", r)))
    r = (100, 300, 250, 350)
    annots1.append(annot(b"Stamp", r, b"/Name /Approved /C [0 0.6 0] /NM (stamp-1)",
                         stream(b"0 0.6 0 RG 3 w 102 302 146 46 re S", r)))
    r = (300, 300, 450, 350)
    annots1.append(annot(b"FreeText", r, b"/DA (0 0 0 rg /Helv 12 Tf) /Contents (Free)",
                         stream(b"0.9 0.9 0.9 rg 300 300 150 50 re f", r)))
    r = (100, 250, 300, 270)
    annots1.append(annot(b"Squiggly", r, b"/QuadPoints " + quad(r) + b" /C [0 0 0]",
                         stream(b"0 0 0 RG 1 w 100 252 m 300 252 l S", r)))

    r = (100, 100, 200, 160)
    sq2 = annot(b"Square", r, b"/C [1 0 0] /Border [0 0 2]", stream(b"1 0 0 RG 2 w 101 101 98 58 re S", r))
    objects[8] = b"[%d 0 R]" % sq2
    r = (150, 150, 250, 250)
    sq3 = annot(b"Square", r, b"/C [0 0 1] /Border [0 0 2]", stream(b"0 0 1 RG 2 w 151 151 98 98 re S", r))

    empty = b"<< /Length 0 >>\nstream\n\nendstream"
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R >>"
    objects[1] = b"<< /Type /Pages /Kids [3 0 R 4 0 R 5 0 R] /Count 3 >>"
    objects[2] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 6 0 R /Rotate 0 "
                  b"/Annots [" + b" ".join(b"%d 0 R" % n for n in annots1) + b"] >>")
    objects[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 7 0 R "
                  b"/Rotate 90 /Annots 9 0 R >>")
    objects[4] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /CropBox [100 100 500 700] "
                  b"/Resources << >> /Contents 8 0 R /Annots [%d 0 R] >>" % sq3)
    objects[5] = objects[6] = objects[7] = empty
    return build_pdf(objects)


# ---------------- hostile annotation fixtures ----------------


def objstm_pdf(objects: list[tuple[bytes, bool]]) -> bytes:
    """PDF 1.5 file; entries flagged True (plain non-stream objects) are packed
    into one Flate-compressed object stream, the xref is a Flate xref stream."""
    import zlib
    header = b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n"
    count = len(objects)
    stm_num, xref_num = count + 1, count + 2
    body = b""
    entries: list[tuple[int, int, int]] = []  # (type, field2, field3) for objects 1..count
    packed: list[tuple[int, bytes]] = []
    for number, (obj, compress) in enumerate(objects, start=1):
        if compress:
            entries.append((2, stm_num, len(packed)))
            packed.append((number, obj))
        else:
            entries.append((1, len(header) + len(body), 0))
            body += b"%d 0 obj\n" % number + obj + b"\nendobj\n"
    head = b""
    data = b""
    for number, obj in packed:
        head += b"%d %d " % (number, len(data))
        data += obj + b"\n"
    payload = zlib.compress(head + data, 9)
    stm_offset = len(header) + len(body)
    body += (b"%d 0 obj\n<< /Type /ObjStm /N %d /First %d /Filter /FlateDecode /Length %d >>\nstream\n"
             % (stm_num, len(packed), len(head), len(payload)) + payload + b"\nendstream\nendobj\n")
    xref_offset = len(header) + len(body)
    rows = b"\x00" + (0).to_bytes(4, "big") + (65535).to_bytes(2, "big")
    for kind, f2, f3 in entries:
        rows += bytes([kind]) + f2.to_bytes(4, "big") + f3.to_bytes(2, "big")
    rows += b"\x01" + stm_offset.to_bytes(4, "big") + (0).to_bytes(2, "big")
    rows += b"\x01" + xref_offset.to_bytes(4, "big") + (0).to_bytes(2, "big")
    xref = zlib.compress(rows, 9)
    body += (b"%d 0 obj\n<< /Type /XRef /Size %d /W [1 4 2] /Root 1 0 R /Filter /FlateDecode /Length %d >>\n"
             b"stream\n" % (xref_num, count + 3, len(xref)) + xref + b"\nendstream\nendobj\n")
    return header + body + b"startxref\n%d\n%%%%EOF\n" % xref_offset


def annots_many_pdf() -> bytes:
    count = 4200
    ap = b"1 0 0 RG 1 w 101 101 38 38 re S"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> /Contents 4 0 R /Annots ["
        + b" ".join([b"5 0 R"] * count) + b"] >>",
        b"<< /Length 0 >>\nstream\n\nendstream",
        b"<< /Type /Annot /Subtype /Square /Rect [100 100 140 140] /F 4 /C [1 0 0] /Border [0 0 1] "
        b"/AP << /N 6 0 R >> >>",
        b"<< /Type /XObject /Subtype /Form /BBox [100 100 140 140] /Length %d >>\nstream\n" % len(ap)
        + ap + b"\nendstream",
    ]
    return build_pdf(objects)


def hostile_pdf() -> bytes:
    page_count = 8
    first_page, content = 3, 3 + page_count
    objects: list[tuple[bytes, bool]] = [(b"", False)] * (content)  # 1..content
    annots_of: dict[int, bytes] = {}

    def add(obj: bytes, compress: bool = True) -> int:
        objects.append((obj, compress))
        return len(objects)

    def annot(subtype: bytes, extra: bytes = b"", rect: bytes = b"[100 100 140 140]") -> bytes:
        return b"<< /Type /Annot /Subtype /" + subtype + b" /Rect " + rect + b" /F 4 " + extra + b" >>"

    def numbers(values) -> bytes:
        return b"[" + b" ".join(b"%g" % v for v in values) + b"]"

    def quads(n: int) -> bytes:
        # Identical quads: compresses to almost nothing.
        return b"[" + b" ".join([b"10 16 18 16 10 10 18 10"] * n) + b"]"

    def stroke(n: int) -> bytes:
        return b"[" + b" ".join([b"10 20"] * n) + b"]"

    # --- page 0: string limits
    annots_of[0] = b" ".join(b"%d 0 R" % add(o) for o in (
        annot(b"Text", b"/C [1 0.8 0] /Contents (" + b"x" * 65537 + b")"),
        annot(b"Text", b"/C [1 0.8 0] /T (" + b"a" * 1025 + b")"),
        annot(b"Text", b"/C [1 0.8 0] /NM (" + b"n" * 257 + b")"),
        annot(b"Text", b"/C [1 0.8 0] /Contents (" + b"y" * 65536 + b")"),
        annot(b"Text", b"/C [1 0.8 0] /Contents (plain) /T (Tester) /NM (plain-1)"),
    ))

    # --- page 1: geometry limits
    big_ink = b"[" + b" ".join([stroke(9000)] * 6) + b"]"
    annots_of[1] = b" ".join(b"%d 0 R" % add(o) for o in (
        annot(b"Highlight", b"/C [1 1 0] /QuadPoints " + quads(4097)),
        annot(b"Highlight", b"/C [1 1 0] /QuadPoints " + quads(4096)),
        annot(b"Highlight", b"/C [1 1 0] /QuadPoints [1 2 3 4 5 6 7]"),
        annot(b"Highlight", b"/C [1 1 0] /QuadPoints [(a) /b null true 1 2 3 4]"),
        annot(b"Highlight", b"/C [1 1 0]"),
        annot(b"Ink", b"/C [0 0 1] /InkList [" + stroke(10001) + b"]"),
        annot(b"Ink", b"/C [0 0 1] /InkList [" + b" ".join([b"[10 10 20 20]"] * 257) + b"]"),
        annot(b"Ink", b"/C [0 0 1] /InkList []"),
        annot(b"Ink", b"/C [0 0 1] /InkList [1 2 3]"),
        annot(b"Ink", b"/C [0 0 1] /InkList [[1 2 3]]"),
        annot(b"Ink", b"/C [0 0 1] /InkList " + big_ink),
        annot(b"Ink", b"/C [0 0 1] /InkList [[100 500 130 540 160 560]]"),
    ))

    # --- page 2: malformed entries (direct objects and dangling references)
    valid = add(annot(b"Square", b"/C [0 0.5 0] /Border [0 0 2]", b"[300 300 360 360]"))
    annots_of[2] = (b"null 42 9999 0 R (string) true "
                    + b"<< /Type /Annot /Subtype /Square /C [1 0 0] >> "
                    + b"<< /Type /Annot /Subtype /Square /Rect [200 200 100 100] /C [0 0 1] >> "
                    + b"<< /Type /Annot /Subtype /Square /Rect [0 0 340282346638528859811704183484516925440 "
                    + b"1000000000000000000000000000000] /C [0 0 1] >> "
                    + b"<< /Type /Annot /Subtype /Square /Rect [(a) /b null 10] /C [0 0 1] >> "
                    + b"<< /Type /Annot /Rect [10 10 50 50] >> "
                    + b"<< /Type /Annot /Subtype /Bogus /Rect [10 10 50 50] >> "
                    + b"<< /Type /Annot /Subtype /Square /Rect [10 10 20] /C [0 0 1] >> "
                    + b"%d 0 R" % valid)

    # --- pages 3, 4: /Annots of the wrong type
    annots_of[3] = None  # patched below
    annots_of[4] = None

    # --- page 5: relations and actions. Object numbers are allocated first so
    # that the annotations can reference each other.
    base = len(objects) + 1
    self_text, self_popup, foreign_text = base, base + 1, base + 2
    irt_a, irt_b = base + 3, base + 4
    foreign_popup = base + 9 + 0  # patched after the page-6 objects exist
    group = [
        annot(b"Text", b"/C [1 0.8 0] /Popup %d 0 R" % self_text),
        annot(b"Popup", b"/Parent %d 0 R /Open true" % self_popup),
        annot(b"Text", b"/C [1 0.8 0] /Popup %d 0 R" % (base + 9)),
        annot(b"Text", b"/C [1 0.8 0] /IRT %d 0 R /RT /R" % irt_b),
        annot(b"Text", b"/C [1 0.8 0] /IRT %d 0 R /RT /R" % irt_a),
        annot(b"FileAttachment", b"/FS << /Type /Filespec /F (payload.bin) >> /Name /PushPin"),
        annot(b"Link", b"/Border [0 0 0] /A << /S /JavaScript /JS (app.alert\\(1\\)) >>"),
        annot(b"Link", b"/Border [0 0 0] /A << /S /Launch /F (calc.exe) >>"),
        annot(b"Widget", b"/FT /Tx /T (field1) /V (value)"),
    ]
    nums = [add(o) for o in group]
    assert nums[0] == self_text and nums[1] == self_popup and nums[2] == foreign_text and nums[3] == irt_a
    annots_of[5] = b" ".join(b"%d 0 R" % n for n in nums)

    # --- page 6: the foreign popup + its parent (the page-5 note above)
    popup6 = add(b"<< /Type /Annot /Subtype /Popup /Rect [400 400 500 480] /Parent %d 0 R /Open true >>" % foreign_text)
    assert popup6 == base + 9
    parent6 = add(annot(b"Text", b"/C [0.5 0.5 1] /Popup %d 0 R /Contents (parent)" % popup6))
    annots_of[6] = b"%d 0 R %d 0 R" % (popup6, parent6)

    # --- page 7: UTF-16BE contents with surrogate problems
    def utf16(units: list[int]) -> bytes:
        return b"<FEFF" + b"".join(b"%04X" % u for u in units) + b">"

    annots_of[7] = b" ".join(b"%d 0 R" % add(o) for o in (
        annot(b"Text", b"/C [1 0.8 0] /Contents " + utf16([0x41, 0xD800, 0x42])),
        annot(b"Text", b"/C [1 0.8 0] /Contents " + utf16([0x43, 0xDC00, 0x44])),
        annot(b"Text", b"/C [1 0.8 0] /Contents " + utf16([0x45, 0xD83D, 0xDE00, 0x46])),
    ))

    objects[0] = (b"<< /Type /Catalog /Pages 2 0 R >>", False)
    kids = b" ".join(b"%d 0 R" % (first_page + i) for i in range(page_count))
    objects[1] = (b"<< /Type /Pages /Kids [" + kids + b"] /Count %d >>" % page_count, False)
    for i in range(page_count):
        if i == 3:
            annots = b" /Annots 42"
        elif i == 4:
            annots = b" /Annots << /Subtype /Square /Rect [10 10 50 50] >>"
        else:
            annots = b" /Annots [" + annots_of[i] + b"]"
        objects[first_page - 1 + i] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << >> "
                                       b"/Contents %d 0 R" % content + annots + b" >>", False)
    objects[content - 1] = (b"<< /Length 0 >>\nstream\n\nendstream", False)
    return objstm_pdf(objects)


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

    # password.pdf: 4 pages, standard security handler R3 (RC4-128), user
    # password "rivet", owner password "owner-secret". Deterministic /ID.
    page_count = 4
    objects = []
    kids = []
    for i in range(page_count):
        page_num = 3 + 2 * i
        kids.append(b"%d 0 R" % page_num)
        content = b"BT /F1 24 Tf 72 720 Td (Page %d of %d) Tj ET\n" % (i + 1, page_count)
        objects.append(None)  # placeholder for the page dict (built below)
    # Rebuild with real dicts; objects: 1 catalog, 2 pages, then per page
    # (page dict, content), then font, then Encrypt dict last.
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>",
               b"<< /Type /Pages /Kids [" + b" ".join(kids) + b"] /Count %d >>" % page_count]
    for i in range(page_count):
        content = b"BT /F1 24 Tf 72 720 Td (Page %d of %d) Tj ET\n" % (i + 1, page_count)
        font_ref = 3 + 2 * page_count
        objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources "
                       b"<< /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>"
                       % (font_ref, 3 + 2 * i + 1))
        objects.append(b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream")
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding "
                   b"/WinAnsiEncoding >>")
    encrypt_num = len(objects) + 1

    doc_id = b"rivet-password-fixture-id"
    user_pw = b"rivet"
    owner_pw = b"owner-secret"
    p_value = -3904
    o_value = r3_owner_hash(owner_pw, user_pw)
    key = r3_encryption_key(user_pw, o_value, p_value, doc_id)
    u_value = r3_user_value(key, doc_id)
    encrypt_dict = (b"<< /Filter /Standard /V 2 /R 3 /Length 128 /P %d /O <%s> /U <%s> >>"
                    % (p_value, o_value.hex().encode(), u_value.hex().encode()))
    objects.append(encrypt_dict)  # stored unencrypted (spec exemption)
    write("password.pdf", build_encrypted_pdf(objects, encrypt_num, encrypt_dict, doc_id, key))

    # Page-editing fixtures (see the module docstring).
    write("markers-5.pdf", markers_pdf())
    write("import-3.pdf", import_pdf())
    write("cropbox.pdf", cropbox_pdf())

    # Annotation fixture (see the module docstring).
    write("annots.pdf", annots_pdf())
    write("annots-many.pdf", annots_many_pdf())
    write("annots-hostile.pdf", hostile_pdf())


if __name__ == "__main__":
    main()
