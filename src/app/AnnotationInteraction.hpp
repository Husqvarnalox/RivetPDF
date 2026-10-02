// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationTool.hpp"

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace rivet::app {

// The pure interaction state machine of the annotation tools (ADR-0013). It
// knows nothing of widgets, the editor or the platform: pointer and key
// inputs in PAGE DISPLAY space go in, INTENTS come out (the controller turns
// them into commands), and preview() describes what to draw while a gesture
// is running. Selection geometry and hit testing are supplied from outside
// (setSelection / the hit callback), so every rule below is unit-testable.
//
// Rules (all distances are logical points; page points = logical / zoom):
//   - SELECT-LIKE behaviour applies under EVERY tool except Ink: a press on
//     a handle of the selected annotation resizes it, a press that hits an
//     annotation (tolerance kHitTolerance) SELECTS it - no creation - and a
//     drag then moves it when it is movable. A press that hits an annotation
//     under Highlight/Underline/StrikeOut/Note/shape/Stamp therefore selects
//     instead of starting the tool. Ink always draws (over annotations) and
//     ignores handles and hits.
//   - A miss under Select or a markup tool is NOT consumed (the viewport
//     does text selection and links) and clears the annotation selection.
//   - A gesture needs kDragThreshold of travel before it acts; below it a
//     shape/line/ink/move/resize press creates or changes nothing. A Note
//     press that never reached the threshold is a click and creates a note;
//     a Stamp click places the default size (kStampWidth x kStampHeight, in
//     display space so it stays 150 wide on screen on rotated pages), a
//     stamp drag sets the size.
//   - Shift constrains shapes to squares/circles and lines to multiples of
//     45 degrees (while creating, and while dragging a line endpoint).
//   - Points are clamped into the page of the press (PointerInput::pageSize).
//   - Move/Resize/line-endpoint gestures commit exactly one intent, on the
//     release; the move/resize is previewed from preview().
//   - Esc: cancels the running gesture; else clears the selection; else
//     returns to Select; else is not consumed. (The note editor sits above
//     these in the controller.)
class AnnotationInteraction {
public:
    static constexpr double kDragThreshold = 3.0;     // logical
    static constexpr double kHitTolerance = 4.0;      // logical
    static constexpr double kHandleSize = 8.0;        // logical, drawn
    static constexpr double kHandleHitRadius = 7.0;   // logical
    static constexpr double kStampWidth = 150.0;      // page points
    static constexpr double kStampHeight = 50.0;
    static constexpr double kNoteSize = 20.0;         // page points (icon box)
    static constexpr double kMinEditSize = 2.0;       // smallest resized extent, page points
    static constexpr std::size_t kMaxInkPoints = 20000; // raw capture cap per stroke

    enum class Handle : std::uint8_t {
        None,
        TopLeft,
        Top,
        TopRight,
        Right,
        BottomRight,
        Bottom,
        BottomLeft,
        Left,
        LineStart,
        LineEnd,
    };

    // What the hit callback reports about one annotation (display space).
    struct HitInfo {
        core::AnnotationId id;
        bool canMove = false;
        bool canResize = false;
        bool canEditContents = false;
        bool canDelete = true;
        bool isLine = false; // Line/Arrow: two endpoint handles instead of eight
        core::Rect bounds;
        core::Point lineStart;
        core::Point lineEnd;
    };

    struct Selected {
        std::size_t page = 0;
        HitInfo info;
    };

    using HitTest = std::function<std::optional<HitInfo>(std::size_t page, core::Point displayPoint,
                                                         double tolerancePoints)>;

    struct PointerInput {
        std::size_t page = 0;
        core::Point point;        // page display points
        core::Size pageSize;      // page display size (zero = no clamping)
        bool shift = false;
        double zoom = 1.0;        // logical points per page point
        int clickCount = 1;
    };

    enum class KeyInput : std::uint8_t { Escape, Delete, Backspace, Enter };

    struct Intent {
        enum class Kind : std::uint8_t {
            None,           // consumed, nothing to do
            PassThrough,    // not handled: the viewport gets the event
            Select,         // select `id`
            ClearSelection,
            CreateNote,     // `page`, `rect` (icon box)
            CreateInk,      // `page`, `strokes` (raw points), `zoom`
            CreateShape,    // `tool` Rectangle/Ellipse: `rect`; Line/Arrow: `a`, `b`
            CreateStamp,    // `page`, `rect`
            Move,           // `id`, `delta`
            Resize,         // `id`, `rect` (new bounds)
            LineEndpoints,  // `id`, `a`, `b`
            Delete,         // `id`
            OpenNoteEditor, // `id`
            ConvertTextSelection,
            SelectTool,     // Esc at the end of the chain: back to Select
        };
        Kind kind = Kind::None;
        // False: the event was not consumed (the viewport handles it too);
        // ClearSelection may carry false (a miss that still deselects).
        bool consumed = true;
        std::size_t page = 0;
        core::AnnotationId id;
        AnnotationTool tool = AnnotationTool::Select;
        core::Rect rect;
        core::Point delta;
        core::Point a;
        core::Point b;
        std::vector<std::vector<core::Point>> strokes;
        double zoom = 1.0;
    };

    // What to draw while a gesture runs.
    struct Preview {
        enum class Kind : std::uint8_t {
            None,
            Ink,    // `points` polyline
            Rect,   // `rect`; `tool` tells Rectangle / Ellipse / Stamp
            Line,   // `a` -> `b`; `tool` tells Line / Arrow (or the edited annotation's)
            Bounds, // `rect`: the moved/resized selection bounds
        };
        Kind kind = Kind::None;
        std::size_t page = 0;
        AnnotationTool tool = AnnotationTool::Select;
        // True when an existing annotation is being moved/resized (draw as
        // selection chrome) rather than a new one being created.
        bool editing = false;
        std::vector<core::Point> points;
        core::Rect rect;
        core::Point a;
        core::Point b;
    };

    void setHitTest(HitTest hitTest) { hitTest_ = std::move(hitTest); }

    AnnotationTool tool() const { return tool_; }
    // Changing the tool cancels a running gesture.
    void setTool(AnnotationTool tool);

    // The selected annotation as the controller resolved it (nullopt = none).
    // The machine also updates it itself on Select / ClearSelection / Delete.
    void setSelection(std::optional<Selected> selection) { selection_ = std::move(selection); }
    const std::optional<Selected>& selection() const { return selection_; }

    Intent pointerDown(const PointerInput& input);
    Intent pointerMove(const PointerInput& input);
    Intent pointerUp(const PointerInput& input);
    Intent key(KeyInput input);

    // After the viewport handled a release this machine did not consume: a
    // markup tool turns a non-empty text selection into markup.
    Intent afterUnconsumedUp(bool textSelectionNonEmpty) const;

    // True while a gesture that changes or creates something is running
    // (what Esc cancels). A press consumed only to select is not one.
    bool gestureInProgress() const;
    // True while a press this machine consumed is still held (the release
    // and moves are consumed too).
    bool gestureActive() const { return gesture_.kind != GestureKind::None; }
    std::optional<std::size_t> gesturePage() const;
    // Drops any gesture (tab switch, tool change); the release that follows
    // simply passes through.
    void cancelGesture() { gesture_ = Gesture{}; }

    Preview preview() const;

    // --- Geometry (public for tests and for the painter) -------------------
    // The 8 resize handles of `bounds` (corners win over edges); None when
    // `point` is not within kHandleHitRadius / zoom of one.
    static Handle handleAt(const core::Rect& bounds, core::Point point, double zoom);
    static Handle lineHandleAt(core::Point lineStart, core::Point lineEnd, core::Point point, double zoom);
    static core::Point handlePoint(const core::Rect& bounds, Handle handle);
    // `bounds` after dragging `handle` to `point` (normalized; may flip).
    static core::Rect resizedRect(const core::Rect& bounds, Handle handle, core::Point point);
    // Normalized drag rectangle; `square` constrains to a square, kept
    // inside `pageSize` when it is non-zero.
    static core::Rect dragRect(core::Point start, core::Point current, bool square, core::Size pageSize);
    // `end` snapped to the nearest multiple of 45 degrees around `anchor`
    // (same length) when `constrain`.
    static core::Point lineEndFor(core::Point anchor, core::Point end, bool constrain);
    // The default stamp box centered on `center`, kept inside `pageSize`.
    static core::Rect defaultStampRect(core::Point center, core::Size pageSize);

private:
    enum class GestureKind : std::uint8_t {
        None,
        Swallow,    // a consumed press with no action (select-only, cancelled)
        NoteClick,
        Ink,
        ShapeDrag,  // Rectangle / Ellipse / Line / Arrow / Stamp
        Move,
        Resize,     // one of the 8 handles
        LineHandle, // LineStart / LineEnd
    };

    struct Gesture {
        GestureKind kind = GestureKind::None;
        std::size_t page = 0;
        core::Point start;
        core::Point current;
        core::Size pageSize;
        double zoom = 1.0;
        bool shift = false;
        bool started = false; // travelled past the threshold
        Handle handle = Handle::None;
        Selected target;      // Move / Resize / LineHandle
        std::vector<core::Point> points; // Ink
    };

    core::Point clamp(core::Point point, core::Size pageSize) const;
    void track(Gesture& gesture, const PointerInput& input) const;
    Intent commit(const Gesture& gesture) const;
    Intent selectIntent(core::AnnotationId id) const;

    HitTest hitTest_;
    AnnotationTool tool_ = AnnotationTool::Select;
    std::optional<Selected> selection_;
    Gesture gesture_;
};

} // namespace rivet::app
