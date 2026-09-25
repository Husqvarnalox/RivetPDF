// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace rivet::ui {

// Single-line UTF-8 text field for search queries, page numbers and zoom
// percentages. Click reports focus intent through onFocusRequested (the host
// owns focus routing and calls setFocused()); while isFocused() the field
// shows a caret and consumes key events. Supports code-point-accurate caret
// movement (UTF-8), Home/End, Backspace/Delete, Shift+arrow and Shift+click
// selection, Cmd/Ctrl+A select-all, typed characters from KeyEvent::text
// (direct multi-byte input must work), Enter/Escape callbacks, and
// horizontal scrolling when the text exceeds the field width.
//
// NOT a rich text editor: single line, no undo history, no IME composition
// (typed text arrives precomposed via KeyEvent::text), no field-internal
// clipboard.
class TextField final : public Widget {
public:
    static constexpr double kHorizontalPadding = 6.0;
    static constexpr double kMinHeight = 24.0;
    static constexpr double kCornerRadius = 5.0;

    // Byte range [begin, end) within text(); empty when begin == end.
    // begin/end always lie on UTF-8 code-point boundaries.
    struct Selection {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool operator==(const Selection&) const = default;
    };

    explicit TextField(std::string placeholder = {});

    const std::string& text() const { return text_; }
    // Replaces the text, clears the selection and puts the caret at the end.
    // Does not fire onTextChanged (programmatic change, not user input).
    void setText(std::string text);

    std::size_t caret() const { return caret_; } // byte offset on a code-point boundary
    Selection selection() const;
    const std::string& placeholder() const { return placeholder_; }

    void setOnTextChanged(std::function<void(const std::string&)> onTextChanged);
    void setOnEnter(std::function<void()> onEnter);
    void setOnEscape(std::function<void()> onEscape);
    void setOnFocusRequested(std::function<void()> onFocusRequested);

    // Horizontal text scroll offset in points (diagnostic/tests; >= 0).
    // Adjusted during paint so the caret stays visible.
    double horizontalScrollOffset() const { return horizontalScroll_; }

    core::Size preferredSize(const PaintContext& context) const override;

    bool wantsFocus() const override;
    bool onMouse(const PointerEvent& event) override;
    bool onKey(const KeyEvent& event) override;
    void paintSelf(PaintContext& context) const override;

private:
    // UTF-8 boundary helpers (static):
    // previousBoundary(s, i): largest j < i with s[j] not a continuation byte.
    // nextBoundary(s, i): smallest j > i with s[j] not a continuation byte
    // (or s.size()).
    static std::size_t previousBoundary(const std::string& s, std::size_t byteIndex);
    static std::size_t nextBoundary(const std::string& s, std::size_t byteIndex);

    // Cached width of the text prefix [0, byteIndex) in points. byteIndex
    // must be a code-point boundary; valid only after a paint rebuilt the
    // cache.
    double prefixWidthUpTo(std::size_t byteIndex) const;
    // Rebuilds the prefix-width cache (one measureText per code point; the
    // accumulated total may differ slightly from measuring the whole string —
    // acceptable for caret work and keeps clicks/selection deterministic).
    void rebuildPrefixWidths(const PaintContext& context) const;
    // Paint-time scroll adjustment so the caret sits inside the padded text
    // area; clamped to >= 0.
    void updateHorizontalScroll(const core::Rect& bounds) const;
    // Boundary nearest the click x by cached prefix widths; before the first
    // paint (no cache) clicks right of the text start map to the end,
    // clicks at/before it to 0.
    std::size_t boundaryForClickX(double localX) const;
    // Erases the selected byte range and collapses the caret to its start.
    void eraseSelection();
    // Common tail of every user edit: drops the width cache and fires
    // onTextChanged.
    void notifyTextChanged();

    std::string text_;
    std::string placeholder_;
    std::size_t caret_ = 0;  // focus end of the selection (byte offset)
    std::size_t anchor_ = 0; // fixed end of the selection (byte offset)
    mutable double horizontalScroll_ = 0.0;
    // prefixWidths_[byte] = width of text_[0, byte) in points; entries at
    // non-boundary byte offsets are unused. Rebuilt during paint, dropped on
    // any text change.
    mutable std::vector<double> prefixWidths_;
    std::function<void(const std::string&)> onTextChanged_;
    std::function<void()> onEnter_;
    std::function<void()> onEscape_;
    std::function<void()> onFocusRequested_;
    bool pressed_ = false; // press started in this field, button still held
};

} // namespace rivet::ui
