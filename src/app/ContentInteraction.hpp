// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationInteraction.hpp"
#include "app/ContentTool.hpp"

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "editor/ContentObjects.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace rivet::app {

// The pure interaction state machine of the content tools (ADR-0014/0015),
// the twin of AnnotationInteraction: pointer and key inputs in PAGE DISPLAY
// space go in, INTENTS come out (the controller turns them into editor
// commands), preview() describes what to draw while a gesture runs. Hit
// testing and selection geometry come from outside (hit callback /
// setSelection), so every rule is unit-testable.
//
// Rules (distances are logical points; page points = logical / zoom):
//   SelectObject
//   - A press on a handle of the selection starts a resize (images and
//     paths: eight handles) or a wrap-width drag (text blocks: ONE handle on
//     the right side of the block's baseline frame; text is never scaled).
//   - Else a press that hits an object (tolerance kHitTolerance / zoom,
//     topmost) SELECTS it - also a read-only one, so the UI can explain why
//     - and a drag then moves it when it is movable. A double-click on a
//     block that can be retyped opens the inline editor. A miss deselects;
//     the press is consumed either way (no text selection in this tool).
//   - Moves commit ONE intent on the release (after kDragThreshold of
//     travel); below it a press only selects.
//   - Corner resize keeps the aspect ratio for images unless Shift is held
//     (for paths it is the other way round: free unless Shift is held); edge
//     handles resize along one axis.
//   - Esc: cancels the running gesture; else clears the selection; else is
//     not consumed. Delete/Backspace delete the selection; Return opens the
//     inline editor; arrows nudge by 1 pt (Shift 10 pt, display space).
//   AddText
//   - A click places a text block at the point; a drag past the threshold
//     defines a box (its width becomes the wrap width).
//   Hover: pointerMove without a gesture tracks the hovered object (also
//   under AddText: nothing) for the highlight.
class ContentInteraction {
public:
    static constexpr double kDragThreshold = AnnotationInteraction::kDragThreshold;
    static constexpr double kHitTolerance = 4.0; // logical points
    static constexpr double kMinSize = 4.0;      // smallest resized extent, page points
    static constexpr double kMinWrapWidth = 8.0; // smallest wrap width, page points
    static constexpr double kNudgeStep = 1.0;    // page points
    static constexpr double kNudgeStepShift = 10.0;

    using Handle = AnnotationInteraction::Handle;

    enum class Kind : std::uint8_t { Image, Path, Text, Other };

    // What the hit callback reports about one object or block (display space).
    struct HitInfo {
        core::ObjectId id;
        bool isBlock = false;
        Kind kind = Kind::Other;
        core::Rect bounds;
        std::array<core::Point, 4> quad{};
        bool canMove = false;
        bool canDelete = false;
        bool canResize = false;   // images and paths: eight handles
        bool canEditText = false; // retype / restyle (Replaceable or FullyEditable)
        bool canWrap = false;     // text blocks: the wrap-width handle
        bool rivetBlock = false;  // a block written by Rivet (own font choice, Bring to Front)
        double wrapBase = 0.0;    // current wrap width in page points (frame width when unwrapped)
        editor::ContentCapability capability = editor::ContentCapability::ReadOnly;
        std::string reason;
    };

    struct Selected {
        std::size_t page = 0;
        HitInfo info;
    };

    using HitTest = std::function<std::optional<HitInfo>(std::size_t page, core::Point displayPoint,
                                                         double tolerancePoints)>;

    using PointerInput = AnnotationInteraction::PointerInput;

    enum class KeyInput : std::uint8_t { Escape, Delete, Backspace, Enter, Left, Right, Up, Down };

    struct Intent {
        enum class Kind : std::uint8_t {
            None,           // consumed, nothing to do
            PassThrough,    // not handled: the viewport gets the event
            Select,         // select `info` on `page`
            ClearSelection,
            Move,           // `info.id`, `delta`
            Resize,         // `info.id`, `rect` (new bounds)
            SetWrap,        // `info.id`, `width` (page points)
            Delete,         // `info.id`
            OpenEditor,     // `info` on `page` (selects it too)
            AddTextClick,   // `page`, `point`
            AddTextBox,     // `page`, `rect`
            Nudge,          // `info.id`, `delta`, `key`
        };
        Kind kind = Kind::None;
        bool consumed = true;
        std::size_t page = 0;
        HitInfo info;
        core::Point delta;
        core::Point point;
        core::Rect rect;
        double width = 0.0;
        KeyInput key = KeyInput::Escape;
    };

    // What to draw while a gesture runs.
    struct Preview {
        enum class Kind : std::uint8_t {
            None,
            Move,   // the moved selection: `rect`, `quad`
            Resize, // the resized selection: `rect`
            Wrap,   // the re-wrapped text frame: `quad`
            AddBox, // the new text box: `rect`
        };
        Kind kind = Kind::None;
        std::size_t page = 0;
        core::Rect rect;
        std::array<core::Point, 4> quad{};
    };

    void setHitTest(HitTest hitTest) { hitTest_ = std::move(hitTest); }

    ContentTool tool() const { return tool_; }
    // Changing the tool cancels a running gesture and clears hover.
    void setTool(ContentTool tool);

    // The selected object as the controller resolved it (nullopt = none).
    void setSelection(std::optional<Selected> selection) { selection_ = std::move(selection); }
    const std::optional<Selected>& selection() const { return selection_; }
    // The hovered object (SelectObject, no gesture); nullopt when none.
    const std::optional<Selected>& hover() const { return hover_; }
    void clearHover() { hover_.reset(); }

    Intent pointerDown(const PointerInput& input);
    Intent pointerMove(const PointerInput& input);
    Intent pointerUp(const PointerInput& input);
    Intent key(KeyInput input);

    // True while a press this machine consumed is still held.
    bool gestureActive() const { return gesture_.kind != GestureKind::None; }
    // True while a gesture that changes or creates something is running
    // (what Esc cancels); a press that only selected is not one.
    bool gestureInProgress() const;
    std::optional<std::size_t> gesturePage() const;
    void cancelGesture() { gesture_ = Gesture{}; }

    Preview preview() const;

    // --- Geometry (public for tests and the painter) ---------------------------
    // The wrap-width handle of a text block: the middle of its right edge.
    static core::Point wrapHandlePoint(const HitInfo& info);
    // Unit vector along the baseline (quad[0] -> quad[1]).
    static core::Point baselineDirection(const std::array<core::Point, 4>& quad);
    // `bounds` after dragging `handle` to `point` keeping the aspect ratio
    // (corner handles; edge handles resize freely).
    static core::Rect aspectResizedRect(const core::Rect& bounds, Handle handle, core::Point point);

private:
    enum class GestureKind : std::uint8_t {
        None,
        Swallow, // a consumed press with no action (select-only, miss)
        Move,
        Resize,
        Wrap,
        AddText,
    };

    struct Gesture {
        GestureKind kind = GestureKind::None;
        std::size_t page = 0;
        core::Point start;
        core::Point current;
        core::Size pageSize;
        double zoom = 1.0;
        bool shift = false;
        bool started = false;
        Handle handle = Handle::None;
        Selected target;
    };

    core::Point clamp(core::Point point, core::Size pageSize) const;
    void track(Gesture& gesture, const PointerInput& input) const;
    core::Rect resizeTarget(const Gesture& gesture) const;
    double wrapTarget(const Gesture& gesture) const;

    HitTest hitTest_;
    ContentTool tool_ = ContentTool::None;
    std::optional<Selected> selection_;
    std::optional<Selected> hover_;
    Gesture gesture_;
};

} // namespace rivet::app
