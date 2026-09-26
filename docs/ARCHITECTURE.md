# Rivet Architecture

This document is the reference for Rivet's software architecture. It describes
the system as designed; where code is still mid-implementation, the structure
described here is the source of truth.

Rivet is a native C++23 PDF viewer/editor. It is built from small, layered subsystems
with a strict dependency direction, a tile-based rendering pipeline, a
serialized-per-document threading model, and a `std::expected`-based error
model. Platform integration (AppKit on macOS) is confined to a dedicated
platform layer; the PDF engine (PDFium) is confined to a dedicated adapter.

Key decisions are recorded in `docs/adr/`:

| ADR | Decision |
| --- | --- |
| [ADR-0001](adr/ADR-0001-cpp23.md) | C++23 language baseline |
| [ADR-0002](adr/ADR-0002-native-platform-architecture.md) | Native per-platform backends, no GUI framework |
| [ADR-0003](adr/ADR-0003-pdfium-abstraction-boundary.md) | PDFium behind Rivet interfaces |
| [ADR-0004](adr/ADR-0004-retained-mode-rivet-ui.md) | Rivet-owned retained-mode widget system |
| [ADR-0005](adr/ADR-0005-tile-based-rendering.md) | Tile-based rendering and cache policy |
| [ADR-0006](adr/ADR-0006-serialized-pdf-access-per-document.md) | Serialized PDF access per document |
| [ADR-0007](adr/ADR-0007-pdf-javascript-xfa-disabled.md) | PDF JavaScript/XFA disabled |

---

## 1. Overview

Rivet is organized as a set of statically linked CMake libraries plus one
executable:

- `rivet_core` - geometry, strong IDs, errors, logging, time, bitmaps, task scheduling.
- `rivet_render` - coordinate transforms, page layout, zoom, viewer state, tile cache, render contracts.
- `rivet_pdf` - PDF engine interfaces (`PdfEngine`, `PdfDocument`, `PdfTypes`) and a null engine when PDFium is not compiled in.
- `rivet_pdfium` - the PDFium adapter; only built with `RIVET_WITH_PDFIUM=ON`.
- `rivet_editor` - commands/undo, document sessions, the render source that drives rasterization.
- `rivet_ui` - Rivet-owned retained-mode widgets.
- `rivet_platform` - thin platform abstraction headers (file dialog, main-thread dispatch).
- `rivet_platform_macos` - AppKit/CoreGraphics/CoreText implementation of the platform abstractions; hosts the application.
- `rivet_app` - shell wiring: toolbar, sidebar, status bar, viewport, `DocumentSession` management.
- `rivet` - the executable.

Dependency direction (an arrow `A -> B` means A may depend on B):

```text
                    rivet_app
                   /    |     \
                  v     |      v
          rivet_ui      |   rivet_editor
                \       |    /    |    \
                 \      v   v     |     v
                  +--> rivet_render <--+   rivet_pdf
                          |                |
                          v                v
                     rivet_core        rivet_pdfium (RIVET_WITH_PDFIUM=ON)
                                          |
                                          v
                                      rivet_core
```

`rivet_platform` and `rivet_platform_macos` sit outside this core chain:

```text
    rivet_app -----> rivet_platform          (abstraction headers only)
        ^                                          ^
        |                                          |
    rivet (executable)                       rivet_platform_macos
         |      \                                     |
         v       v                        depends on: rivet_ui, rivet_core, rivet_platform
   rivet_app    rivet_platform_macos

The `rivet` executable is the composition root: it may depend on both the
application layer and the platform backend; the platform backend itself never
depends on the application layer.
```

Hard rules:

- `rivet_core` depends on nothing inside Rivet (only the C++ standard library / `Threads`).
- `rivet_render` depends only on `rivet_core`.
- `rivet_editor` and `rivet_ui` depend on `rivet_core` + `rivet_render`; neither depends on the other.
- `rivet_app` depends on `rivet_ui`, `rivet_editor`, `rivet_pdf`, and the `rivet_platform` abstractions.
- `rivet_platform_macos` implements the `rivet_platform` abstractions and provides the executable entry point; it is the only layer allowed to use AppKit/CoreGraphics/CoreText.
- Shared UI code (`rivet_ui`, `rivet_app`) never sees AppKit or CoreGraphics types.
- `FPDF_*` types never leave `src/pdf/pdfium/`; nothing above `rivet_pdf` sees engine types.
- Views never call PDFium; `IRenderSource` is the only render entry for views.

---

## 2. Subsystem responsibilities

### `rivet_core` (`src/core/`)

Foundation types shared by everything else.

- **Geometry** (`core/geometry/`): `Point`, `Size`, `Rect`, `Insets`, `Matrix`, and `PageRotation` (0/90/180/270 clockwise, matching the PDF `/Rotate` integer encoding). Types are convention-neutral; see section 4 for coordinate spaces.
- **`StrongId<Tag>`** (`core/StrongId.hpp`): typed identifiers (`DocumentId`, `PageId`, `ObjectId`). Distinct tags produce distinct, non-interchangeable types; value 0 is reserved as the invalid ID. `IdGenerator<Tag>` mints sequential IDs.
- **Errors** (`core/Error.hpp`): `Result<T>` = `std::expected<T, Error>`, `Error{code, message, subsystem}`, shared `ErrorCode` enum. See section 8.
- **`Bitmap`** (`core/Bitmap.hpp`): Rivet-owned raster surface, `BGRA8888Straight` (matching PDFium's `FPDFBitmap_BGRA`), move-only, explicit stride. Allocation arithmetic is overflow-checked and bounded (`kMaxBitmapDimension` = 1M px per axis, `kMaxBitmapBytes` = 512 MiB) because document-driven dimensions are untrusted input.
- **`log`** (`core/Log.hpp`): minimal built-in, thread-safe stderr logging with levels. Never logs document contents or user text.
- **`Time`** (`core/Time.hpp`): wall-clock milliseconds and a monotonic `Stopwatch`.
- **Async** (`core/async/`): `TaskScheduler` (shared worker pool over `std::jthread`), `SerialExecutor` (FIFO serialization over the shared pool), `IMainThreadDispatcher` (marshal-to-main-thread abstraction). See section 5.

### `rivet_render` (`src/render/`)

Everything needed to turn a document into pixels, independent of any UI toolkit and of any PDF engine.

- `PageTransform` - the only place coordinate conversions live (section 4).
- `PageLayout` - stacks pages vertically in content space; precomputed page frames, `visiblePageRange`, `pageIndexAt`, `currentPageIndex` (viewport-center rule, tie -> lower index).
- `ZoomState` - clamped user zoom (0.10 .. 64.0), preset stops, fit-width/fit-page modes.
- `ViewerState` - per-view state (zoom + scroll offset) owned by the application layer per tab and bound by PdfViewport; the viewport is the mutator, the state the single source of truth.
- `RenderScaleKey` - zoom quantized UP to multiples of 1/64 (section 6).
- `TileKey` - `(DocumentId, PageId, RenderScaleKey, tileX, tileY)` cache identity.
- `RenderRequest` / `RasterParams` - cache identity plus pure raster parameters.
- `RenderPriority` - `Visible` / `Impending` / `Prefetch` scheduling hints.
- `TileCache` - byte-bounded, mutex-guarded LRU of rendered tiles (section 7).
- `IRenderSource` - the abstract render service consumed by viewports (section 6).

### `rivet_pdf` (`src/pdf/`)

Engine-independent interfaces: `PdfEngine` (backend availability, `openDocument`), `PdfDocument` (owned document handle; `pageInfo`, `renderPage`), `PdfTypes` (`PdfDocumentInfo`, `PdfPageInfo`), and the `createEngine()` factory (`PdfSystem.hpp`). When `RIVET_WITH_PDFIUM=OFF`, `createEngine()` returns a null backend: `isAvailable()` is `false` and every operation reports `NotAvailable`. The application shell still launches in this configuration.

### `rivet_pdfium` (`src/pdf/pdfium/`)

The PDFium adapter, built only when `RIVET_WITH_PDFIUM=ON`. Implements the `rivet_pdf` interfaces on top of PDFium and links the imported target `PDFium::PDFium` created by `cmake/FindPDFium.cmake`. All `FPDF_*` usage is confined to this directory. Beyond rendering it provides text extraction (`PdfTextPage` in displayed-page coordinates), document outline (depth/node/cycle-bounded), page labels, and per-page links (internal destinations and scheme-validated external URLs). See [ADR-0003](adr/ADR-0003-pdfium-abstraction-boundary.md) and `docs/BUILDING_PDFIUM.md`.

### `rivet_editor` (`src/editor/`)

- **`Command` / `CommandStack`** - undo/redo with `execute` / `undo` / `redo` / `canUndo` / `canRedo` / `clear`, depth-bounded.
- **`DocumentSession`** - owns one open document: its `DocumentId`, the PDF document handle, the `PageLayout`, a revision counter, the `CommandStack`, and a `SerialExecutor`-driven `DocumentRenderer`.
- **`DocumentRenderer`** - implements `IRenderSource`: dedupes requests, serializes PDF access through the session's `SerialExecutor`, stores results in the `TileCache`, and returns bitmaps via main-thread callbacks.

### `rivet_ui` (`src/ui/`)

A small, Rivet-owned retained-mode widget system: `Widget`, `Container`, `Button`, `Toolbar`, `ScrollBar`, `TextField` (UTF-8 caret/selection, echo masking), `TabStrip`, `PageThumbnailList` (lazy, virtualized thumbnails over the shared TileCache), `OutlinePanel`, `PdfViewport`. Painting goes through a `PaintContext` abstraction that hides CoreGraphics. Widgets and event handling are main-thread-only. `IViewerTextBridge` (implemented by the app layer) connects the viewport to text features without an editor dependency. See [ADR-0004](adr/ADR-0004-retained-mode-rivet-ui.md).

### `rivet_platform` / `rivet_platform_macos` (`src/platform/`)

`rivet_platform` declares thin abstractions - file dialog, main-thread dispatch, clipboard, external URL opener, print service. `rivet_platform_macos` implements them with AppKit (`NSWindow`/`NSView` host, `NSOpenPanel`, `NSPasteboard`, `NSWorkspace` URL opening, `NSPrintOperation` printing), provides the CoreGraphics `PaintContext` implementation and CoreText text services, and defines the `rivet` executable entry point. It is the only module that touches AppKit/CoreGraphics/CoreText directly.

### `rivet_app` (`src/app/`)

Application shell wiring: `DocumentWorkspace`/`DocumentTab` (multi-tab workspace, asynchronous open with Loading/Ready/Error/NeedsPassword states, per-tab `ViewerState` + selection + search) and the `ShellController` composition root. Composes `rivet_ui` widgets with `rivet_editor` services.

`ShellController` owns the scheduler, engine, workspace and widget tree; it builds the tab strip, toolbar, viewport and loading overlay, runs the layout, owns keyboard focus and key routing (focused widget → Escape priorities → shortcuts → viewport), and rebinds everything when the active tab changes. It also handles tabs, window title, presentation mode, open and print. Feature logic lives in focused controllers that share one `ShellContext` (workspace, platform services, viewport, and status/focus/relayout sinks back into the shell):

- `TextInteractionController` — the viewport's `IViewerTextBridge` (text hit testing, selection, selection/search highlight rects, link hit testing), asynchronous copy via `TextService::requestRangesText`, internal link navigation and the external-URL policy.
- `SearchBarController` — the find bar; drives the active tab's `TextSearchController`. Result notifications from background tabs are ignored; switching tabs closes the bar.
- `SidebarController` — Pages (thumbnails) / Outline modes. The outline loads asynchronously via `LinkService::requestOutline` and is rebuilt only if the same session is still active. Expansion state is per document.
- `PasswordPromptController` — the masked prompt for NeedsPassword tabs; the field is cleared before `retryWithPassword`.
- `StatusBarController` — the status message and the "Page [field] / N" indicator with strict page-number parsing.

Controllers are main-thread only and every feature no-ops when no Ready tab is active. The shell constructs them after the members they reference and destroys them first (reverse declaration order). `DocumentWorkspace::closeTab` keeps the closed tab alive until its host hooks have run, so bound views can unbind from a live session.

---

## 3. Build and test layout

CMake >= 3.28 with the Ninja generator. Presets: `debug`, `release`,
`relwithdebinfo`, `sanitizers`, `debug-pdfium`. Options: `RIVET_WITH_PDFIUM`
(default `OFF`), `RIVET_BUILD_TESTS` (default `ON`),
`RIVET_ENABLE_SANITIZERS` (default `OFF`). See `docs/BUILDING.md` and
`docs/BUILDING_PDFIUM.md`.

Tests use CTest and a tiny internal harness (`tests/harness/RivetTest.h`); no
third-party test framework. One test executable per module: `rivet_core_tests`,
`rivet_async_tests`, `rivet_render_tests`, `rivet_pdf_tests`,
`rivet_editor_tests`, `rivet_ui_tests`.

---

## 4. Coordinate systems

Rivet distinguishes three user-facing coordinate spaces plus one internal
intermediate. `PageTransform` is the ONLY place conversions between them live.

```text
   PDF page space                page display space
   (points, origin               (points, origin at the
    bottom-left, y-up,            TOP-LEFT of the displayed
    unrotated page)               page, y-down)
          |                              ^
          |        PageRotation          |
          +------- applied inside ------>+
                  PageTransform

   PDF page space  <--pageToLogical/logicalToPage-->  logical viewport space
                                                        (points, origin top-left
                                                         of the viewport, y-down;
                                                         = page display points
                                                           * zoom, offset by the
                                                           page frame)

   logical viewport space  <--logicalToPhysical/physicalToLogical (x backing scale)-->
                                                        physical pixel space
                                                        (device pixels, origin
                                                         top-left, y-down)
```

Precisely:

1. **PDF page space** - PDF user space: points, origin bottom-left, y-up, as encoded in the file (before `/Rotate`).
2. **Page display space** (internal to `PageTransform` and raster params) - points, origin at the top-left of the *displayed* page, y-down, after applying `PageRotation` (clockwise 0/90/180/270, matching the PDF `/Rotate` encoding). For unrotated page size `(w0, h0)` and rotated display box `(W, H)`, an unrotated page point `(x, y)` maps to display deltas `(dx, dy)`:
   - `None`: `W = w0, H = h0; dx = x, dy = h0 - y`
   - `Clockwise90`: `W = h0, H = w0; dx = y, dy = x`
   - `Clockwise180`: `W = w0, H = h0; dx = w0 - x, dy = y`
   - `Clockwise270`: `W = h0, H = w0; dx = h0 - y, dy = w0 - x`
3. **Logical viewport space** - points, origin top-left of the viewport, y-down:
   `logical = pageFrameLogical.origin + zoom * (dx, dy)`
4. **Physical pixel space** - device pixels: `physical = logical * backingScale` (the display's backing scale). The effective raster density is `devicePixelsPerPoint = zoom * backingScale`.

`pageToLogical` / `logicalToPage` and `logicalToPhysical` / `physicalToLogical` are exact inverses; a 90-degree rotation maps axis-aligned rects to axis-aligned rects, so rect conversion is exact. `PageTransform::make` requires `zoom > 0` and `backingScale > 0`.

`PageLayout` works in **content space** (points, top-left origin, y-down): pages stacked vertically, each frame horizontally centered within the widest page, surrounded by a margin (default 24 pt) with a gap between pages (default 16 pt). A page's frame in content space becomes its `pageFrameLogical` once the viewport scroll offset is applied.

`PdfDocument::renderPage` and `RasterParams` take the sub-rectangle in **page display space** (item 2) plus `devicePixelsPerPoint`; the engine never needs to know about viewport offsets or backing scales separately.

---

## 5. Threading model

```text
                       main thread (UI)
   widget/event handling, paint, DocumentSession ownership
                          ^            |
        IMainThreadDispatcher.post     | render callbacks
                          |            v
   +----------------------+------------+------------------+
   |                 TaskScheduler (shared)                |
   |   std::jthread pool, FIFO queue, auto-sized           |
   |   clamp(hardware_concurrency - 1, 2, 8) workers       |
   |                                                       |
   |   SerialExecutor (doc A)  SerialExecutor (doc B) ...  |
   |   one-at-a-time FIFO over the shared pool;            |
   |   NO dedicated OS thread per document                 |
   +-------------------------------------------------------+
```

- **Shared `TaskScheduler`**: fixed-size `std::jthread` pool, FIFO queue, used for background rasterization. Shutdown is intentionally fast: pending tasks are discarded, in-flight tasks run to completion before join.
- **`SerialExecutor` per open document**: exactly one task of a given executor runs at any moment, in FIFO post order, executed on the shared `TaskScheduler`. Idle executors cost nothing. This serializes all access to one PDFium document handle without dedicating an OS thread per document.
- **Process-wide PDFium call gate**: PDFium's entire public API is not thread-safe (it also holds process-global state such as font caches and `FPDF_GetLastError`), so no two `FPDF_*` calls may run concurrently even for *different* documents. The PDFium adapter serializes every call through an internal `PdfiumCallGate` (a mutex that never leaves `rivet_pdfium`); the per-document executors remain in charge of FIFO ordering, coalescing, cancellation and lifecycle. See [ADR-0006](adr/ADR-0006-serialized-pdf-access-per-document.md) and its correction section.
- **Print spooling**: `editor::PrintSpooler` renders print bands on its own `SerialExecutor` (over the shared pool) and delivers progress/completion through `IMainThreadDispatcher`; the main thread only shows panels and composites pre-rendered band files (section 6, Printing).
- **Main-thread marshaling**: render callbacks are delivered via `IMainThreadDispatcher`, implemented by the platform layer (dispatch to the macOS main queue in production). All widget and event handling is main-thread-only.
- **`TileCache`** is internally mutex-guarded: entries may be inserted, looked up and evicted from scheduler threads and the main thread concurrently.
- `ZoomState` and widget state are main-thread-only and not internally synchronized.

---

## 6. Rendering pipeline

Rendering is tile-oriented from day one ([ADR-0005](adr/ADR-0005-tile-based-rendering.md)).

- **Tile size**: 512 x 512 device pixels.
- **`RenderScaleKey`**: zoom quantized UP to multiples of 1/64 (`ceil(zoom * 64) / 64`, clamped to `[0.10, 64.0]`). Rounding up guarantees the raster is never produced at a lower resolution than requested; the painter scales down by less than 1/64. Zoom levels that quantize to the same key share tiles.
- **`PhysicalRenderScaleKey`**: device pixels per point = quantized zoom x display backing scale, quantized UP to multiples of 1/64 and clamped to `[0.1, 512]`. Two render requests that would produce different pixel dimensions (e.g. 100% zoom on a 1x vs a 2x display) never share a cache entry: `RasterParams::devicePixelsPerPoint` is always derived from this key, and `DocumentRenderer` rejects requests whose params disagree with the key.
- **`TileKey`** = `(DocumentId, PageId, PhysicalRenderScaleKey, tileX, tileY)` - cache identity per tile cell of the page grid.
- **`RenderRequest`** = `TileKey` + `RasterParams{ pageRectPoints (page display space), devicePixelsPerPoint }`.
- **`RenderPriority`**: `Visible` (on screen now), `Impending` (about to become visible via scroll lookahead), `Prefetch`.

Pipeline from a viewport request to a painted tile:

```text
 PdfViewport (main thread)
     |  requestRender(RenderRequest, RenderPriority, onDone)
     v
 IRenderSource  <-- implemented by DocumentRenderer (rivet_editor)
     |  1. dedupe: identical (TileKey, revision) requests are coalesced
     |  2. cache probe: TileCache hit -> callback immediately
     v
 SerialExecutor (per document)  -->  TaskScheduler (shared pool)
     |
     v
 PdfDocument::renderPage(pageIndex, pageRectPoints, devicePixelsPerPoint)
     |  (worker thread; PDFium raster confined to rivet_pdfium)
     v  core::Bitmap
 TileCache::put(TileKey, revision, bitmap)      [mutex-guarded LRU]
     |
     v
 IMainThreadDispatcher.post(...)                [back to main thread]
     |
     v
 onDone(Result<Bitmap>) -> viewport repaints that tile
```

The synchronous paint path is `IRenderSource::cachedTile(TileKey, revision)`: a non-blocking cache probe used while painting; on a miss the viewport schedules an async `requestRender` and paints what it has (gray placeholder until the tile arrives). Paint enumerates only the tiles overlapping the visible region (direct index math, clamped to the page grid - never a full-page grid sweep). Permanently failed tiles are recorded by `DocumentRenderer` and replay their error without re-scheduling, so a broken tile cannot create a repaint/render loop; a revision change or an explicit `retryFailedTiles()` clears the record. On a zoom change the viewport drops queued-not-started requests so renders for the previous scale are not drained pointlessly.

Rules:

- `IRenderSource` is the only render entry for views. Views never call PDFium and never touch `PdfDocument`.
- `DocumentRenderer` (editor layer) owns the `TileCache`, dedupes requests, and serializes PDF access via the session's `SerialExecutor`.
- Results are delivered exactly once per accepted request, on the main thread when a dispatcher is configured (tests may run callbacks inline on the worker thread).
- `cancelAll()` drops queued requests; results report `Cancelled` or do not fire.
- Pages load lazily: only tiles for visible/impending pages are scheduled, so documents with thousands of pages never render fully into memory.

### Printing

Printing never rasterizes on the main thread and never uses an alternate PDF engine:

```text
 PrintCoordinator (app, main)   platform::IPrintService::choosePrintSettings
     |                           (native panel, modal, no rendering, no preview)
     v
 editor::PrintSpooler           own SerialExecutor over the shared TaskScheduler
     |  plan densities + bands   (pure arithmetic, main thread)
     |  worker: per band -> PdfDocument::renderPage(page, bandRect, dpp)
     |          -> raw BGRA file in an owner-only temp spool directory
     v  onProgress / onComplete(Result<PrintSpool>)   [via IMainThreadDispatcher]
 platform::IPrintService::printSpool   (main) composites file-backed band images
```

- **Density policy** (`PrintSpoolOptions`): 150 dpi target; each page is reduced so its raster fits 48 Mi-pixels (A0 still gets the full 150 dpi), with a 0.5 px/pt floor so huge posters still print; a whole-job 1 GiB spool budget scales density down globally before rendering starts and fails with a clear error when even the floor does not fit.
- **Memory bound**: bands are full-width horizontal strips of at most 4 Mi-pixels; only one band exists in memory at a time. A page so wide that a 1-pixel strip exceeds the band budget gets a lower density (the band bound beats the floor).
- **Failure model**: the first render or I/O error aborts the job, removes the spool and is reported - never a partial or blank print. A band that cannot be read while printing cancels the AppKit job (`printInfo.jobDisposition = NSPrintCancelJob`, which makes `-runOperation` return NO with no output) and is reported.
- **Cancellation/lifetime**: Esc cancels an active spool (checked between bands). Closing the tab or tearing down the shell cancels and waits for the worker (at most one band render), because the spool borrows the session's `PdfDocument`. Completions queued after the spooler died are no-ops.
- Oversized pages are scaled down uniformly to fit the sheet's printable area (never up); each sheet keeps its page's display size/orientation.

---

## 7. Cache policy

`TileCache` is an LRU bounded by an exact byte budget (default 256 MiB, a hard bound):

- Byte accounting is exact: the total is the sum of `Bitmap::sizeBytes()` and never exceeds `maxBytes()`. `put()` refuses a bitmap whose size alone would not fit (and refuses null/invalid bitmaps).
- Re-putting an existing key replaces the entry and promotes it to most-recently-used. `get()` hits are promoted; misses and stale entries return `nullptr`.
- **Revision invalidation**: entries are stamped with the document revision they were rendered for. A `get()` with a different revision is a miss and lazily drops the stale entry, so a document edit invalidates its tiles without a full cache sweep.
- Shrinking the budget evicts least-recently-used entries immediately.
- Keys include `DocumentId`, so multiple open documents (planned: tabs) can share one cache without collision.
- Thread-safe via an internal mutex (section 5).

---

## 8. Error model

`rivet::core::Result<T>` = `std::expected<T, rivet::core::Error>` ([ADR-0001](adr/ADR-0001-cpp23.md)).

- `Error { ErrorCode code; std::string message; std::string subsystem; }`
- One shared `ErrorCode` enum (`InvalidArgument`, `NotFound`, `NotAvailable`, `Unsupported`, `Io`, `InvalidDocument`, `OutOfMemory`, `Cancelled`, `Internal`, ...); the `subsystem` string identifies the origin.
- `Status` = `Result<void>`; `ok()` produces a successful status.
- Fallible operations at subsystem boundaries return `Result` rather than throwing. Exceptions do not cross subsystem boundaries; `Bitmap::create` is the pattern - impossible dimensions or allocation failure return an error instead of throwing.

---

## 9. Security and privacy posture

PDFs are untrusted input.

- **Allocation arithmetic is overflow-checked and bounded**: `kMaxBitmapDimension` = 1M pixels per axis, `kMaxBitmapBytes` = 512 MiB; impossible page dimensions are rejected.
- **No document JavaScript execution**; PDFium is built with V8 and XFA disabled ([ADR-0007](adr/ADR-0007-pdf-javascript-xfa-disabled.md)).
- **No automatic link launching**; no embedded content execution.
- **No telemetry**; no logging of document contents or metadata (`core::log` documents this as a hard rule).
- Documents stay local: no accounts, no cloud processing, no uploads.

---

## 10. Directory layout

```text
rivet/
├── CMakeLists.txt            # options, FindPDFium wiring, incremental subsystem registration
├── CMakePresets.json         # debug / release / relwithdebinfo / sanitizers / debug-pdfium
├── cmake/
│   ├── FindPDFium.cmake      # locates a local PDFium, creates imported PDFium::PDFium
│   ├── RivetWarnings.cmake
│   └── RivetSanitizers.cmake
├── docs/                     # this documentation + adr/
├── src/
│   ├── core/                 # rivet_core
│   │   ├── geometry/         # Point, Size, Rect, Insets, Matrix, PageRotation
│   │   └── async/            # TaskScheduler, SerialExecutor, IMainThreadDispatcher
│   ├── render/               # rivet_render
│   ├── pdf/                  # rivet_pdf (interfaces + null engine)
│   │   └── pdfium/           # rivet_pdfium (only with RIVET_WITH_PDFIUM=ON); FPDF_* confined here
│   ├── editor/               # rivet_editor
│   ├── ui/                   # rivet_ui
│   ├── platform/             # rivet_platform (abstraction headers)
│   │   └── macos/            # rivet_platform_macos (AppKit host, CoreGraphics PaintContext, rivet executable)
│   └── app/                  # rivet_app
├── tests/
│   ├── harness/              # RivetTest.h, TestMain.cpp (internal micro-harness)
│   ├── core/  render/  pdf/  editor/  ui/
└── build/                    # preset build trees (gitignored)
```

Subsystems register their own `CMakeLists.txt`; the top level skips a
subdirectory until its `CMakeLists.txt` exists, so the tree can grow
incrementally during development.

---

## 11. Planned evolution

Within the approved scope, the architecture leaves room for:

- **Multi-document tabs** - one `DocumentSession` per open document, each with its own `SerialExecutor` and revision counter. No new machinery is required: `TileKey` already includes `DocumentId`, the shared `TaskScheduler` already multiplexes executors, and one cache can serve all sessions. Session lifetime follows tab lifetime.
- **Page editing** - landed in the editor layer (Phase 3): `editor::PageModel` is the session-owned mutable page list (stable, never-reused `PageId`s; each entry = source document + source page index + `PdfPageView` + `contentRevision`), mutated only by transactional `PageCommands` on the `CommandStack`, publishing an immutable `PageModelSnapshot` after every mutation that worker jobs capture instead of reading session state. Tiles, text pages and links are keyed by (`PageId`, `contentRevision`), so reorder/delete/duplicate invalidate nothing and rotate/crop only the affected page; `PageLayout` is rebuilt from the snapshot. Dirty state follows `CommandStack::stateId()`; saving/extracting builds a `PdfAssemblyRequest` from the snapshot. Content editing (text/objects) is expected to be the hardest part and will be developed gradually on the same command foundation.

Known limitations of the current foundation, recorded as future
architectural requirements:

- **Lazy page metadata**: `DocumentSession::create` loads every page's
  metadata synchronously on the calling (main) thread. Measured cost is a few
  microseconds per page for small documents, but a thousands-page document
  will need incremental/lazy metadata loading behind the same `PageLayout`
  interface.

Features beyond this (annotations, forms, editing, ...) are roadmap items in
the README and are not yet part of the architecture described here. Page
labels, outline/bookmarks, links, text selection/search and the workspace
model landed in Phase 2 (2026-09-25).
