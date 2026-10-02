# ADR-0016: Font embedding and fallback

Status: Accepted (Phase 5)

Date: 2026-10-03

## Context

Writing new text needs a font that (a) contains the glyphs, (b) may legally
be embedded, (c) renders identically everywhere (so it must be embedded),
and (d) does not depend on the operating system (the portable layers must
build on Windows/Linux; CoreText is allowed only in the macOS platform
implementation and the content model must not depend on it).

## Decision

### Reuse the document's font when it can encode the text

For edits of existing text the block's own font (member 0) is reused when
`FPDFText_SetText` followed by `FPDFTextObj_GetText` round-trips the whole
new text and the font is not Type3. This keeps the original look for the
common case (fixing a typo with characters the font already has).

### Bundled fallback fonts

Rivet ships three families, Regular and Bold, compiled into `rivet_pdf`:

| Choice | Family | Source | License |
| --- | --- | --- | --- |
| Sans | Arimo | Chrome OS core fonts (PDFium test fonts) | Apache-2.0 |
| Serif | Tinos | same | Apache-2.0 |
| Mono | Cousine | same | Apache-2.0 |

They are metric-compatible with Arial/Times New Roman/Courier New, which
makes substitution in existing documents look reasonable. The files are
subset at build-preparation time (`tools/fonts/subset_fonts.py`, fontTools)
to Basic Latin, Latin-1, Latin Extended-A, Greek, Cyrillic, general
punctuation, currency symbols and a few common symbols, keeping hinting off;
the subset `.ttf` files and the Apache-2.0 license live in
`third_party/fonts/`. Their OS/2 fsType permits embedding (checked by the
script; installable embedding).

Fallback selection for an existing block: Mono when the original font is
monospace, Serif when it is serif, else Sans; Bold when the original is
bold. A block re-set in a bundled font is written as new text objects
appended on top of the page content (its original objects are removed), so
its z-order changes: it is drawn above everything else on the page. The
same applies to lines a block gains beyond its original object count. Italic is not shipped: italic originals fall back to the upright
face (documented).

### Embedding

New fonts are embedded with `FPDFText_LoadFont(..., FPDF_FONT_TRUETYPE,
cid = true)` (Type0/CIDFontType2 with Identity-H and a ToUnicode CMap) so
any BMP character of the subset can be written and copied back out.
PDFium embeds the WHOLE font program it is given (it does not subset),
which is why the shipped files are already subsets: each bundled face
used adds roughly its file size (about 60-120 KB) to the saved PDF, once
per document per face (fonts are cached per output document during an
assembly). Characters outside the subset cannot be written; the editor
refuses such a commit with a clear message instead of writing tofu.

### Not supported (documented)

- Using system fonts or user-chosen font files.
- Synthetic (fake) bold/italic.
- Embedding subsets generated at save time.
- Vertical writing modes for new text.

## Consequences

- Deterministic, OS-independent output; small and legally clean bundle.
- Documents gain one embedded font per bundled face actually used.
- Scripts beyond Latin/Greek/Cyrillic need a future font strategy.
