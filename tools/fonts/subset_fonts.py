#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Regenerates the fonts Rivet bundles (ADR-0016).

Subsets Arimo / Tinos / Cousine (Regular + Bold, Apache-2.0) to the character
set below and writes:

  third_party/fonts/{Arimo,Tinos,Cousine}-{Regular,Bold}.ttf
  src/pdf/BundledFontCoverage.inc   (sorted code point ranges per face)

This is an OFFLINE maintainer tool: the build never runs it. The generated
files are committed. fontTools is needed only here:

  python3 -m venv <dir> && <dir>/bin/pip install fonttools
  <dir>/bin/python tools/fonts/subset_fonts.py --source <dir with the full fonts>

The source fonts are the full Arimo-*/Tinos-*/Cousine-* TrueType files, e.g.
from PDFium's third_party/test_fonts/test_fonts/ or
https://github.com/google/fonts/tree/main/apache/{arimo,tinos,cousine}.

Kept tables: cmap glyf loca hmtx hhea head maxp name OS/2 post. Hinting
programs, layout tables (GSUB/GPOS/kern) and everything else are dropped:
Rivet positions glyphs itself (no shaping, no kerning).
"""

import argparse
import copy
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTFont

# (face index in PdfBundledFont order, source file name, output file name)
FACES = [
    ("Arimo-Regular.ttf", "SansRegular"),
    ("Arimo-Bold.ttf", "SansBold"),
    ("Tinos-Regular.ttf", "SerifRegular"),
    ("Tinos-Bold.ttf", "SerifBold"),
    ("Cousine-Regular.ttf", "MonoRegular"),
    ("Cousine-Bold.ttf", "MonoBold"),
]

# Inclusive ranges (ADR-0016): Basic Latin, Latin-1 Supplement + Latin
# Extended-A, Greek and Coptic, Cyrillic, General Punctuation, Currency
# Symbols, Letterlike Symbols, four arrows, minus sign, replacement character.
RANGES = [
    (0x0020, 0x007E),
    (0x00A0, 0x017F),
    (0x0370, 0x03FF),
    (0x0400, 0x04FF),
    (0x2000, 0x206F),
    (0x20A0, 0x20CF),
    (0x2100, 0x214F),
    (0x2190, 0x2193),
    (0x2212, 0x2212),
    (0xFFFD, 0xFFFD),
]

KEEP_TABLES = {"cmap", "glyf", "loca", "hmtx", "hhea", "head", "maxp", "name", "OS/2", "post"}

# OS/2 fsType: 0 = installable, bit 3 (0x8) = editable. Restricted (0x2),
# preview & print only (0x4), no-subsetting (0x100) are refused.
def embedding_allowed(fs_type):
    if fs_type & 0x2 or fs_type & 0x100:
        return False
    return fs_type == 0 or bool(fs_type & 0x8)


def wanted_codepoints():
    cps = []
    for lo, hi in RANGES:
        cps.extend(range(lo, hi + 1))
    return cps


def ranges_of(codepoints):
    out = []
    for cp in sorted(codepoints):
        if out and cp == out[-1][1] + 1:
            out[-1][1] = cp
        else:
            out.append([cp, cp])
    return out


def unique_glyph_per_codepoint(font):
    """Gives every code point its own glyph.

    PDFium builds the ToUnicode map of a font it embeds by inverting the
    font's cmap, and when several code points share one glyph (NBSP and
    space, soft hyphen and hyphen, Greek question mark and semicolon, ...)
    text extraction returns the LAST one - a space typed in Rivet would come
    back as U+00A0 and break search and copy. The duplicates are copies of
    the shared glyph, so rendering is unchanged. Idempotent.
    """
    cmap = font.getBestCmap()
    owners = {}
    for cp in sorted(cmap):
        owners.setdefault(cmap[cp], []).append(cp)
    order = list(font.getGlyphOrder())
    glyf = font["glyf"]
    hmtx = font["hmtx"]
    for glyph, cps in owners.items():
        for cp in cps[1:]:
            name = f"{glyph}.u{cp:04X}"
            glyf.glyphs[name] = copy.deepcopy(glyf[glyph])
            hmtx.metrics[name] = hmtx.metrics[glyph]
            order.append(name)
            for table in font["cmap"].tables:
                if table.isUnicode() and table.cmap.get(cp) == glyph:
                    table.cmap[cp] = name
    font.setGlyphOrder(order)
    glyf.glyphOrder = order


def subset_one(src_path, dst_path):
    font = TTFont(src_path)
    fs_type = font["OS/2"].fsType
    if not embedding_allowed(fs_type):
        raise SystemExit(f"{src_path}: OS/2 fsType {fs_type:#x} does not allow embedding")

    cmap = font.getBestCmap()
    present = [cp for cp in wanted_codepoints() if cp in cmap]
    if not present:
        raise SystemExit(f"{src_path}: none of the wanted code points are present")

    options = subset.Options()
    options.hinting = False
    options.layout_features = []
    options.notdef_outline = True
    options.glyph_names = False
    options.legacy_kern = False
    options.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    options.name_languages = [0x409]
    options.drop_tables += ["DSIG", "GDEF", "GPOS", "GSUB", "kern", "gasp", "fpgm", "prep", "cvt "]
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=present)
    subsetter.subset(font)

    for tag in list(font.keys()):
        if tag != "GlyphOrder" and tag not in KEEP_TABLES:
            del font[tag]
    unique_glyph_per_codepoint(font)
    font.save(dst_path)

    # Re-read what was actually written and report its coverage.
    written = TTFont(dst_path)
    covered = [cp for cp in written.getBestCmap() if any(lo <= cp <= hi for lo, hi in RANGES)]
    return ranges_of(covered), os.path.getsize(dst_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", required=True, help="directory holding the full Arimo/Tinos/Cousine TTFs")
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    parser.add_argument("--out", default=os.path.join(root, "third_party", "fonts"))
    parser.add_argument("--coverage", default=os.path.join(root, "src", "pdf", "BundledFontCoverage.inc"))
    args = parser.parse_args()

    os.makedirs(args.out, exist_ok=True)
    tables = []
    for name, enumerator in FACES:
        ranges, size = subset_one(os.path.join(args.source, name), os.path.join(args.out, name))
        print(f"{name}: {size} bytes, {sum(hi - lo + 1 for lo, hi in ranges)} code points")
        tables.append((enumerator, name, ranges))

    with open(args.coverage, "w", encoding="utf-8") as f:
        f.write("// SPDX-License-Identifier: MPL-2.0\n")
        f.write("// GENERATED by tools/fonts/subset_fonts.py - do not edit.\n")
        f.write("// Sorted, non-overlapping inclusive code point ranges per bundled face,\n")
        f.write("// in PdfBundledFont order.\n")
        for enumerator, name, ranges in tables:
            f.write(f"// {enumerator}: {name}\n")
            f.write(f"inline constexpr CodepointRange kCoverage{enumerator}[] = {{\n")
            for lo, hi in ranges:
                f.write(f"    {{0x{lo:04X}, 0x{hi:04X}}},\n")
            f.write("};\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
