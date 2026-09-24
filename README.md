# Rivet

**A fast, native, open-source PDF editor for Windows, macOS, and Linux.**

Rivet is an open-source PDF editor focused on performance, simplicity, privacy, and full local control.

The long-term goal is simple:

> Build a real, free, native alternative to Adobe Acrobat.

No subscriptions.
No accounts.
No forced cloud storage.
No artificial feature limits.

Your documents stay on your computer.

---

## Status

> **Early development**

Rivet is currently in the initial development stage.

The architecture, rendering pipeline, document model, and editing engine are still being designed and implemented.

The project is not ready for daily use yet.

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

The long-term goal is to support actual PDF content editing, not only annotations.

Planned functionality includes:

* Edit existing text
* Add text
* Move text
* Delete text
* Change fonts
* Change font size
* Change text color
* Move images
* Resize images
* Replace images
* Delete images
* Move PDF objects

Editing existing PDF text is one of the most technically difficult parts of the project and will be developed gradually.

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

* [ ] Repository structure
* [ ] Cross-platform build system
* [ ] Application window
* [ ] Platform abstraction
* [ ] PDFium integration
* [ ] Open PDF
* [ ] Basic page rendering
* [ ] Zoom
* [ ] Scroll
* [ ] Render cache
* [ ] Tile rendering

### Phase 2 — Viewer

* [ ] Page thumbnails
* [ ] Multiple documents
* [ ] Text extraction
* [ ] Text selection
* [ ] Copy text
* [ ] Search
* [ ] Bookmarks
* [ ] Printing

### Phase 3 — Page Editing

* [ ] Page reorder
* [ ] Rotate
* [ ] Delete
* [ ] Duplicate
* [ ] Insert
* [ ] Extract
* [ ] Split
* [ ] Merge
* [ ] Crop

### Phase 4 — Annotations

* [ ] Highlight
* [ ] Underline
* [ ] Strikeout
* [ ] Notes
* [ ] Drawing
* [ ] Shapes
* [ ] Stamps

### Phase 5 — Content Editing

* [ ] Page object selection
* [ ] Text block reconstruction
* [ ] Text editing
* [ ] Font handling
* [ ] Text reflow
* [ ] Image editing
* [ ] Object movement
* [ ] Object deletion

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

The final project license has not yet been selected.

The intended license will allow Rivet to remain open-source while remaining compatible with the licenses of its dependencies.

A `LICENSE` file will be added before the first public release.

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

