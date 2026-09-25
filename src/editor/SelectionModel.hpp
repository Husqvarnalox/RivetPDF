// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"

#include <cstdint>
#include <functional>
#include <utility>

namespace rivet::editor {

// Document-semantic identity of a text caret: one page (PageId, NOT a raw
// backend index) plus the page-local character index into that page's
// PdfTextPage. Stable across scrolling and zooming because it never touches
// view geometry.
struct TextPosition {
    core::PageId page;
    std::uint32_t characterIndex = 0;

    bool operator==(const TextPosition&) const = default;
};

// Mouse text selection: anchor (fixed end) + focus (moving end). Direction
// is preserved so a backwards drag behaves naturally; consumers normalize
// (with the session's page ordering) when they need an ordered range.
struct TextSelection {
    TextPosition anchor;
    TextPosition focus;

    bool empty() const { return anchor == focus; }
    bool operator==(const TextSelection&) const = default;
};

// Holds the active selection of one view (one tab). Pure state: geometry and
// page ordering live in the text service / session. Main-thread only.
class SelectionModel {
public:
    // Starts (or restarts) a selection with the given anchor and focus.
    void start(TextPosition position);
    // Moves the focus end (drag); activates the selection when inactive.
    // Passing the anchor position keeps the selection empty-but-active.
    void setFocus(TextPosition position);
    // Shift+click-style extension when no selection is active yet: anchors
    // at the old focus (or position when cleared) and focuses at position.
    void extendTo(TextPosition position);
    void clear();

    bool empty() const { return !active_; }
    // The selection may be empty (anchor == focus) while still active.
    TextPosition anchor() const { return selection_.anchor; }
    TextPosition focus() const { return selection_.focus; }
    TextSelection selection() const { return selection_; }

    // Fired after every state change (including clear).
    void setCallback(std::function<void()> onChanged);

private:
    bool active_ = false;
    TextSelection selection_{};
    std::function<void()> onChanged_;
};

} // namespace rivet::editor
