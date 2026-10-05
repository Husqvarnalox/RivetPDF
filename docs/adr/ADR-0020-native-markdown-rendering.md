# ADR-0020: Native Markdown rendering without a web engine

Status: Accepted

Date: 2026-10-05

## Context

Rivet must render Markdown natively without resorting to WebView or embedding a browser (ADR-0002).
The rendering pipeline must use the same retained-mode widget system and paint abstraction as PDF
rendering, so the UI layer remains independent of the backend.

## Decision

**Render pipeline:** `MarkdownDocument` → `markdown::layoutMarkdown()` → `MarkdownLayout` → `MarkdownPreviewView`
(a `ui::Widget`) → `PaintContext` primitives (fill, text, line).

**Layout engine** (`src/markdown/MarkdownLayout.hpp`, `MarkdownLayout.cpp`):
- Pure function: `layoutMarkdown(MarkdownDocument, width, ITextMeasurer, IImageSizeProvider) → MarkdownLayout`.
- No painting, no platform code. Text metrics come from an abstract `ITextMeasurer` (tested with fixed-width fakes).
- Image sizes come from `IImageSizeProvider`; images are loaded asynchronously by the view.
- Output: positioned lines, runs, decorations, links, and hit-test helpers, all in document space (origin top-left,
  y-down).
- Complexity: O(N) in document size; `MeasureCache` avoids re-measuring the same (text, style) pair across
  width changes.

**Typography and spacing** (`Typography` struct):
- Font sizes per text kind (Body, H1–H6, Code, Quote, Table, Link).
- Line-height factor, margins, gaps, indentation — all configurable constants, no hard-coded pixels.

**Rendering** (`MarkdownPreviewView`, a `ui::Widget`):
- Inherits from `ui::Widget` and `ISearchTarget`; paints through a `PaintContext` (same abstraction as
  PDF rendering).
- Visible blocks only: paint enumerates the layout blocks overlapping the viewport, avoids full-document
  traversal.
- Per-block horizontal overflow: wide tables and code blocks scroll horizontally on their own with shift+wheel
  input; the page itself never scrolls horizontally (a thin indicator shows the extent).
- Text selection, copy, and search: implemented through the layout's hit-test and text-extraction APIs.

**Link policy:**
- `http://`, `https://`, and `mailto:` links are offered to the shell's URL opener after a policy check.
- `#anchor` (internal fragments) scroll to the heading with that slug within the document.
- Relative file paths, absolute paths, and all other schemes are blocked (status message explains why).
- No JavaScript or form submission; links are inert until clicked.

**Images:**
- `MarkdownImageStore` (a shared component owned by the view) loads images asynchronously with bounds
  and type checks:
  - Local file:// URLs only; remote (http/https) are blocked immediately.
  - Image dimensions are clamped and timeouts apply.
  - Fallback: placeholder with alt text while loading, or when blocked/failed.
- Images are never logged; alt text and URLs are untrusted.

**Deferred features (not yet implemented):**
- Export to PDF / Print: requires integrating the layout with the PDF assembly pipeline (ADR-0017 for
  content editing; a similar Phase 3 task for Markdown).
- Mermaid diagrams: would need a diagram renderer; deferred pending a lightweight solution.
- Math rendering (LaTeX, MathML): deferred; KaTeX integration is an option.
- WYSIWYG visual editing in Rendered mode: the preview is read-only for now; text editing happens
  in Source mode.

## Consequences

- Pure layout (no platform code) makes the engine unit-testable with fakes and portable to future
  platforms.
- The view is just another `ui::Widget`; it uses the same paint and input systems as the PDF viewport.
- Selection and search use the layout's hit-test and text APIs; no regex or DOM needed.
- Per-block horizontal scroll keeps pages narrow and readable; users with wide displays can resize
  the window, and very wide tables/code still fit.
- Image loading is local-only and bounded; untrusted file servers cannot hang Rivet or exhaust memory.
- The shell remains in control of link policy; the view cannot open arbitrary URLs on click.

## Rejected alternatives

- **WebView/Electron**: violates ADR-0002; introduces a browser runtime, security surface, and
  dependency bloat.
- **HTML rendering with a C++ library** (e.g., Gumbo for parsing): Markdown → HTML → render adds
  unnecessary intermediate representation; direct Markdown → layout is simpler.
- **CSS-based layout engine** (e.g., Taffy from Rust, or a C++ port): more powerful than needed for
  Markdown's relatively fixed structure. Custom layout is simpler and doesn't require a generalist
  layout engine.
- **Delegating image loading to a URL fetch service**: local-only, bounded, timeout-aware image loading
  is a deliberate security/privacy choice.
