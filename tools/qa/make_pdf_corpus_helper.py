#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Helper for tools/qa/make-acrobat-corpus.sh (dev/QA only).

  gen   plain|sigfield|xrefstream OUT.pdf   write a small source PDF
  annot IN.pdf OUT.pdf                      append an incremental revision adding a
                                            /Text annotation to page object 3
  flip  IN.pdf OUT.pdf                      flip one byte inside the signed range
                                            (first signature's /ByteRange)
"""
import re
import sys
import zlib


def content_stream(text):
    return ("BT /F1 24 Tf 72 700 Td (%s) Tj ET\n" % text).encode("latin-1")


def objs_common(title):
    # object numbers: 1 catalog, 2 pages, 3 page, 4 content, 5 font
    return {
        4: b"<< /Length %d >>\nstream\n" % len(content_stream(title)) + content_stream(title) + b"endstream",
        5: b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    }


def gen(kind, out):
    title = "Rivet Acrobat corpus: " + kind
    o = objs_common(title)
    if kind == "sigfield":
        o[1] = b"<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R] /SigFlags 3 >> >>"
        o[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
                b"/Resources << /Font << /F1 5 0 R >> >> /Annots [6 0 R] >>")
        o[6] = (b"<< /Type /Annot /Subtype /Widget /FT /Sig /T (SigField1) /F 4 "
                b"/Rect [72 100 272 160] /P 3 0 R >>")
    else:
        o[1] = b"<< /Type /Catalog /Pages 2 0 R >>"
        o[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R "
                b"/Resources << /Font << /F1 5 0 R >> >> >>")
    o[2] = b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>"
    n = max(o)
    buf = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offs = {}
    if kind == "xrefstream":
        for k in sorted(o):
            offs[k] = len(buf)
            buf += b"%d 0 obj\n" % k + o[k] + b"\nendobj\n"
        xo = len(buf)
        xnum = n + 1
        offs[xnum] = xo
        rows = bytearray()
        for k in range(0, xnum + 1):
            if k == 0:
                rows += bytes([0, 0, 0, 0, 0xFF])
            else:
                off = offs[k]
                rows += bytes([1]) + off.to_bytes(3, "big") + bytes([0])
        body = zlib.compress(bytes(rows))
        buf += (b"%d 0 obj\n<< /Type /XRef /Size %d /W [1 3 1] /Root 1 0 R /Filter /FlateDecode "
                b"/Length %d >>\nstream\n" % (xnum, xnum + 1, len(body))) + body + b"\nendstream\nendobj\n"
        buf += b"startxref\n%d\n%%%%EOF\n" % xo
    else:
        for k in sorted(o):
            offs[k] = len(buf)
            buf += b"%d 0 obj\n" % k + o[k] + b"\nendobj\n"
        xo = len(buf)
        buf += b"xref\n0 %d\n" % (n + 1)
        buf += b"0000000000 65535 f \n"
        for k in range(1, n + 1):
            buf += b"%010d 00000 n \n" % offs[k]
        buf += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (n + 1, xo)
    open(out, "wb").write(buf)


def annot(src, out):
    d = open(src, "rb").read()
    startxref = int(re.findall(rb"startxref\s+(\d+)\s+%%EOF\s*$", d)[-1])
    trailers = re.findall(rb"trailer\s*<<(.*?)>>\s*startxref", d, re.S)
    size = int(re.search(rb"/Size\s+(\d+)", trailers[-1]).group(1))
    root = re.search(rb"/Root\s+(\d+ \d+ R)", trailers[-1]).group(1)
    ms = list(re.finditer(rb"(?:^|\n)3 0 obj\n(.*?)\nendobj", d, re.S))
    page = ms[-1].group(1)
    new = size
    if b"/Annots [" in page:
        page2 = page.replace(b"/Annots [", b"/Annots [%d 0 R " % new, 1)
    elif re.search(rb"/Annots\s+\d+ 0 R", page):
        raise SystemExit("indirect /Annots not supported by this helper")
    else:
        page2 = page.rstrip()
        assert page2.endswith(b">>")
        page2 = page2[:-2] + b" /Annots [%d 0 R] >>" % new
    annot_obj = b"<< /Type /Annot /Subtype /Text /Rect [400 700 420 720] /Contents (post-sign note) /F 4 /P 3 0 R >>"
    buf = bytearray(d)
    if not buf.endswith(b"\n"):
        buf += b"\n"
    o3 = len(buf)
    buf += b"3 0 obj\n" + page2 + b"\nendobj\n"
    on = len(buf)
    buf += b"%d 0 obj\n" % new + annot_obj + b"\nendobj\n"
    xo = len(buf)
    buf += b"xref\n3 1\n%010d 00000 n \n%d 1\n%010d 00000 n \n" % (o3, new, on)
    buf += b"trailer\n<< /Size %d /Root %s /Prev %d >>\nstartxref\n%d\n%%%%EOF\n" % (new + 1, root, startxref, xo)
    open(out, "wb").write(buf)


def flip(src, out):
    d = bytearray(open(src, "rb").read())
    m = re.search(rb"/ByteRange\s*\[\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s*\]", d)
    a, b, c, e = (int(x) for x in m.groups())
    # a character of the page text: inside the first signed range
    pos = d.index(b"Rivet Acrobat corpus") + 8
    assert 0 < pos < a + b
    d[pos] ^= 0x01
    open(out, "wb").write(d)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "gen":
        gen(sys.argv[2], sys.argv[3])
    elif cmd == "annot":
        annot(sys.argv[2], sys.argv[3])
    elif cmd == "flip":
        flip(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
