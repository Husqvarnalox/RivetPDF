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
| [ADR-0008](adr/ADR-0008-page-model-and-stable-page-identity.md) | Page model, stable page identity, assembled saves |
| [ADR-0009](adr/ADR-0009-background-save-rebase-and-file-lifecycle.md) | Background save, editing lock, rebase, dirty close/quit |
| [ADR-0010](adr/ADR-0010-atomic-save-replacement.md) | Atomic save by temp file + rename |
| [ADR-0011](adr/ADR-0011-annotation-model-identity-and-rendering.md) | Annotation model, identity and rendering split |
| [ADR-0012](adr/ADR-0012-annotation-persistence.md) | Annotation persistence through the page assembly |
| [ADR-0013](adr/ADR-0013-annotation-tools-and-interaction.md) | Annotation tools, interaction and on-screen rendering |

---

## 1. Overview

Rivet is organized as a set of statically linked CMake libraries plus one
executable:

- `rivet_core` - geometry, strong IDs, errors, logging, time, bitmaps, task scheduling, atomic file writing.
- `rivet_render` - coordinate transforms, page layout, zoom, viewer state, tile cache, render contracts.
- `rivet_pdf` - PDF engine interfaces (`PdfEngine`, `PdfDocument`, `PdfTypes`, page views/geometry, document assembly) and a null engine when PDFium is not compiled in.
- `rivet_pdfium` - the PDFium adapter; only built with `RIVET_WITH_PDFIUM=ON`.
- `rivet_editor` - page model, commands/undo, document sessions, the save pipeline, the render source that drives rasterization.
- `rivet_ui` - Rivet-owned retained-mode widgets.
- `rivet_platform` - thin platform abstraction headers (file dialog, main-thread dispatch).
- `rivet_platform_macos` - AppKit/CoreGraphics/CoreText implementation of the platform abstractions; hosts the application.
- `rivet_app` - shell wiring: toolbar, sidebar, status bar, viewport, `DocumentSession` management, page-editing and file-lifecycle controllers.
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
- **`AtomicFileWriter`** (`core/io/`): temp-file + `rename(2)` writer with `IByteSink`; every failure leaves the destination untouched ([ADR-0010](adr/ADR-0010-atomic-save-replacement.md)).
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

Engine-independent interfaces: `PdfEngine` (backend availability, `openDocument`), `PdfDocument` (owned document handle; `pageInfo`, `renderPage`), `PdfTypes` (`PdfDocumentInfo`, `PdfPageInfo`), `PdfPageGeometry` (`PdfBox`, `PdfPageView`, pure user/display-space mapping), `PdfAssembly` (`PdfAssemblyRequest`, `IPdfByteSink`; `PdfEngine::assembleDocument`), the content contracts `PdfContent` (`PdfPageContent` records, `PdfPageContentEdits` + `validate`, limits), `PdfTextLayout` (deterministic line breaking over measured advances) and `BundledFonts` (the three Apache-2.0 fallback faces and their coverage tables), and the `createEngine()` factory (`PdfSystem.hpp`). When `RIVET_WITH_PDFIUM=OFF`, `createEngine()` returns a null backend: `isAvailable()` is `false` and every operation reports `NotAvailable`. The application shell still launches in this configuration.

### `rivet_pdfium` (`src/pdf/pdfium/`)

The PDFium adapter, built only when `RIVET_WITH_PDFIUM=ON`. Implements the `rivet_pdf` interfaces on top of PDFium and links the imported target `PDFium::PDFium` created by `cmake/FindPDFium.cmake`. All `FPDF_*` usage is confined to this directory. Beyond rendering it provides text extraction (`PdfTextPage` in displayed-page coordinates), document outline (depth/node/cycle-bounded), page labels, and per-page links (internal destinations and scheme-validated external URLs), and document assembly for save/extract (`PdfiumAssembly`: `FPDF_CreateNewDocument`, `FPDF_ImportPagesByIndex`, `FPDF_MovePages`, `FPDFPage_Delete`, `FPDFPage_SetRotation`, `FPDFPage_SetCropBox`, `FPDF_SaveAsCopy`). `PdfiumContent` extracts page content (`pageContent`), applies content edits (`applyContentEdits`: one function used by the per-page materializer for display and by assembly for save) and runs the regeneration fidelity probe (section 14). Documents are opened through `FPDF_LoadCustomDocument` over a shared `PdfiumFileSource` (an open file descriptor), so a document keeps reading its original bytes even after a save atomically replaces the path. See [ADR-0003](adr/ADR-0003-pdfium-abstraction-boundary.md) and `docs/BUILDING_PDFIUM.md`.

### `rivet_editor` (`src/editor/`)

- **`PageModel` / `PageModelSnapshot` / `PageCommands`** - the editable page list and its commands; see section 10.
- **`Command` / `CommandStack`** - undo/redo with `execute` / `undo` / `redo` / `canUndo` / `canRedo` / `clear`, depth-bounded (100), with per-state `stateId()` for dirty tracking.
- **`PageSelection`** - page selection by `PageId` (set + active page + range anchor), independent of the text selection.
- **`DocumentSession`** - owns one open document: its `DocumentId`, the base PDF handle, the `PageModel`, the `PageLayout` (rebuilt from the snapshot after every model change), the `CommandStack`, dirty state, the editing lock, `rebaseOnto`, and a `SerialExecutor`-driven `DocumentRenderer`.
- **`DocumentSaver`** - `makeSaveJob` / `makeExtractJob` (main thread) and `runDocumentWrite` (worker): assembly into an `AtomicFileWriter`, plus reopening the written file for rebase; see section 11.
- **`DocumentRenderer`** - implements `IRenderSource`: dedupes requests, serializes PDF access through the session's `SerialExecutor`, stores results in the `TileCache`, and returns bitmaps via main-thread callbacks.
- **`ContentService` / `TextBlocks` / `ContentGeometry` / `ContentCommands`** - page content as the editor sees it: lazy extraction per (page, edits) into an LRU, the `ObjectId` registry, display-space object views, reconstructed text blocks with capabilities, z-order hit testing, and the undoable content edit factories; see section 14.

### `rivet_ui` (`src/ui/`)

A small, Rivet-owned retained-mode widget system: `Widget`, `Container`, `Button`, `Toolbar`, `ScrollBar`, `TextField` (UTF-8 caret/selection, echo masking), `TabStrip`, `PageThumbnailList` (lazy, virtualized thumbnails over the shared TileCache; emits selection/reorder/delete intents by `PageId`, including drag-and-drop with a drop gap), `OutlinePanel`, `PdfViewport`. Painting goes through a `PaintContext` abstraction that hides CoreGraphics. Widgets and event handling are main-thread-only. `ViewportTool` (an interface the viewport hosts for modal editing tools such as the crop tool; receives pointer/key events and paints an overlay). `IViewerTextBridge` (implemented by the app layer) connects the viewport to text features without an editor dependency. See [ADR-0004](adr/ADR-0004-retained-mode-rivet-ui.md).

### `rivet_platform` / `rivet_platform_macos` (`src/platform/`)

`rivet_platform` declares thin abstractions - file dialog, main-thread dispatch, clipboard, external URL opener, print service. `rivet_platform_macos` implements them with AppKit (`NSWindow`/`NSView` host, `NSOpenPanel`, `NSPasteboard`, `NSWorkspace` URL opening, `NSPrintOperation` printing), provides the CoreGraphics `PaintContext` implementation and CoreText text services, and defines the `rivet` executable entry point. It is the only module that touches AppKit/CoreGraphics/CoreText directly.

### `rivet_app` (`src/app/`)

Application shell wiring: `DocumentWorkspace`/`DocumentTab` (multi-tab workspace, asynchronous open with Loading/Ready/Error/NeedsPassword states, per-tab `ViewerState` + selection + search) and the `ShellController` composition root. Composes `rivet_ui` widgets with `rivet_editor` services.

`ShellController` owns the scheduler, engine, workspace and widget tree; it builds the tab strip, toolbar, viewport and loading overlay, runs the layout, owns keyboard focus and key routing (focused widget → Escape priorities → shortcuts → viewport), and rebinds everything when the active tab changes. It also handles tabs, window title, presentation mode, open and print. Feature logic lives in focused controllers that share one `ShellContext` (workspace, platform services, viewport, and status/focus/relayout sinks back into the shell):

- `TextInteractionController` — the viewport's `IViewerTextBridge` (text hit testing, selection, selection/search highlight rects, link hit testing), asynchronous copy via `TextService::requestRangesText`, internal link navigation and the external-URL policy.
- `SearchBarController` — the find bar; drives the active tab's `TextSearchController`. Result notifications from background tabs are ignored; switching tabs closes the bar.
- `SidebarController` — Pages (thumbnails) / Outline modes. The outline loads asynchronously via `LinkService::requestOutline` and is rebuilt only if the same session is still active. Expansion state is per document.
- `PasswordPromptController` — the masked prompt for NeedsPassword tabs; the field is cleared before `retryWithPassword`.
- `PageEditingController` — page selection (per tab, by `PageId`), thumbnail intents, rotate/delete/duplicate/move/crop/undo/redo as commands on the session's `CommandStack`, the crop tool, and the reaction to every page-model change (selection policy, text-selection invalidation, search restart, layout anchoring). Refuses edits while the session is editing-locked.
- `FileController` — Save / Save As / Extract / Split / Import / Merge, the save-in-flight bookkeeping and the dirty close/quit orchestration (section 11).
- `ContentController` / `ContentBarController` — the Edit (select object) and Add Text tools over a `ContentBackend` seam (section 14): hover/selection, drag-move and resize previews, nudges, delete, the inline on-page text editor, Add Text, Replace Image (decoded off the main thread by the platform `IImageDecoder`), and the properties bar.
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
`rivet_editor_tests`, `rivet_ui_tests`, `rivet_app_tests`, `rivet_platform_portable_tests` (plus the macOS-only `rivet_platform_tests`). Round-trip coverage of the real save path (assemble, atomic write, reopen, verify rotation/crop/order/encryption/deleted content) lives in `tests/pdf/TestPdfPageEditing.cpp`, `tests/editor/TestPageModelPdfium.cpp` and `tests/app/TestFileRoundTripPdfium.cpp` and runs only when PDFium is built in; the app-layer lifecycle/race tests (`TestFileController.cpp`, `TestPageEditingController.cpp`) and the editor model/command/saver tests run with fakes in every configuration.

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

Page editing adds a **view**: `pdf::PdfPageView` = absolute rotation + crop box in PDF user space (the effective `CropBox ∩ MediaBox`). `pdf::userToDisplay` / `displayToUser` / `userBoxToDisplayRect` / `displayRectToUserBox` (`src/pdf/PdfPageGeometry`) map between user space and page display space for any view, using the same formulas as above with the crop box as origin (`dx0 = x - crop.left`, `dy0 = y - crop.bottom`). Page display space is therefore always the space of the page's *current* view (what is rendered); a cropped page's display space starts at the crop box corner. See section 12 for how the crop tool uses this.

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
- **Saving/extracting/importing** run on the shared `TaskScheduler` (not on a document's `SerialExecutor`): `editor::runDocumentWrite` assembles and writes from an immutable `PageModelSnapshot`, and the import worker opens the source PDF and reads its page metadata. The assembly holds the PDFium call gate for its whole duration and its byte sink must not call back into the PDF layer. Completions return to the main thread through `IMainThreadDispatcher` (section 11).
- **Print spooling**: `editor::PrintSpooler` renders print bands on its own `SerialExecutor` (over the shared pool) and delivers progress/completion through `IMainThreadDispatcher`; the main thread only shows panels and composites pre-rendered band files (section 6, Printing).
- **Main-thread-only state**: the `PageModel`, `CommandStack`, `PageSelection`, dirty/editing-lock state, `DocumentSession::rebaseOnto` and all controllers. Workers never read them; they capture a `PageModelSnapshot` (or a copy of an entry) on the main thread. Observers (page-model change, dirty change, command-stack change) fire synchronously on the main thread and never under a lock.
- **Main-thread marshaling**: render callbacks and all async completions are delivered via `IMainThreadDispatcher`, implemented by the platform layer (dispatch to the macOS main queue in production). All widget and event handling is main-thread-only.
- **`TileCache`** is internally mutex-guarded: entries may be inserted, looked up and evicted from scheduler threads and the main thread concurrently.
- `ZoomState` and widget state are main-thread-only and not internally synchronized.

---

## 6. Rendering pipeline

Rendering is tile-oriented from day one ([ADR-0005](adr/ADR-0005-tile-based-rendering.md)).

- **Tile size**: 512 x 512 device pixels.
- **`RenderScaleKey`**: zoom quantized UP to multiples of 1/64 (`ceil(zoom * 64) / 64`, clamped to `[0.10, 64.0]`). Rounding up guarantees the raster is never produced at a lower resolution than requested; the painter scales down by less than 1/64. Zoom levels that quantize to the same key share tiles.
- **`PhysicalRenderScaleKey`**: device pixels per point = quantized zoom x display backing scale, quantized UP to multiples of 1/64 and clamped to `[0.1, 512]`. Two render requests that would produce different pixel dimensions (e.g. 100% zoom on a 1x vs a 2x display) never share a cache entry: `RasterParams::devicePixelsPerPoint` is always derived from this key, and `DocumentRenderer` rejects requests whose params disagree with the key.
- **`TileKey`** = `(DocumentId, PageId, PhysicalRenderScaleKey, tileX, tileY, contentRevision)` - cache identity per tile cell of the page grid; `contentRevision` is the page model's per-page content revision (rotate/crop mint a new one, so only the edited page misses; see section 10).
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

## 10. Page model, identity and undo

Full rationale: [ADR-0008](adr/ADR-0008-page-model-and-stable-page-identity.md).

**Model.** `editor::PageModel` is the session-owned ordered list of `PageEntry` (`PageId`, source document + source page index, `PdfPageView`, `contentRevision`, plus the source's media box and native view). Sources are never mutated. Every mutation is transactional and publishes an immutable `PageModelSnapshot` plus a `PageModelChange` diff; `DocumentSession` reacts (layout rebuild, eviction of text/links of deleted pages) and then calls the app's observer.

**PageId.** Minted by the model, never reused. Move, rotate and crop keep the id; duplicate and insert/import mint new ids; delete retires ids; undo restores the original ids (redo replays the ids minted by the first execute); rebase after save keeps all ids. `contentRevision` is minted model-wide by rotate/crop and restored exactly by undo/redo, so `(PageId, contentRevision)` names one view forever. Tiles (`TileKey.contentRevision`), text pages and links are cached by it: reorder/delete/duplicate invalidate nothing, rotate/crop only the affected page.

**Page source / assembly.** A snapshot converts to a `PdfAssemblyRequest` (`Save` = `PreserveBase` over every page; `Extract` = `Fresh` over a subset in model order). `PdfEngine::assembleDocument` builds the result on a private working document, never the live ones (PDFium: `FPDF_LoadCustomDocument` / `FPDF_CreateNewDocument`, `FPDF_ImportPagesByIndex`, `FPDF_MovePages`, `FPDFPage_Delete`, `FPDFPage_SetRotation`, `FPDFPage_SetCropBox`, `FPDF_SaveAsCopy` with `FPDF_NO_INCREMENTAL`). Page labels are shown only while the model is the identity order over the base pages; outline/link destinations resolve to the first entry presenting the source page (nothing once deleted).

**Commands and the single stack.** All page edits are `PageCommand`s (Move, Delete, Rotate, Duplicate, Insert, Crop) on the session's one `CommandStack` (depth 100). A multi-page operation is one command, one model mutation and one undo step. A failing command leaves the model untouched and reports `failure()`; only successful executes are pushed, and pushing clears redo. Delete refuses to remove every page (the model is never empty).

**Dirty checkpoint.** Every history position has a `stateId()`: 0 initially, a fresh monotonic id per successful execute, the lower state's id after undo, the command's id after redo. Evicting the oldest entries or `clear()` keeps the current id. `DocumentSession::isDirty()` is `stateId() != savedStateId_`; `markSaved()` records the current id. Undoing back to the saved state is clean again; a new command after an undo gets a new id and never falsely matches. `setOnDirtyChanged` fires on every flip. `rebaseOnto` clears the undo history but keeps the `stateId`, so a save leaves the document clean with an empty undo stack.

---

## 11. File lifecycle: save, extract, import, close

Full rationale: [ADR-0009](adr/ADR-0009-background-save-rebase-and-file-lifecycle.md), [ADR-0010](adr/ADR-0010-atomic-save-replacement.md).

```text
 main: makeSaveJob (snapshot + request), lock editing
   worker: assembleDocument -> 256 KiB buffered sink -> AtomicFileWriter
           (temp in dest dir, fsync, rename)  -> reopen file, read page metadata
 main: rebaseOnto (same PageIds, sources -> new file, undo cleared)
       -> markSaved -> unlock editing
```

- **Background serialization**: assembly and the atomic write run on the shared pool, never the main thread; PDFium access is under the process-wide call gate for the whole assembly.
- **Editing lock**: while a save is in flight `DocumentSession::execute/undo/redo` refuse, so the written snapshot is exactly the state that is marked saved and rebased. Viewing, selection, search, print and extract keep working.
- **Atomic replacement**: temp file in the destination directory then `rename(2)`; any failure leaves the destination intact and no temp file ([ADR-0010](adr/ADR-0010-atomic-save-replacement.md)). Saving over the document's own path is supported because the live document reads through its own open descriptor (`PdfiumFileSource`).
- **Rebase**: the session switches to the freshly written file with identical ids/order; previous documents are released once in-flight jobs drop their snapshots.
- **Operation identity**: `FileController` addresses async completions by `(TabId, generation)`; `TabId`s are never reused and a generation is minted per operation. A completion applies only while its tab is alive and Ready (imports also only while the generation is registered). A completion for a closed tab is dropped (the file was still written atomically). A heap-owned `alive_` flag guards completions against a destroyed controller. **Modals re-resolve**: the main-thread dispatcher is drained while a modal (alert, save/open panel) runs, so a completion can close a tab during the prompt; every flow therefore keeps a `TabId`, never a `DocumentTab*`, across a modal and re-resolves it afterwards (a vanished or no-longer-Ready tab ends the flow). **Worker fence**: every worker task holds a `core::AsyncScope` token; `~FileController` cancels the scope (writers poll it through `DocumentWriteControl::cancelled`, aborting before the commit) and waits for active tasks, so no worker outlives the controller.
- **One save at a time** per shell: interactive saves during a save are reported; quit-lifecycle saves are queued and chained.
- **Dirty only cleared on success**: `markSaved()` runs only in the success path; a failed save unlocks editing and leaves the document dirty.
- **Import / Merge**: open panel; a worker opens the source and reads its page metadata; an `InsertPagesCommand` inserts all pages (new ids, native views) before the current page, after it, or appended, as one undo step. Encrypted sources are rejected.
- **Extract**: save panel; a worker assembles a `Fresh` document from the selected pages in model order. The source model and its dirty state are not affected; document-level structure (outline, metadata, forms, labels, encryption) is not carried over.
- **Split by ranges** (`FileCommand::Split`): an export that reuses the Extract pipeline and never touches the source (not dirty, model, selection, history unchanged). Flow: `IAlertService::promptForText` for the ranges (default implementation returns nullopt; NSAlert + text field on macOS) -> `editor::parsePageRanges` (1-based inclusive, separated by commas/whitespace/newlines; rejects empty, 0, negative, garbage, `end < start`, out of range, overflow and overlapping ranges, with specific messages) -> save panel (its path supplies the output directory and base name) -> `editor::splitOutputPaths` (`<stem>_<a>-<b>.pdf`, `<stem>_<a>.pdf` for one page; one trailing `.pdf`, any case, is stripped so `.pdf.pdf` never occurs) -> **collision check**: if any output already exists nothing is written and the split is reported and aborted (never overwrites); each output is also committed with `overwriteExisting = false` (`renamex_np(RENAME_EXCL)` on macOS, best-effort re-check elsewhere), so a file that appears after the check is not replaced either -> one `makeExtractJob` per range, all captured up front on the main thread from one model snapshot -> ONE worker task runs them sequentially with `runDocumentWrite`. **Partial-success semantics**: the first failure stops the run; outputs already written remain, the failing output leaves nothing under its final name (atomic write) and the status names it and the "N of M files written" count; there is no cross-file transaction. **Close/quit**: like Extract, closing the tab does not stop a split (jobs own their snapshots); destroying the `FileController` (quit) cancels the worker scope, which the worker polls through `DocumentWriteControl::cancelled` (the current output aborts before its commit and later ones never start); the destructor waits for the worker to return and the completion is dropped.
- **Dirty close/quit**: a dirty tab prompts Save / Don't Save / Cancel; several dirty tabs use the platform review prompt. "Save" on a tab close closes the tab when its save settles; window close with "Save" performs the saves and leaves the window open; quit defers the platform reply until every accepted save settled. With no alert service the answer is Cancel (nothing is discarded silently). A save that *fails* keeps the tab open and aborts the quit. "Don't Save" closes exactly the tab it was asked about (the tab is re-resolved by id after the prompt).

---

## 12. Crop semantics

The crop tool (`app::CropTool`, a `ui::ViewportTool`) edits one page's crop box over the rendered page; it never mutates the document, it reports `onApply(PdfBox)` / `onReset` / `onCancel` and the owner runs `CropPagesCommand`.

- **Stored form**: a crop is a `PdfBox` in PDF **user space** (the page's unrotated space, origin bottom-left) kept in the entry's `view.cropBox`; rotation is an independent absolute value in `view.rotation`. Saving writes them to `/CropBox` (`FPDFPage_SetCropBox`) and `/Rotate` (`FPDFPage_SetRotation`).
- **Editing space** (`CropFrame`): the *uncropped display space* - display space (points, top-left origin, y-down, current rotation applied) of the view `{view.rotation, mediaBox}`. The whole media box is `{0, 0, displaySize}` and the current crop is a sub-rect of it. The currently rendered page is that space translated by the current crop rect's origin, so the tool can extend the crop beyond the rendered page up to the media box (`base_` is the rendered page's origin in this space).
- **Conversions**: `toUserBox` clamps the edited rect to the media rect, maps it with `displayRectToUserBox` over the uncropped view (so rotation is handled by the same mapping as rendering), and intersects the result with the media box; `fromUserBox` is the clamped inverse (`userBoxToDisplayRect`). Rotating a page after cropping keeps the user-space box and only changes how it is displayed.
- **Clamping**: every edge is clamped into the media rect; the minimum size is 18 pt per axis (`kMinSizePoints`, reduced to the media size when smaller); drag-to-move is clamped to the media box; hit testing uses fixed logical-point handle sizes (independent of zoom and backing scale).
- **Targets**: the tool edits the current page; the command applies the resulting user-space box to the whole selection when the current page belongs to it, otherwise to the current page only. The same user-space box is applied to every target, and the command fails atomically if it does not lie within any target's media box. Reset (`std::nullopt`) restores each page's native crop box.
- **Command**: `CropPagesCommand` validates with `isValidViewFor` (non-empty crop inside the media box, 0.01 pt slack), mints a fresh `contentRevision` per page and is one undo step; undo/redo restore the recorded views and revisions.

---

## 13. Annotations

Full rationale: [ADR-0011](adr/ADR-0011-annotation-model-identity-and-rendering.md) (model, identity, rendering split), [ADR-0012](adr/ADR-0012-annotation-persistence.md) (persistence), [ADR-0013](adr/ADR-0013-annotation-tools-and-interaction.md) (tools and interaction).

```text
 pdf:     PdfAnnotationData (user space) + limits + buildAppearance (one description for overlay and /AP)
 pdfium:  annotations(page) from a private reader document (bounded cache) | render with hidden indices
          | assembly annotationEdits {removeIndices, create} -> report {createdIndices, annotsCount}
 editor:  PageEntry.annotations (immutable PageAnnotationState) + rasterRevision
          AnnotationService (lazy originals, LRU, id registry, display-space views, hitTest)
          AnnotationCommands (factories in display space -> AnnotationStateCommand on the one CommandStack)
 ui:      ViewportLayer slot in PdfViewport, Path/fillPath/strokePath/drawTextInBox, TextArea
 app:     AnnotationInteraction (pure state machine) -> AnnotationController -> commands
          AnnotationLayer (events/paint) + AnnotationPainter + note editor
```

- **Identity**: `core::AnnotationId`, minted by the editor and never reused. Originals get ids from a registry keyed by `(PageId, /Annots index)`; created annotations get theirs when the command is built, so redo and undo restore the same ids. Duplicate and import mint fresh ids; a save re-keys the registry from the writer's report and keeps every id.
- **Storage and coordinates**: data lives in the source page's PDF user space (like the crop box), so rotate/crop never rewrite annotations. Everything above the editor is display space of the page's current view; all conversions go through `pdf/PdfPageGeometry` wrapped by `editor/AnnotationGeometry`.
- **Page state**: `PageEntry::annotations` is a shared immutable `{suppressed /Annots indices, overlay items}` (null = untouched). Commands swap state pointers (undo is O(changed pages)). `rasterRevision` (tiles) changes with rotate/crop and when the suppressed set changes; text and links stay on `contentRevision`, so annotation edits never invalidate text or the selection, and pure overlay edits invalidate no tiles at all.
- **Rendering split**: untouched originals (including all unsupported kinds) are drawn by the PDFium raster with their own appearances. Created/edited annotations are drawn by Rivet as a vector overlay from the same `buildAppearance` description the writer saves. Suppressed originals are hidden from the raster by setting the Hidden flag for one gated render and restoring it. Nothing is drawn twice; after a save the written annotations stay overlay-drawn and suppressed, so the rebased raster is unchanged and no tile flush happens.
- **Loading**: originals load lazily per page on a worker under the PDFium call gate, delivered on the main thread with alive/generation guards; cached in a bounded LRU (128 pages in the service, 64 in the PDFium reader). The cache holds only originals, so eviction never loses an edit.
- **Persistence**: the snapshot's states become per-page `annotationEdits` applied after the view on the private working document (remove suppressed indices descending, append created ones with generated `/AP`, verify the count). Editing an existing annotation is remove + re-create. Unsupported annotations are never rewritten. Save goes through the Phase 3 pipeline (background, atomic, editing lock, rebase).
- **Interaction**: one tool state per shell (Select, markup, Note, Ink, shapes, Stamp); the annotation layer sits between the crop tool and the viewport's own handling. Markup is created from the text selection (one annotation per page, one undo step). Selection is single and per tab; move/resize are previewed and committed as one command. Esc: gesture → note editor → selection → Select tool.
- **Safety**: no actions run on annotation clicks (no JavaScript, Launch, attachments; links keep their explicit-click path). Contents and authors are untrusted, never logged, capped; per-page item, quad, ink stroke/point and length limits make over-limit annotations opaque and refuse creation past them.
- **Page operations**: reorder/rotate keep states; crop keeps annotations outside the crop (hidden by the clip, back on reset - nothing is silently destroyed); duplicate copies with fresh ids; delete + undo restores state, ids and revisions; import carries originals through the page import.

---

## 14. Content editing

Full rationale: [ADR-0014](adr/ADR-0014-content-object-model.md) (object model, identity, capabilities), [ADR-0015](adr/ADR-0015-text-editing-strategy.md) (in-place text editing, blocks, reflow), [ADR-0016](adr/ADR-0016-font-embedding-and-fallback.md) (fonts), [ADR-0017](adr/ADR-0017-content-regeneration-and-save.md) (regeneration, display materialization, save, rebase).

```text
 pdf:     PdfPageContent (user space records: type, matrix, bounds/quad, font, origin Source(i)|Created(tag))
          PdfPageContentEdits {objects: remove/transform/replaceImage; textBlocks: tag, members, text, font,
          size, color, rigid placement, wrapWidth, lineAdvance} + validate(edits, sourceObjectCount) + limits
          PdfTextLayout (deterministic wrapping) | BundledFonts (Arimo/Tinos/Cousine subsets, coverage)
 pdfium:  pageContent(index, edits) | renderPage/textPage(..., edits) through a per-page materializer
          | applyContentEdits (shared by materializer and assembly) | regeneration fidelity probe
          | assembly contentEdits -> PdfAssembledPageContent {origins, blockTags} report
 editor:  PageEntry.contentEdits (immutable pointer) + contentRevision + rasterRevision
          ContentService (lazy extraction, LRU, ObjectId registry, display-space views, blocks, hitTest, rebased)
          ContentCommands (factories in display space -> ContentEditsCommand on the one CommandStack)
 ui:      ContentLayer (ViewportLayer after the annotation layer), TextArea reused for the inline editor
 app:     ContentInteraction (pure state machine) -> ContentController -> ContentBackend -> commands
          ContentBarController (properties), platform IImageDecoder + openImage
```

- **Model**: a page is its source page plus one immutable `PdfPageContentEdits` (null = untouched). Object edits are keyed by the SOURCE object index (remove, accumulated affine transform, image replacement); text edits are blocks keyed by a tag that is the block's `ObjectId` value. Commands swap the pointer and mint a fresh `contentRevision` and `rasterRevision` for that page only (tiles, text, search and links of the page are invalidated; annotations, which are keyed by `/Annots` index, are not). Undo is O(changed pages).
- **Identity**: `core::ObjectId` is minted by the editor; the registry maps `(PageId, source index)` and `(PageId, tag)` to ids and is never evicted or reissued, so undo/redo and re-extraction keep ids. After a save the registry is re-keyed from the assembly report (`ContentService::rebased`); a page without a report drops its entries and ids are minted afresh on the next resolve.
- **Coordinates**: edits and records live in the source page's user space; everything above the editor is display space of the entry's current view through `ContentGeometry` (one tested mapping for translation, resize, upright placement and quads). Rotation and crop never rewrite content.
- **Display**: the backend renders and extracts an edited page from a scratch document (the page imported from a private, never-rendered copy, edits applied, saved to memory and reloaded) so the viewed raster is exactly what a save produces. Extraction after an edit is asynchronous; the resolved view is marked stale (`loaded = false`) until it lands, and the factories that only need identities (move, delete, retype, Add Text, replace, bring to front) keep working on the stale view while resize waits for fresh geometry.
- **Text**: blocks are reconstructed from text objects (same font/size/baseline run, consistent line advance) and classified: FullyEditable (own embedded/standard font with a Unicode mapping), Replaceable (retype re-sets the block in a bundled substitute when the font cannot encode the text), MoveOnly, ReadOnly (with a reason shown in the UI). Editing replaces the block's own objects in place with the same font when possible; reflow is a deterministic local wrap within the block's width; a longer text extends the block downward (explicit overflow policy, never clipped silently).
- **Fonts**: existing text keeps its font when the font can write the new text; otherwise a metric-compatible bundled face (Sans/Serif/Mono, Regular/Bold, Apache-2.0 subsets in `third_party/fonts/`) is embedded through the backend. No platform text APIs in the portable layers; no synthetic bold.
- **Safety**: the regeneration probe refuses pages whose content PDFium cannot rewrite faithfully (shading, inline images, Type3, patterns, hidden optional content, ...), so edits never silently destroy content. Per-page object, text and image limits make over-limit pages read-only. Annotations, widgets and links are never listed as content; nested Form XObjects are read-only. Nothing in a document's content is logged.
- **Interaction**: one tool state per shell; the content layer sits after the annotation layer. Selection is single and per tab; drag-move, resize and nudge bursts are one command each (nudges coalesce within 500 ms by undo + combined move, guarded by the command-stack state id). The inline editor commits as one undo step and is committed by Save, close and quit.
- **Save**: assembly applies the page's edits after the view through the same `applyContentEdits`; the report re-keys identities; the written document is reloaded and shows the same content without a tile flush.

---

## 15. Directory layout

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
│   │   ├── io/               # AtomicFileWriter (temp file + rename)
│   │   └── async/            # TaskScheduler, SerialExecutor, IMainThreadDispatcher
│   ├── render/               # rivet_render
│   ├── pdf/                  # rivet_pdf (interfaces, page geometry, assembly + content contracts, bundled fonts, null engine)
│   │   └── pdfium/           # rivet_pdfium (only with RIVET_WITH_PDFIUM=ON); FPDF_* confined here
│   ├── editor/               # rivet_editor (page model, commands, session, saver, render/text/link/annotation services)
│   ├── ui/                   # rivet_ui
│   ├── platform/             # rivet_platform (abstraction headers)
│   │   └── macos/            # rivet_platform_macos (AppKit host, CoreGraphics PaintContext, rivet executable)
│   └── app/                  # rivet_app (shell, workspace, controllers, crop tool, annotation and content tools)
├── third_party/fonts/        # bundled fallback fonts (Apache-2.0 subsets) + tools/fonts/subset_fonts.py
├── tests/
│   ├── harness/              # RivetTest.h, TestMain.cpp (internal micro-harness)
│   ├── core/  render/  pdf/  editor/  ui/  app/  platform/
└── build/                    # preset build trees (gitignored)
```

Subsystems register their own `CMakeLists.txt`; the top level skips a
subdirectory until its `CMakeLists.txt` exists, so the tree can grow
incrementally during development.

---

## 16. Planned evolution

Within the approved scope, the architecture leaves room for:

- **Multi-document tabs** - one `DocumentSession` per open document, each with its own `SerialExecutor` and revision counter. No new machinery is required: `TileKey` already includes `DocumentId`, the shared `TaskScheduler` already multiplexes executors, and one cache can serve all sessions. Session lifetime follows tab lifetime.
- **Page editing** - landed in Phase 3, now closed (sections 10-12, [ADR-0008](adr/ADR-0008-page-model-and-stable-page-identity.md), [ADR-0009](adr/ADR-0009-background-save-rebase-and-file-lifecycle.md)). Content editing landed in Phase 5 on the same command foundation (section 14).

Known limitations of the current foundation, recorded as future
architectural requirements:

- **Lazy page metadata**: `DocumentSession::create` loads every page's
  metadata synchronously on the calling (main) thread. Measured cost is a few
  microseconds per page for small documents, but a thousands-page document
  will need incremental/lazy metadata loading behind the same `PageLayout`
  interface.

### Phase 3 status: CLOSED (2026-10-02)

Page editing (reorder, rotate, delete, duplicate, crop, import/merge,
extract, split by ranges, undo/redo) and the file lifecycle (background
save/Save As, atomic replacement, dirty close/quit prompts) are complete.
The Phase 3 quality gate (manual QA, perf probes, correctness review) found
no open P0/P1 issues. The following P2 items are known and deferred:

- **Window-close Review with 2+ "Save" answers**: the second interactive
  save is refused ("A save is already in progress") because window-close
  saves are not queued like quit saves; the tab stays dirty (no data loss).
- **Close/discard and printing**: `saveSettled` / `discardAndClose` do not
  call `cancelPrintForTab`; a print of a closing tab still runs off its own
  snapshot.
- **Import insertion index**: `beforeIndex` is captured when the import
  starts and can be stale if pages are added or removed before the worker
  completes (pages still land in a valid position, clamped).
- **`rebaseOnto` failure path** does not restore `info_` / `baseLabels_` /
  `path_`; only reachable if reopening the just-written file fails.
- **Non-interactive save job-build failure** does not call `saveSettled`;
  not reachable today (job building only fails for an unavailable engine).
- **Status bar** "N pages" is not refreshed after a delete until the next
  page change.
- **Sidebar wheel scroll** uses raw deltas (slow with a mouse wheel); the
  outline "No outline" placeholder is drawn flush against the left edge.
- **Split save panel** asks to "Replace" the base name (e.g. `report.pdf`)
  although only suffixed outputs are written and the base file is never
  touched.
- **Linux no-replace** for split outputs is a best-effort re-check (no
  portable atomic rename-without-replace); macOS uses `RENAME_EXCL`.
- **Quit waits for export/split workers**: the worker scope is cancelled
  cooperatively, so quit is delayed by at most the current output's write.
- **ShellController tab-close re-resolve** is covered by the FileController
  contract tests, not by a ShellController-level test.

UX notes:

- **Duplicate -> Undo -> Redo selection**: after redo the selection lands
  deterministically on the page after the original (no stale `PageId`), not
  on the re-created duplicate. Cosmetic; candidate for UX polish.
- **GCC 16 `-Wnull-dereference` / `-Warray-bounds`** in tests were analysed
  as false positives (`detachChild(nullptr)` after inlining; a devirtualised
  `make_shared<Bitmap>`), not undefined behaviour. They are fixed locally
  (an explicit null guard in `Widget::detachChild`, a plain `shared_ptr`
  construction in the test); no warning is disabled globally. GCC 13's
  `-Wnull-dereference` inside `<streambuf>` for `istreambuf_iterator` is
  likewise a false positive; tests read files with `read()`/`gcount()`.

### Phase 5 status: CLOSED (2026-10-03)

Content editing (section 14) is complete for the approved scope: object
selection, move/resize/delete/nudge, in-place text editing with the inline
editor, Add Text, font fallback, deterministic reflow, image replacement,
undo/redo, display materialization and save. Known limitations, recorded
honestly:

- **Z-order**: existing objects cannot be reordered; only text added by
  Rivet can be brought to the front. A block re-set in a bundled font, or
  one that needs more lines than it had objects, gets its new text objects
  appended on top of the page content.
- **Rotated text** is edited unrotated in the inline editor and keeps its
  rotation on commit; the editor is not clipped to the viewport.
- **Read-only pages**: pages whose content fails the regeneration probe
  (shading, inline images, Type3 fonts, patterns, hidden optional content,
  CMYK text, character spacing, text clipping, TrueType fonts without
  widths, shared image streams) are shown with the reason and cannot be
  edited at all; nested Form XObjects are read-only everywhere.
- **Fonts**: italic originals fall back to the upright bundled face; the
  bundled subsets cover Latin, Greek and Cyrillic only (other scripts are
  refused with the offending code points).
- **Undo/Redo menu titles** are static (do not show the command name).
- **Outline snap-back**: after an edit the selection outline shows the
  previous geometry for one extraction round trip.
- **Inline editor preview**: the editor lays the text out with the UI
  font, so its line breaks can differ from the PDF's by a word; the commit
  reflows with the block's PDF wrap width and font metrics.
- **Annotate button**: it only shows the annotation bar; the content tool
  (and its selection) ends when an annotation tool is chosen.

Features beyond this (forms, ...) are roadmap items in the README and are
not yet part of the architecture described here. Page labels,
outline/bookmarks, links, text selection/search and the workspace model
landed in Phase 2 (2026-09-25); page editing and the file lifecycle in
Phase 3; annotations (section 13) in Phase 4; content editing (section 14)
in Phase 5.
