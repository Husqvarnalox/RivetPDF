# ADR-0013: Annotation tools, interaction and on-screen rendering

Status: Accepted (integrated with Phase 4; manual QA pass pending)

Date: 2026-10-02

## Context

ADR-0011 and ADR-0012 define the annotation model and its persistence.
This ADR fixes how the user works with annotations on screen:

- Tools and their state.
- Selection and how it coexists with text selection.
- Hit testing.
- Overlay painting.
- The note editor.
- Keyboard handling.

The constraints are:

- `PdfViewport` must stay a document view, not a God object. The crop tool
  already uses the `ViewportTool` slot (ADR-0008), and annotations must not
  take that slot permanently.
- `ui` and `editor` must not depend on each other. Everything the UI paints
  comes from the editor as display-space data, through `app`.
- Interaction logic must be unit-testable without a window, a platform
  layer or PDFium.

## Decision

### Tool state

- The app owns one tool state per shell:
  - `Select`.
  - `Highlight`, `Underline`, `StrikeOut`.
  - `Note`.
  - `Ink`.
  - `Rectangle`, `Ellipse`, `Line`, `Arrow`.
  - `Stamp`.
- Each creation tool has a current style (colour, opacity, width, fill,
  stamp name), remembered per tool for the session.
- **Esc**, in priority order:
  1. Cancels an in-progress gesture.
  2. Otherwise closes the note editor, committing it.
  3. Otherwise clears the annotation selection.
  4. Otherwise returns to `Select`.
- Escape priorities that already exist (find bar, presentation, crop) keep
  precedence.
- Switching tabs ends the current gesture and closes the note editor, which
  commits it. The tool stays.

### Layering: an annotation layer, not a tool

`PdfViewport` gets a second, independent slot, the annotation layer
(`ui::ViewportLayer`). It is always installed while a document is shown.
For pointer and key events the order is:

1. The active `ViewportTool` (crop).
2. The annotation layer.
3. The viewport's own handling (text selection, links, scrolling).

Wheel and pinch events never reach the layers. The layer also gets an
`afterMouse` notification once the viewport has handled an unconsumed
event, so markup tools can turn a text selection into markup on mouse-up.

For each visible page, painting runs in this order:

1. Tiles.
2. The layer's `paintPage` (overlay-drawn annotations).
3. Text selection, search and link overlays.
4. The layer's `paintAbove` (selection outline, handles, gesture preview).
5. The active tool.

The viewport knows nothing about annotations. It only calls the slot.

The concrete layer (`app::AnnotationLayer`) is thin. It forwards events,
converted to `(pageIndex, page display point)` with the viewport's page
rects and zoom, into a pure state machine. It paints what the controller
resolves.

### Pure interaction state machine

`app::AnnotationInteraction` has no dependency on ui, editor or the
platform. Its inputs are:

- Pointer down, move and up, with the page index, the display point in
  page points, the modifiers and the zoom.
- Keys.
- A hit-test callback.

Its outputs are intents:

- Select an id, or clear the selection.
- Begin, update, commit or cancel a gesture.
- Delete the selection.
- Open the note editor.

Every tool's gesture is a small value type:

- Markup: none. It uses the text selection.
- Note: a click.
- Ink: point capture with Ramer–Douglas–Peucker reduction at commit,
  tolerance 0.5 / zoom points.
- Shapes: a drag rectangle. Shift constrains to a square or circle, or to
  45° for a line or arrow.
- Stamp: a click places the default size, a drag sets the size.

Below a drag threshold of 3 logical points, a click on Rectangle or Ellipse
creates nothing, and a click on Line or Arrow does the same.

`app::AnnotationController` applies the intents to the active session. It
builds editor commands with the factories, runs them through
`DocumentSession::execute` (refused while the editing lock is held) and
reports failures in the status bar.

### Selection

- Selection is single, keyed by `AnnotationId`, and stored per tab.
- A click with the `Select` tool hits annotations first:
  - Hit testing is topmost-first, in the editor, in page display points.
  - The tolerance is 4 logical points / zoom.
  - A hit selects the annotation and clears the text selection.
  - A miss falls through to the viewport, so text selection and link
    clicks work unchanged. The miss also clears the annotation selection.
- The same click-through rule applies under the other tools, apart from
  the gesture start.
- A selected annotation shows its bounds outline. Annotations with the
  `Resize` capability show 8 handles; Line and Arrow show 2 endpoint
  handles. Handles are fixed-size in logical points.
- Dragging inside a movable selection moves it. Dragging a handle resizes
  it. Both are previewed live from the gesture, and exactly one
  Move/Resize command runs on mouse-up. One drag is one undo step.
- **Delete** and **Backspace** delete the selected annotation when the
  viewport has focus. Page deletion stays on its existing route (sidebar
  and menu).
- Ids that no longer resolve after undo, page deletion or a rebase without
  a report clear the selection lazily.

### Decisions made during implementation

- `ui::PointerEvent` has no click count, so `AnnotationLayer` detects
  double-clicks itself: 0.4 s and 5 logical points, with an injectable
  clock for tests.
- A press that hits an annotation selects it under every tool except Ink,
  which draws over annotations. Creation gestures start only on empty page
  areas.
- The tool strip is a second toolbar row, toggled by "Annotate" in the main
  toolbar. Choosing a tool from the menu reveals the strip. Hiding the strip
  returns to `Select`. The strip shows only the style controls that apply to
  the current tool or selection.
- The macOS key mapping gives Space, '+' and '-' their typed text as well as
  their dedicated keys. Zoom shortcuts still work, and text fields and the
  note editor receive these characters.

### Markup from text

Highlight, Underline and StrikeOut are created from text selection quads.

- **Selection first:** with a text selection active, choosing the tool
  creates the markup immediately.
- **Tool first:** with the tool active, a text drag selects text as usual.
  On mouse-up the selection becomes markup and is then cleared.

The quads are the text selection's per-line rects in page display space.
These are converted to `PdfQuad`s by the editor, with the order TL, TR, BL,
BR in display terms, mapped to user space. One annotation is created per
page, all in one undo step.

### Painting

`app::AnnotationPainter` paints each `drawnByOverlay` annotation's
`DisplayAppearance`:

- Paths become `PaintContext::fillPath`/`strokePath`, which are new.
- The appearance opacity multiplies every colour's alpha. This is
  per-operation, like the saved `/GS gs`.
- Stamp labels use the new `PaintContext::drawTextInBox`: one line in a
  bold system font, fitted and rotated by quarter turns.

Geometry is mapped as viewport = page origin + display × zoom. Annotations
painted by the raster are not painted again. Hidden originals are excluded
from the raster through the render target (ADR-0011). Nothing is ever drawn
twice.

### Note editor

A note's contents are edited in a Rivet panel:

- The panel holds a new multi-line `ui::TextArea`: UTF-8 code-point-safe
  caret, Enter inserts a newline, Cmd+Enter commits.
- It is anchored next to the note and opens on creation or double-click.
- Closing commits one EditContents command when the text changed.

Contents are untrusted. They are never logged, never interpreted, capped at
`kMaxContentsBytes` and rendered only as plain text.

### Safety

Annotation clicks never run actions:

- No JavaScript.
- No Launch.
- No file attachment opening.
- URI and GoTo links keep their existing explicit-click handling, with the
  URL policy unchanged.

## Consequences

- The annotation feature adds:
  - Two slot calls in `PdfViewport`.
  - Path primitives in `PaintContext`.
  - A text-area widget.
- Everything else lives in `app` (thin) and `editor` (logic).
- Crop and annotation interaction compose: crop takes precedence while
  active.
- On-screen and saved appearances share one description. Anti-aliasing
  differs slightly between CoreGraphics and PDFium.
