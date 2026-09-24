# ADR-0002: Native per-platform architecture with thin Rivet abstractions

Status: Accepted

Date: 2026-09-24

## Context

Rivet is a desktop PDF editor for macOS first, with Windows and Linux
planned. The README states the product philosophy: native first, minimal
dependencies, "a normal desktop application, not a website wrapped inside a
desktop shell".

The platform layer must provide, per platform: window/view hosting, an input
event source, a 2D paint implementation, text services, file dialogs, and
main-thread dispatch. These differ substantially across macOS, Windows, and
Linux.

Two design pressures pull in opposite directions:

- Sharing all application code (editor logic, rendering, widgets) across
  platforms, to avoid triple maintenance.
- Using each platform's native capabilities for windowing, compositing,
  input, and text, to get native feel, startup time, and HiDPI behavior.

## Decision

Rivet uses **per-platform native backends behind thin Rivet-owned
abstractions**:

- `rivet_platform` declares abstraction headers only: file dialog,
  main-thread dispatch (`IMainThreadDispatcher`).
- `rivet_platform_macos` implements them with **AppKit** (`NSWindow`/`NSView`
  hosting, `NSOpenPanel`) and provides the **CoreGraphics** `PaintContext`
  implementation and **CoreText** text services. It also defines the `rivet`
  executable entry point.
- Shared layers (`rivet_ui`, `rivet_app`, `rivet_editor`, `rivet_render`,
  `rivet_core`) never see AppKit, CoreGraphics, or CoreText types. The
  `PaintContext` abstraction is the only surface through which widgets draw.
- Planned counterparts: Direct2D/DirectWrite on Windows and the native Linux
  stack, implementing the same abstraction headers.

**Explicitly rejected** as application foundations: Electron (and any
browser/WebView runtime), Qt, SDL, GLFW, GTK as an application framework,
wxWidgets, Flutter, and Dear ImGui. **Skia is also rejected as Rivet's
renderer** - Rivet renders through its own tile pipeline onto platform paint
contexts; PDFium's built-in rasterizer (AGG configuration) produces the page
bitmaps.

## Consequences

- The shared codebase (widgets, editor, rendering) is platform-independent
  C++23; adding a platform means implementing a small, well-defined set of
  abstractions, not porting the application.
- Native behavior (window chrome, input latency, HiDPI backing scale,
  text rendering, dialogs) comes from the OS.
- No browser runtime, interpreted UI layer, or large application framework is
  shipped, keeping startup fast, binaries small, and memory use low.
- The abstraction surface must stay genuinely small; any leak of
  `NSWindow`/`CGContext` types into shared code is a design bug and must be
  fixed in the abstraction instead.
- Per-platform paint/text backends must be kept behaviorally consistent
  (coordinate conventions, premultiplied alpha, color management) since the
  shared layers assume one contract.

## Rejected alternatives

- **Electron / WebView-based shells**: bundles a browser runtime; violates
  the native-first, low-memory, fast-startup goals. Explicitly ruled out in
  the README.
- **Qt / wxWidgets**: full application frameworks with their own widget
  systems, event loops, and styling; they would replace the native layer
  rather than wrap it, add a large dependency, and produce non-native
  behavior on macOS.
- **SDL / GLFW**: windowing/input libraries aimed at games or GL contexts;
  they do not provide native widgets, text services, or dialogs, and would
  still leave Rivet to build a UI toolkit on top.
- **GTK as the app framework**: ties the application to a non-native toolkit
  on macOS and Windows.
- **Flutter**: ships its own rendering runtime and Dart toolchain; not a
  native C++ model.
- **Dear ImGui**: immediate-mode UI suited to tooling/debug overlays; a
  document editor's retained UI (widgets, viewports, accessibility) fits a
  retained model better (see ADR-0004).
- **Skia as Rivet's renderer**: a large dependency; Rivet's rendering needs
  (page rasterization via PDFium, tile compositing via the platform paint
  API) are met without it.
