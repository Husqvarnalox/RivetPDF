# ADR-0019: Markdown parser and model

Status: Accepted

Date: 2026-10-05

## Context

Rivet needs to parse Markdown and build a model to drive layout and editing.

Design constraints:
- No web engine; parsing and rendering must be native and fast.
- The parser output must not grow unbounded for untrusted input (files can be arbitrarily large).
- The model must be **immutable and portable**: it is produced by a worker and consumed by layout,
  search, and the source editor without dependencies on UI, platform, or network code.
- The parser must not throw; errors must be recoverable and reported as diagnostics.
- **Source range fidelity** is needed to map rendered output back to the source text (for editing,
  selection, and search).

## Decision

**Parser backend:** Use **MD4C v0.6.0** (MIT-licensed, vendored at `third_party/md4c/`), built through a
Rivet-owned `IMarkdownParser` interface in `src/markdown/MarkdownParser.hpp`.

- `IMarkdownParser::parse()` is a pure, side-effect-free function: no state, no callbacks that escape
  the library, no retention of the input.
- `makeMarkdownParser()` returns a concrete MD4C-based parser (the default; other backends can be
  registered later if needed).

**Document model:** An immutable `MarkdownDocument` in `src/markdown/MarkdownModel.hpp` containing:

- **Blocks**: Paragraph, Heading, CodeBlock, Quote, List, Table, HorizontalRule, ImageBlock
  (a paragraph with only an image), and HtmlBlock (raw HTML kept as plain text, never rendered).
- **Inlines**: Text, Emphasis, Strong, Strike, Code, Link, Image, SoftBreak, HardBreak.
- **Every block and inline carries a `SourceRange`** (byte offsets into the source) so layout can
  map hits back to source offsets for editing.
- **Headings** are indexed by slug (generated with `kmarkdown::makeSlug`); the slug table supports
  anchor navigation (#section-name).
- **Diagnostics**: limits exceeded (nesting depth, block count, inline depth, code block size, table
  size) degrade gracefully—parsing does not fail, a Diagnostic records the limit hit, and the
  document continues. Images and HTML are not validated for remote/unsafe URLs (link policy is the
  shell's responsibility).

**Parse limits** (configurable, with safe defaults in `ParseLimits`):
- Source truncation at 64 MiB.
- Container nesting depth ≤ 32; deeper nesting is flattened.
- Inline nesting depth ≤ 32; deeper nesting is flattened.
- Total blocks ≤ 1 million; parsing stops at the limit.
- Per-table cells ≤ 200,000; table columns ≤ 256.
- Per code/HTML block text ≤ 4 MiB.

**CommonMark + GFM extensions:**
- Tables, task lists, strikethrough, and permissive autolinks are enabled.
- Raw HTML blocks and inlines are kept as plain text (not interpreted or rendered as HTML).

**Source ranges and reconstructability:**
- Ranges are **heuristically derived** and have documented limitations. They map source bytes
  (after BOM stripping) into semantic units; exact byte-for-byte correspondence is not guaranteed
  (e.g., after entity resolution or escape-sequence removal).
- Ranges are used for selection and editing, not for round-trip source regeneration.

## Consequences

- The parser is wrapped behind `IMarkdownParser`, so swapping implementations (e.g., a Rust parser,
  or a different C library) is possible without touching the rest of Rivet.
- Diagnostics are always present; the document is never rejected outright. Large documents
  degrade gracefully: the view shows what parsed, and the shell can report limits exceeded.
- HTML is not rendered (no browser engine). Links and images are subject to the shell's policy
  (link policy, image loader bounds).
- **No native syntax highlighting or spell checking yet.** Those can be layered on the model later
  without changing the parsing API.
- The model is thread-safe (immutable); parsing can run on a worker without worrying about races
  with rendering or editing.

## Rejected alternatives

- **Hand-rolled parser**: reinventing CommonMark parsing is error-prone; MD4C is battle-tested and
  widely used.
- **Rust/WebAssembly parser (Comrak, pulldown-cmark)**: adds a non-C++ dependency and build
  complexity; MD4C is C89 and links trivially.
- **HTML rendering (WebView, Chromium)**: violates ADR-0002 (no web engine). MD4C's choice to keep
  HTML as text aligns with Rivet's local, native-only architecture.
- **Native syntax highlighting via parser events**: hook into the parser and emit style spans; too
  coupled to the specific parser. A later phase can add highlighting as a post-pass over the model.
