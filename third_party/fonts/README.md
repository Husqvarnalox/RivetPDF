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

## Regenerating

The build never runs this; the outputs are committed. fontTools is needed
only by the maintainer script:

```
python3 -m venv /tmp/fontvenv && /tmp/fontvenv/bin/pip install fonttools
/tmp/fontvenv/bin/python tools/fonts/subset_fonts.py --source <dir with the full Arimo/Tinos/Cousine TTFs>
```

Then rebuild; the fonts are re-embedded automatically (the embed step depends
on the `.ttf` files).
