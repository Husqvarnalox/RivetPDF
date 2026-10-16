// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/UiTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

namespace rivet::app {

// The Redact tool's input as a pure state machine (ADR-0026), the twin of
// ContentInteraction: no widgets, no document. All points are DISPLAY space
// of one page (points, top-left origin, y-down); `page` is a layout index.
//
//   - a primary drag on a page draws a rectangle (clamped to that page) and
//     yields AddMark on release when both sides reach kMinDragExtent;
//   - a click (no drag) on an existing mark selects it (SelectMark), a click
//     elsewhere clears the selection;
//   - Delete / Backspace with a selected mark yields RemoveMark; Escape
//     cancels a running drag, else clears the selection.
//
// Main thread only.
class RedactionInteraction {
public:
    // Display points both sides of a dragged mark must reach.
    static constexpr double kMinDragExtent = 2.0;

    struct MarkRef {
        std::size_t page = 0;
        std::size_t index = 0; // position in the page's mark list
        bool operator==(const MarkRef&) const = default;
    };

    struct Intent {
        enum class Kind : std::uint8_t { None, AddMark, SelectMark, ClearSelection, RemoveMark };
        Kind kind = Kind::None;
        std::size_t page = 0;
        core::Rect rect;       // AddMark: display rect of `page`
        std::size_t index = 0; // SelectMark / RemoveMark
        bool consumed = true;  // false = let the event through
    };

    // The mark at a display point of a page (topmost first), if any.
    using MarkHitTest = std::function<std::optional<std::size_t>(std::size_t page, core::Point display)>;
    void setMarkHitTest(MarkHitTest hitTest) { hitTest_ = std::move(hitTest); }

    Intent press(std::size_t page, core::Point display, core::Size pageSize);
    void drag(core::Point display);
    Intent release(core::Point display);
    Intent key(ui::Key key);
    void cancel();

    bool dragging() const { return drag_.has_value(); }
    // The rectangle being dragged (page, display rect).
    std::optional<std::pair<std::size_t, core::Rect>> preview() const;

    const std::optional<MarkRef>& selection() const { return selection_; }
    void setSelection(std::optional<MarkRef> selection) { selection_ = selection; }

private:
    struct Drag {
        std::size_t page = 0;
        core::Point start;
        core::Point current;
        core::Size pageSize;
    };

    core::Point clamp(core::Point point, core::Size pageSize) const;

    MarkHitTest hitTest_;
    std::optional<Drag> drag_;
    std::optional<MarkRef> selection_;
};

} // namespace rivet::app
