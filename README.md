# Rivet

**A fast, native, open-source document editor for Windows, macOS, and Linux.**

Rivet is an open-source document editor for PDF and Markdown, focused on performance, simplicity, privacy, and full local control.

The long-term goal is simple:

> Build a real, free, native alternative to Adobe Acrobat.

No subscriptions.
No accounts.
No forced cloud storage.
No artificial feature limits.

Your documents stay on your computer.

---

## Status

> **Early development** - Phase 5 (Content Editing) and Phase 6 (Markdown) in progress.

Rivet is under active development. The macOS build currently provides:

**PDF**: tabs, thumbnails, outline, search, text selection, printing; page-level editing (multi-select, drag reorder, rotate, delete, duplicate, crop, import/merge, extract, split, undo/redo, background Save / Save As with atomic file replacement and unsaved-changes prompts); annotations (markup, notes, ink, shapes, stamps); content editing (select, move, resize, delete page objects; retype existing text in place; add text; replace images).

**Markdown**: read-only viewing with source text editing. Rendered view (read-only; no WYSIWYG editing); Source view for text editing with live preview; Split view showing both. Edit text, undo/redo with dirty tracking and background save.

**Not yet implemented:**
- Markdown WYSIWYG / visual editing in Rendered mode.
- Markdown Export to PDF and Print.
- Markdown features: Mermaid diagrams, math rendering (LaTeX/MathML).
- PDF forms; PDF Extract/Split document-level structure (outline, metadata, labels).
- Windows and Linux shells (portable layers build and are tested on both).

Known PDF limitations (details in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#phase-3-status-closed-2026-10-02) and [Phase 5 status](docs/ARCHITECTURE.md#phase-5-status-closed-2026-10-03)): editing is paused while a save runs; reviewing several unsaved tabs on window close saves only one per pass; split never overwrites existing files (atomically on macOS, best effort elsewhere); pages whose content PDFium cannot regenerate faithfully are read-only for content tools; existing objects cannot be reordered in z; bundled fallback fonts cover Latin, Greek and Cyrillic only.

The project is not ready for daily use yet. macOS is the primary platform; Windows and Linux shells are not yet available.

---

## Goals

Rivet aims to become a full-featured PDF editor while remaining lightweight and native.

The main priorities are:

* Fast startup
* Low memory usage
* Native desktop performance
* Smooth rendering and scrolling
* Large document support
* Full offline functionality
* Cross-platform support
* Privacy by default
* No telemetry by default
* No account required
* No subscription
* Open-source development

Rivet should feel like a normal desktop application, not a website wrapped inside a desktop shell.

---

## Platforms

Planned desktop support:

* Windows
* macOS
* Linux

Possible future platforms:

* iOS
* Android

Desktop is the current priority.

---

## Planned Features

### Viewing

* Open and view PDF documents
* Multiple document tabs
* Smooth zooming and scrolling
* Page thumbnails
* Bookmarks
* Text selection
* Text search
* Copy text
* Fullscreen mode
* Presentation mode
* Printing

### Page Management

* Reorder pages
* Rotate pages
* Delete pages
* Duplicate pages
* Insert pages
* Extract pages
* Split PDFs
* Merge PDFs
* Crop pages

### Content Editing

Rivet edits actual PDF content, not only annotations:

* Edit existing text in place (the block keeps its own font when that font can write the new text; otherwise a bundled metric-compatible font is embedded)
* Add text (bundled Sans / Serif / Mono, Regular / Bold)
* Move and delete text blocks
* Change the font of text added by Rivet, the font size and the text color of any editable block
* Deterministic reflow inside the block's width; longer text extends the block downward, never clipped silently
* Move, resize, replace (PNG / JPEG) and delete images
* Move, resize and delete vector paths

What is deliberately not done: editing text set in Type3 fonts, text with unmappable glyphs, text under a clip, or anything on a page that PDFium cannot regenerate faithfully (shading, inline images, patterns, hidden optional content, ...). Such objects are selectable and the status bar explains why they are read-only. Annotations, form widgets and links are never treated as page content, and nested Form XObjects are read-only.

#### Using the content tools (macOS)

* **Edit** (toolbar, Content menu, Cmd+Shift+E) selects page objects. Click selects the topmost object (a text block as a unit); the status bar says what it allows (read-only with the reason, move/delete only, or retype with a bundled substitute font). Drag moves it with a live preview; arrow keys nudge by 1 pt (Shift: 10 pt, a quick burst of one key is a single undo step); Delete or Backspace removes it; Esc clears the selection.
* Images and paths show eight handles. A corner handle keeps the aspect ratio of an image (hold Shift to resize freely; for paths it is the other way round). Text is never scaled: the handle on the right side of a text block sets its wrap width.
* **Double-click** (or Return) on an editable text block opens the inline editor over the block. Esc cancels; clicking outside or Cmd+Return commits as one undo step. Text a bundled font cannot write is refused with the offending code points. Save, close and quit commit an open edit first. Rotated blocks are edited unrotated and keep their rotation.
* **Add Text** (toolbar, Content menu, Cmd+Shift+T): click the page to type a new block, or drag to draw a box (its width becomes the wrap width). Font family (Sans, Serif, Mono), Bold, size and color are in the properties bar; an empty commit creates nothing.
* The properties bar also restyles a selected text block, shows an image's pixel size, offers **Replace Image...** (PNG or JPEG, decoded in the background within size limits) and Bring to Front for text added by Rivet.
* Limitation: Undo/Redo menu items do not yet show the name of the content command.

### Annotations

* Highlight
* Underline
* Strikeout
* Freehand drawing
* Shapes
* Arrows
* Text comments
* Sticky notes
* Stamps

### Forms

* Fill PDF forms
* Text fields
* Checkboxes
* Radio buttons
* Dropdown fields
* Signature fields

### Signatures

Planned:

* Drawn signatures
* Image signatures
* Certificate-based digital signatures
* PDF signature validation

### Document Tools

Planned:

* OCR
* PDF compression
* PDF optimization
* Redaction
* Watermarks
* Headers and footers
* Metadata editing
* Password protection
* Encryption
* Document comparison

---

## Architecture

Rivet is designed as a native C++ desktop application.

The current planned architecture is:

```text
Rivet
│
├── Application
│   ├── Commands
│   ├── Undo / Redo
│   ├── Document Controller
│   ├── Selection
│   └── Tool System
│
├── Editor Core
│   ├── Document Model
│   ├── Text Model
│   ├── Page Objects
│   ├── Images
│   ├── Annotations
│   └── Forms
│
├── Rendering
│   ├── Page Renderer
│   ├── Tile Renderer
│   ├── Render Cache
│   └── Viewport
│
├── PDF Engine
│   └── PDFium
│
└── Platform
    ├── Windows
    ├── macOS
    └── Linux
```

The project intentionally keeps the editor architecture separate from the underlying PDF engine.

This allows Rivet to maintain its own document model, editor logic, caching, UI, and tooling without tightly coupling the application to a specific PDF library.

---

## Technology

Current planned stack:

| Component        | Technology                                        |
| ---------------- | ------------------------------------------------- |
| Language         | C++23                                             |
| Build system     | CMake                                             |
| PDF engine       | PDFium                                            |
| Windows graphics | Direct2D / DirectWrite                            |
| macOS graphics   | CoreGraphics / CoreText / Metal where appropriate |
| Linux graphics   | Native Linux stack                                |
| Testing          | C++ testing framework                             |

The goal is to minimize unnecessary dependencies.

Large application frameworks and browser-based desktop runtimes are intentionally avoided.

Rivet will not use Electron.

---

## Performance

Performance is one of the core design goals.

Rivet should eventually support:

* Fast application startup
* Smooth 60 FPS scrolling where possible
* Large PDF files
* Documents containing hundreds or thousands of pages
* Lazy page loading
* Tile-based rendering
* Bounded memory usage
* Background rendering
* Render cache eviction
* Incremental document loading where possible

Pages should be rendered only when required instead of loading an entire document into memory.

Example:

```text
Document

Page 14
Page 15
Page 16  <- visible
Page 17  <- visible
Page 18
Page 19

Only the required pages and render tiles remain in memory.
```

---

## Privacy

Rivet is designed as a local-first application.

PDF documents should not need to leave the user's computer.

The project does not require:

* User accounts
* Cloud storage
* Online document processing
* Uploading PDFs to third-party servers

Features that require network access, if introduced in the future, should be optional and clearly separated from the core editor.

---

## Why Rivet?

There are many PDF viewers.

There are many PDF annotation tools.

There are very few high-quality, native, fully featured, free PDF editors.

Many existing applications require:

* Expensive subscriptions
* Accounts
* Cloud processing
* Proprietary formats
* Artificial feature restrictions
* Large application runtimes

Rivet exists to provide another option.

A PDF editor should simply let you open a document, edit it, save it, and continue working.

---

## Development Philosophy

Rivet follows several principles.

### Native first

The desktop application should use native system capabilities instead of embedding an entire browser runtime.

### Local first

Documents remain local unless the user explicitly chooses otherwise.

### Performance matters

Memory usage, rendering latency, input latency, and startup performance are considered product features.

### Dependencies must justify themselves

Rivet should not reimplement complex standards purely for the sake of avoiding dependencies.

At the same time, unnecessary frameworks and libraries should not become part of the core application.

### Editor logic belongs to Rivet

The underlying PDF engine handles the PDF format.

Rivet handles:

* editing behavior
* document state
* undo and redo
* selection
* tools
* rendering strategy
* caching
* user experience

---

## Repository Structure

The repository will gradually move toward a structure similar to:

```text
rivet/
├── src/
│   ├── app/
│   ├── core/
│   ├── editor/
│   ├── pdf/
│   ├── render/
│   ├── ui/
│   └── platform/
│
├── tests/
│
├── third_party/
│
├── cmake/
│
├── assets/
│
├── CMakeLists.txt
├── LICENSE
└── README.md
```

The structure may change significantly while the project is still young.

---

## Building

Build instructions will be added once the initial project structure and dependency management are stable.

The expected toolchain will include:

```text
C++23 compiler
CMake
Ninja or platform-native build tools
PDFium
```

Supported compilers are expected to include:

* MSVC
* Clang
* GCC

---

## Roadmap

### Phase 1 — Foundation

* [x] Repository structure
* [x] Cross-platform build system
* [x] Application window
* [x] Platform abstraction
* [x] PDFium integration
* [x] Open PDF
* [x] Basic page rendering
* [x] Zoom
* [x] Scroll
* [x] Render cache
* [x] Tile rendering

### Phase 2 — Viewer

* [x] Page thumbnails
* [x] Multiple documents
* [x] Text extraction
* [x] Text selection
* [x] Copy text
* [x] Search
* [x] Bookmarks
* [x] Printing

### Phase 3 — Page Editing (closed)

* [x] Page reorder
* [x] Rotate
* [x] Delete
* [x] Duplicate
* [x] Insert
* [x] Extract
* [x] Split
* [x] Merge
* [x] Crop

### Phase 4 — Annotations

* [x] Highlight
* [x] Underline
* [x] Strikeout
* [x] Notes
* [x] Drawing
* [x] Shapes
* [x] Stamps

### Phase 5 — Content Editing (closed)

* [x] Page object selection
* [x] Text block reconstruction
* [x] Text editing
* [x] Font handling
* [x] Text reflow
* [x] Image editing
* [x] Object movement
* [x] Object deletion

### Phase 6 — Advanced Features

* [ ] Forms
* [ ] Signatures
* [ ] OCR
* [ ] Redaction
* [ ] Compression
* [ ] Optimization
* [ ] Encryption
* [ ] Document comparison

---

## Contributing

Contributions will be welcome.

The project is currently at a very early stage, so large architectural contributions should be discussed before implementation.

More detailed contribution guidelines will be added as the codebase stabilizes.

Future contribution areas may include:

* C++ development
* PDF internals
* Rendering
* Windows development
* macOS development
* Linux development
* Performance optimization
* UI/UX
* Accessibility
* Testing
* Documentation
* Localization

---

## License

Rivet is licensed under the Mozilla Public License 2.0.
See LICENSE for details.

---

## Project Name

**Rivet**

The name represents the idea of a small, strong tool that holds documents together without getting in the user's way.

---

## Disclaimer

Rivet is an independent open-source project.

It is not affiliated with Adobe, Adobe Acrobat, or any other commercial PDF software vendor.

PDF is an open document format standardized by ISO.

---

## Support the Project

Rivet is currently developed as an open-source project.

If the project becomes useful to you, the best ways to help are:

* Star the repository
* Report bugs
* Test unusual PDF files
* Suggest improvements
* Contribute code
* Improve documentation

---

**Rivet — edit PDFs, not subscriptions.**

