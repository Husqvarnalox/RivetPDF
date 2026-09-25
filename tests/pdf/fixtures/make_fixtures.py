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


if __name__ == "__main__":
    main()
