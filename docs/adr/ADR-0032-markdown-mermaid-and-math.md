# ADR-0032: Mermaid diagrams and math in Markdown

Status: Accepted

Date: 2026-10-16

## Context

Markdown documents often contain ```` ```mermaid ```` blocks and TeX math
(`$...$`, `$$...$$`). Rivet has no web engine (ADR-0002), so neither
mermaid.js nor KaTeX/MathJax can run.

## Decision

1. **Native, portable, bounded renderers in `rivet_markdown`** producing
   layout primitives (text runs, lines, rects, rounded rects, paths/arrows)
   that the existing painter and the PDF exporter (ADR-0023) both draw, so
   preview, print and export match.
2. **Mermaid subset:** `flowchart`/`graph` (TD/TB/BT/LR/RL; node shapes
   rect, round, stadium, diamond, circle; edges `-->`, `---`, `-.->`, `==>`
   with labels; subgraphs one level) laid out with a layered (Sugiyama-style)
   algorithm: longest-path ranking, barycentric crossing reduction (bounded
   iterations), simple coordinate assignment, orthogonal-ish polyline edges;
   `sequenceDiagram` (participants/actors, `->>`, `-->>`, `->`, notes,
   `loop`/`alt`/`opt` boxes). Anything else, or a parse error, renders the
   source as a code block with a one-line notice. Limits: 500 nodes, 2000
   edges, 64 KiB source.
3. **Math subset:** inline `$...$` (not when `$` is followed by a digit/space
   pattern typical for money, per common heuristics) and display `$$...$$`
   blocks; enabled through MD4C's LaTeX math extension. A TeX-subset
   layout engine (boxes and glue): `\frac`, `\sqrt[n]{}`, sub/superscripts,
   `\sum \int \prod \lim` with limits, Greek letters, common operators and
   relations, `\left( \right)` delimiters sized by content, `\text{}`,
   `\mathbf`, `\mathit`, matrices (`pmatrix`, `bmatrix`, `cases`), accents
   (`\hat \bar \vec \dot`). Glyphs come from the bundled faces plus a small
   bundled math-symbol subset font (OFL/Apache-2.0, added to
   `third_party/fonts` with its license); unknown commands render in red
   as literal source. Limits on nesting depth and size.
4. Both are read-only islands in the WYSIWYG view (ADR-0031): edited in the
   Source view, re-rendered on the async parse path.

## Consequences

- Only the listed subsets; complex Mermaid types (gantt, class, state) and
  full LaTeX are out of scope.
- One new bundled font asset with a permissive license.

## Implementation notes

- Phase 1 (engines only, not yet wired into layout/preview/export):
  `src/markdown/mermaid/` (`Mermaid.hpp`: `renderMermaid`, `isMermaidFenceInfo`;
  `DiagramScene` of rects, paths, polylines and text with semantic colour and
  font roles; text measured with `diagramTextStyle()` through
  `ITextMeasurer`) and `src/markdown/math/` (`MathScene.hpp`: `typesetMath`
  with an `IMathMeasurer` that reports ink extents per font role Math / Text
  / TextBold; `MathDelimiters.hpp`: `findMathSpans`, `isInlineMathSpan`).
- The math face is Noto Sans Math 3.000 (SIL OFL 1.1), subset and bundled
  as `PdfBundledFont::MathRegular` (`third_party/fonts/README.md`). It has
  no spacing accents, so `\hat`/`\tilde` use ASCII `^`/`~`, `\dot` uses `.`
  and `\bar` a rule. Large delimiters, radicals and display operators are
  the base glyphs scaled (no OpenType MATH variants).
- Phase 2 (integration):
  - Parsing: MD4C's `MD_FLAG_LATEXMATHSPANS` gives verbatim TeX (no
    emphasis or escape processing inside) with exact source provenance;
    each `$...$` span MD4C matches is re-checked with `isInlineMathSpan`
    (the money rule) and otherwise turned back into its literal text.
    `$$...$$` inside a sentence is display-style math laid out inline; a
    paragraph that is only `$$...$$` stays a DisplayMath block.
  - Layout: inline formulas are atomic runs (`LayoutRun::graphic`) on the
    line's baseline; their ascent/descent grow the line, a formula wider
    than the line is scaled down. Display math and Mermaid blocks are
    `LayoutBlockKind::Graphic` blocks, centered, scaled by
    min(1, width / w, maxGraphicHeight / h) (the exporter caps the height
    at the page's content height; pagination treats them as atoms).
    Diagrams use `DiagramStyle` scaled by bodySize / 15. A block that
    cannot be rendered (unsupported diagram, parse error, limits) is its
    source as a read-only code block under a one-line `Notice`. Scenes are
    cached in `MeasureCache` keyed by renderer, font size / style scale and
    source text; they do not depend on the width, so neither typing
    elsewhere nor resizing re-renders them.
  - Layout runs on the main thread with the paint context bound (as for
    all text); only parsing is asynchronous. The cache keeps relayouts
    render-free.
  - Formula glyphs (`MathFont::Math`) are the bundled face everywhere:
    `BundledMathMeasurer` measures them with the PDF advances and the ink
    boxes of their outlines; the preview fills the outlines
    (`pdf::bundledGlyphOutline`) through `PaintContext::fillPath`, the
    exporter writes them as `MathRegular` text (Arimo for the few code
    points the subset lacks). This replaced the planned per-platform
    "load a font from memory" option: no platform code is needed and both
    outputs place each glyph at exactly the same position.
    `\text{}` runs and diagram labels use the body face (platform text in
    the preview, Arimo in the PDF), like ordinary Markdown text.
  - Colours: `app::markdownGraphicColor(palette, GraphicRole)` is the one
    role -> colour mapping (diagram roles, math errors, notices); the
    exporter uses it with the default palette.
  - PDF: `PdfBuildPath` (moves, lines, cubics, close; fill and/or stroke,
    dash array, round or miter joins) was added to `PdfBuild`, its
    validation and the PDFium builder. Rounded and dashed rectangles are
    written as paths; the preview cuts dashed strokes itself
    (`dashedPieces`) with the same pattern.
  - Known limit: Rivet's PDF text extraction reports characters outside
    the BMP (the mathematical italic letters) as U+FFFD per UTF-16 half.
