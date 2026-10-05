# ADR-0018: Multi-document-type shell

Status: Accepted

Date: 2026-10-05

## Context

Rivet was designed as a PDF viewer/editor. The shell, workspace, and viewport were hardcoded for PDF
documents only. Supporting Markdown requires the shell to simultaneously hold both document types
(determined by file extension), dispatch events and rendering to the correct backend, and offer
kind-specific chrome (toolbars, sidebars, viewing modes).

Naive approaches:
- Separate executables or processes per document kind would duplicate the shell, widget tree, and session
  management infrastructure.
- A kitchen-sink plugin system (dynamic type registration, virtual factories) would add accidental
  complexity and overhead for a static two-kind choice (PDF and Markdown).

## Decision

The shell uses a **per-tab `DocumentKind` enum** (`Pdf` or `Markdown`) determined **once at open time from the file
extension** (`documentKindForPath()`). The choice is immutable for the tab's lifetime and drives a **minimal
kind-dispatched seam**.

**Model:**

- `DocumentTab` owns **one of two payloads**: a PDF `DocumentSession` (null for Markdown tabs) or a
  `MarkdownTabState` (null for PDF tabs). Both payloads implement a kind-agnostic contract: dirty
  tracking (`isDirty`), undo/redo, revision tracking (tabs' display caches key on revision).
- `DocumentKind` is a tag; no virtual/dynamic dispatch; the tag gates a `if (tab->isPdf()) ... else ...`
  check at the shell and controller layers.
- **Per-tab view state** (`ViewerState` + scrolling) is owned by `DocumentTab` and lives in the tab,
  not the viewport or view. Switching tabs swaps both the viewport/markdown-view and the state.

**Shell wiring:**

- `ShellController` owns one `PdfViewport` (slots PDF rendering) and one `MarkdownHostView` (slots
  Markdown rendering); only one is visible at a time, bound by `bindActiveTab()`.
- **Chrome switchover** (`applyChromeForKind`): shows/hides PDF-specific toolbar items (Annotate,
  Edit, Add Text) and the sidebar; shows/hides the Markdown mode buttons (Rendered / Source / Split).
  The layout runs when the kind changes.
- `FileController`, `ShellController` event handlers and platform menus remain **kind-agnostic**:
  Save, Undo/Redo, Copy/Paste dispatch through kind checks to the correct payload. Selection/search
  routes to the active tab's backend without caring which it is.

**No plugin framework.** New document kinds (past Markdown) require source edits; the architecture does
not reserve slots for extensibility. The cost of a true plugin system (discovery, version management,
sandbox, API versioning) is not justified by two static kinds.

## Consequences

- `DocumentTab` is a union type, not a discriminated interface. Platform-specific payloads stay
  separated (PDF lives in a `DocumentSession` with `PdfDocument` handles; Markdown lives in
  `MarkdownTabState`); the tab layer never knows their details, only the contract they implement.
- The shell remains simple: the seam is explicit `if (tab->isPdf())` checks that are easy to audit
  and reason about.
- Adding a third kind later requires editing `DocumentKind`, `DocumentTab`, `ShellController`,
  controller dispatch points, and the open-path file-type detection. No surprise, but not
  extensible without those edits.
- View/edit state per tab (`ViewerState`, markdown scroll position) does not get lost on tab
  switches, because it lives in the tab, not the viewport or view.

## Rejected alternatives

- **Shared `IDocument` virtual interface**: every payload would need virtual methods for dirty/undo/
  revision; both implementations would carry `vptr` overhead and force virtual dispatch in hot
  rendering/undo paths. A `DocumentKind` tag dispatch is simpler and faster.
- **Plugin system with dynamic registration**: acceptable for a mature product; adds layers
  (factory, service locator, version negotiation, upgrade paths) that Rivet does not yet need,
  and would require careful isolation of payload implementations.
- **Separate views per kind** (not a single shell wiring): forces duplication of tab strip, toolbar,
  keyboard routing, search bar, etc. Code size and maintenance would suffer.
- **Store kind in the file's magic bytes or sniffing**: unreliable and slow; `.md` vs `.pdf`
  extension is explicit and standard.
