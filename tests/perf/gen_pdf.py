#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Usage: gen_pdf.py out.pdf pages [tag]   -- simple text pages (Helvetica).
import sys
out, n = sys.argv[1], int(sys.argv[2])
tag = sys.argv[3] if len(sys.argv) > 3 else "PAGE"
objs = []
def add(b): objs.append(b); return len(objs)
add(b"")  # 1 catalog placeholder
add(b"")  # 2 pages placeholder
font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
kids = []
for i in range(n):
    lines = "".join("BT /F1 12 Tf 72 %d Td (%s-%d line %d lorem ipsum dolor sit amet consectetur) Tj ET\n" % (720 - 14 * l, tag, i, l) for l in range(40))
    c = add(("<< /Length %d >>\nstream\n%sendstream" % (len(lines), lines)).encode())
    p = add(("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>" % (font, c)).encode())
    kids.append(p)
objs[0] = b"<< /Type /Catalog /Pages 2 0 R >>"
objs[1] = ("<< /Type /Pages /Count %d /Kids [%s] >>" % (n, " ".join("%d 0 R" % k for k in kids))).encode()
buf = bytearray(b"%PDF-1.4\n"); offs = []
for i, o in enumerate(objs):
    offs.append(len(buf)); buf += ("%d 0 obj\n" % (i + 1)).encode() + o + b"\nendobj\n"
x = len(buf)
buf += ("xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)).encode()
for o in offs: buf += ("%010d 00000 n \n" % o).encode()
buf += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)).encode()
open(out, "wb").write(buf)
