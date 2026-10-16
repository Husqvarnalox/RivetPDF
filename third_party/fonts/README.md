# Bundled fonts

Fonts Rivet embeds for new and substituted text (ADR-0016). They are
compiled into `rivet_pdf` at build time by `cmake/EmbedFonts.cmake`.

| File | Family | Source version | Use |
|---|---|---|---|
| `Arimo-Regular.ttf`, `Arimo-Bold.ttf` | Arimo (Helvetica/Arial metrics) | 1.23 / see `name` table | Sans |
| `Tinos-Regular.ttf`, `Tinos-Bold.ttf` | Tinos (Times metrics) | see `name` table | Serif |
| `Cousine-Regular.ttf`, `Cousine-Bold.ttf` | Cousine (Courier metrics) | 1.21 / see `name` table | Mono |

- **Origin**: Google Fonts, <https://github.com/google/fonts/tree/main/apache>
  (`arimo`, `tinos`, `cousine`); the exact input files were the copies in
  PDFium's `third_party/test_fonts/test_fonts/` (Mar 2018 builds).
- **License**: Apache License 2.0 (`LICENSE` in this directory). The OS/2
  `fsType` of every source font is 0 (installable embedding); the script
  refuses fonts that do not allow embedding or subsetting.
- **Modifications**: subset only (no glyph edits). Character set: U+0020-007E,
  U+00A0-017F, U+0370-03FF, U+0400-04FF, U+2000-206F, U+20A0-20CF,
  U+2100-214F, U+2190-2193, U+2212, U+FFFD, intersected with what each font
  contains. Hinting programs and layout tables (GSUB/GPOS/kern) are dropped;
  kept tables: cmap glyf loca hmtx hhea head maxp name OS/2 post.
  One structural fix-up: code points that the source font maps to a shared
  glyph (for example U+00A0 and U+0020) get their own copy of the glyph.
  PDFium rebuilds the ToUnicode map of an embedded font by inverting the cmap,
  so a shared glyph would extract as the wrong character (a no-break space
  instead of a space). The copies are identical outlines and metrics.
- **Coverage table**: `src/pdf/BundledFontCoverage.inc` is generated together
  with the fonts and is what `bundledFontCovers()` consults.

## Noto Sans Math (math face, ADR-0032)

| File | Family | Source version | Use |
|---|---|---|---|
| `NotoSansMath-Regular.ttf` | Noto Sans Math Regular | 3.000 | Math (`PdfBundledFont::MathRegular`) |

- **Origin**: the official Noto release
  <https://github.com/notofonts/math/releases/tag/NotoSansMath-v3.000>,
  asset `NotoSansMath-v3.000.zip`
  (SHA-256 `ac351837b41f8a897f020b97fb0f075ad574c1e9669fb5839ada1f92fd748356`);
  input file `NotoSansMath/unhinted/ttf/NotoSansMath-Regular.ttf`
  (SHA-256 `b127e84699212b6b2ef50aff58e0ebebeec04ffe6db1b9eb9e209c8c3d97b4aa`).
- **License**: SIL Open Font License 1.1 (`LICENSE-OFL.txt` in this
  directory, copied from the release's `OFL.txt`; copyright 2022 The Noto
  Project Authors). No Reserved Font Name is declared. OS/2 `fsType` is 0.
- **Modifications**: subset only (`tools/fonts/subset_fonts.py --set math`),
  to the `MATH_RANGES` character set in the script (ASCII, Latin-1, Greek,
  accents, general punctuation, letterlike symbols, arrows, mathematical
  operators, miscellaneous technical, supplemental arrows/operators,
  mathematical alphanumeric bold/italic/double-struck letters and digits).
  The `MATH`, `GSUB`, `GPOS` and `GDEF` tables and hinting are dropped (the
  Rivet math engine positions glyphs itself); kept tables as above. Same
  shared-glyph fix-up as the text faces.
- **Output**: SHA-256 of the committed subset
  `1d37283e5628d4440586f45bcf4141876c007cde7ac1f20b3cbd003a9edd92a3`
  (190172 bytes); coverage table `src/pdf/BundledFontCoverageMath.inc`.

## Regenerating

The build never runs this; the outputs are committed. fontTools is needed
only by the maintainer script:

```
python3 -m venv /tmp/fontvenv && /tmp/fontvenv/bin/pip install fonttools
/tmp/fontvenv/bin/python tools/fonts/subset_fonts.py --source <dir with the full Arimo/Tinos/Cousine TTFs>
/tmp/fontvenv/bin/python tools/fonts/subset_fonts.py --set math --source <dir with the full NotoSansMath-Regular.ttf>
```

Then rebuild; the fonts are re-embedded automatically (the embed step depends
on the `.ttf` files).
