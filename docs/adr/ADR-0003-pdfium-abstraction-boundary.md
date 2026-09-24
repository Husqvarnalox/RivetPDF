# ADR-0003: PDFium behind Rivet interfaces, built externally

Status: Accepted

Date: 2026-09-24

## Context

Rivet needs a PDF engine to parse, decode, and rasterize PDF documents.
Writing a full PDF interpreter from scratch is not a sensible first step; the
README records that "the underlying PDF engine handles the PDF format" while
Rivet owns editing behavior, state, undo/redo, rendering strategy, and
caching.

Candidate engines include PDFium, MuPDF, Poppler, and PDF.js. Requirements
shaped by Rivet's goals:

- High-fidelity rendering matching Chrome's PDF viewer behavior.
- Permissive license compatible with an open-source editor.
- C/C++ linkage (no JS runtime in-process).
- Ability to disable JavaScript and XFA (security posture).
- The engine must be replaceable and must not leak into any other subsystem:
  the README states Rivet "intentionally keeps the editor architecture
  separate from the underlying PDF engine".

Additionally, PDFium is a large Chromium-family codebase that must never be
fetched by Rivet's build: the project does not download dependencies, and
developers must be able to build the entire application without any PDFium
checkout at all.

## Decision

PDFium is the first PDF engine, integrated as follows:

1. **Behind Rivet interfaces.** `rivet_pdf` defines `PdfEngine`,
   `PdfDocument`, and `PdfTypes`. `PdfSystem.hpp` provides the
   `createEngine()` factory. No engine-specific types appear in these
   interfaces.
2. **Confined adapter.** `rivet_pdfium` (`src/pdf/pdfium/`) is the only code
   allowed to include PDFium headers or touch `FPDF_*` types. Nothing above
   `rivet_pdf` sees engine types; views never call PDFium.
3. **External build, imported target.** The developer builds PDFium locally
   (see `docs/BUILDING_PDFIUM.md`) and points Rivet at it via
   `RIVET_PDFIUM_ROOT` (CMake variable or environment variable).
   `cmake/FindPDFium.cmake` locates `<root>/include/fpdfview.h` and the
   library and creates the imported target `PDFium::PDFium`.
   **Rivet never downloads PDFium** - not at configure time, not at build
   time, not vendored into the repository.
4. **Null engine is a first-class configuration.** With
   `RIVET_WITH_PDFIUM=OFF` (the default), `rivet_pdf` compiles a null engine:
   `isAvailable()` returns `false` and every operation reports
   `NotAvailable`. The application shell still launches, and all non-PDF
   subsystems (core, render, UI, editor, tests) build without PDFium present.

## Consequences

- Rendering fidelity and PDF coverage come from a battle-tested engine
  (Chrome's PDF engine) without writing an interpreter.
- The editing model, document session, command stack, and tile cache are
  decoupled from the engine; a different backend could be added by
  implementing `rivet_pdf` interfaces without touching other subsystems.
- Every developer who wants PDF features must build PDFium once and set
  `RIVET_PDFIUM_ROOT`. This is documented in `docs/BUILDING_PDFIUM.md` and
  enforced with a clear configure-time error message.
- PDFium API churn (it tracks Chromium) is absorbed in one directory; the
  adapter is the only code that needs updating on engine upgrades.
- `RIVET_WITH_PDFIUM=OFF` keeps CI and contributor onboarding cheap, and
  guarantees the null path stays compilable and tested.

## Rejected alternatives

- **MuPDF**: AGPL - its license is incompatible with Rivet's open-source
  goals.
- **Poppler**: GPL-licensed components; also oriented around Cairo/GTK
  tooling rather than an embeddable raster engine.
- **PDF.js**: JavaScript, requiring a JS engine in-process; contradicts the
  native C++ model and the no-JavaScript security posture.
- **Vendoring PDFium source into the repository**: a massive tree that would
  dominate the repository, duplicate upstream CI, and slow every clone.
- **Auto-download at configure/build time** (like some dependency managers):
  Rivet's build never fetches anything; builds must be reproducible and
  offline.
- **Linking PDFium types throughout the app**: would make the engine
  irreplaceable and couple editor/UI/render code to `FPDF_*` types and
  PDFium threading rules.
