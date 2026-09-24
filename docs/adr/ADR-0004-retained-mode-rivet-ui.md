# ADR-0004: Small Rivet-owned retained-mode widget system

Status: Accepted

Date: 2026-09-24

## Context

Rivet needs a UI: a window shell with toolbar, sidebar, status bar, and a PDF
viewport, plus dialogs and small controls. The constraints:

- The application must look and behave natively (ADR-0002), but the shared
  layers must not use AppKit/CoreGraphics types directly.
- Rivet is a C++23 codebase with a strict no-large-dependency policy.
- A PDF editor's UI is dominated by one complex custom widget - the document
  viewport - plus a modest number of ordinary controls. Heavyweight widget
  toolkits bring far more surface than needed.
- Document UIs are retained-mode by nature: widgets persist, repaint on
  damage, and own state between frames.

## Decision

Rivet builds a **small, Rivet-owned, retained-mode widget system** in
`rivet_ui`: `Widget` (base), `Container`, `Button`, `Toolbar`, `Sidebar`,
`PdfViewport`.

- Retained mode: widgets are persistent objects that own state and repaint
  through a `PaintContext` abstraction; there is no per-frame immediate-mode
  rebuild of the UI.
- `PaintContext` hides CoreGraphics: the platform layer
  (`rivet_platform_macos`) implements painting on top of CoreGraphics (and
  the corresponding API on future platforms), so widgets draw through one
  interface.
- Platform backends implement painting and input delivery: the host view
  forwards events to the widget tree on the main thread and provides the
  paint context during draw.
- All widget and event handling is main-thread-only.
- No general GUI framework is introduced; the widget set grows only as the
  shell needs controls.

## Consequences

- The UI layer is small, auditable, and fully under Rivet's control; no
  third-party widget toolkit appears in the dependency tree.
- Porting to Windows/Linux means implementing `PaintContext` and input/host
  abstractions; the widget tree, shell, and viewport port unchanged.
- Rivet must build and maintain every control it needs (buttons, toolbars,
  sidebars today; menus, fields, and lists as the shell grows). This is
  accepted: the required set for a document editor is modest, and the
  viewport - the hardest widget - is custom in any framework.
- Consistency with native platform conventions (focus, key handling,
  accessibility) is the platform backend's responsibility and must be kept
  aligned per platform.

## Rejected alternatives

- **Qt / wxWidgets**: would impose their own widget systems, event loops,
  and styles on the whole application, conflicting with the native-first
  architecture (ADR-0002).
- **Dear ImGui (immediate mode)**: immediate-mode UI rebuilds every frame and
  keeps state outside the widget tree; a document editor benefits from
  retained widgets that repaint on damage and integrate with native input,
  and an immediate-mode core would fight the native hosting model.
- **Native UI per platform with no shared layer** (e.g. AppKit
  storyboards + WinUI + GTK, each separate): triples shell maintenance and
  forces business logic (toolbar actions, session management) to be written
  per platform; the shared retained widget tree with native backends avoids
  this.
- **Web-based UI (Electron/WebView)**: rejected at the architecture level
  (ADR-0002).
