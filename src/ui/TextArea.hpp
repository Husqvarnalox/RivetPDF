// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/PaintContext.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rivet::ui {

// Multi-line plain UTF-8 text editor (note contents). Modelled on TextField:
// click reports focus intent through onFocusRequested (the host owns focus
// routing and calls setFocused()); while isFocused() it shows a caret and
// consumes key events.
//
// Hard line breaks ('\n') plus word wrapping to the widget width (break after
// spaces, falling back to code-point breaks for long words). Vertical
// scrolling keeps the caret visible; the mouse wheel scrolls too. Keys:
// Left/Right (code points), Up/Down (by x position across wrapped lines),
// Home/End (visual line; Cmd+Left/Right the same), Cmd+Up/Down (document
// start/end), all extendable with Shift; Backspace/Delete, Cmd+A, typed text
// from KeyEvent::text (multi-byte input must work), Enter inserts '\n',
// Cmd+Enter fires onCommit, Escape fires onEscape. Mouse: click, drag and
// shift-click selection.
//
// Layout model: glyph widths are measured per code point at paint time and
// cached for the lifetime of the widget (the font is fixed), so
// the wrapped layout is a pure function of (text, width, cached widths) and
// key handling never needs a PaintContext. A code point typed since the last
// paint uses an estimated width until the next paint measures it. Before the
// first paint (or with a zero-width frame) the text simply does not wrap.
//
// The contents are untrusted user data: nothing here ever logs them.
// NOT a rich editor: no undo history, no IME composition, no clipboard.
class TextArea final : public Widget {
public:
    static constexpr double kPadding = 6.0;
    static constexpr double kLineHeight = 18.0;
    static constexpr double kCornerRadius = 5.0;
    static constexpr double kPreferredWidth = 220.0;
    static constexpr double kPreferredHeight = 96.0;

    // Byte range [begin, end) within text(), on code-point boundaries.
    struct Selection {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool operator==(const Selection&) const = default;
    };

    explicit TextArea(std::string placeholder = {});

    const std::string& text() const { return text_; }
    // Replaces the text (truncated to maxBytes at a code-point boundary),
    // clears the selection, puts the caret at the end and scrolls it into
    // view. Does not fire onTextChanged (programmatic change).
    void setText(std::string text);

    std::size_t caret() const { return caret_; } // byte offset on a code-point boundary
    Selection selection() const;
    const std::string& placeholder() const { return placeholder_; }

    // Upper bound on text().size() in bytes; inserting beyond it is truncated
    // at a code-point boundary. Default: unlimited. Existing longer text is
    // truncated silently.
    void setMaxBytes(std::size_t maxBytes);
    std::size_t maxBytes() const { return maxBytes_; }

    void setOnTextChanged(std::function<void(const std::string&)> onTextChanged);
    void setOnCommit(std::function<void()> onCommit);
    void setOnEscape(std::function<void()> onEscape);
    void setOnFocusRequested(std::function<void()> onFocusRequested);

    // Layout introspection (tests/hosts); reflects the current frame width.
    std::size_t lineCount() const;
    // Byte range of visual line `index` (excludes the '\n' of a hard break);
    // an empty range when out of range.
    Selection lineRange(std::size_t index) const;
    // Visual line the caret is displayed on.
    std::size_t caretLine() const;
    // Vertical scroll offset in points (>= 0).
    double scrollOffset() const { return scrollY_; }

    core::Size preferredSize(const PaintContext& context) const override;

    bool wantsFocus() const override;
    bool onMouse(const PointerEvent& event) override;
    bool onKey(const KeyEvent& event) override;
    void paintSelf(PaintContext& context) const override;
    void layout() override;

private:
    struct VisualLine {
        std::size_t begin = 0;
        std::size_t end = 0; // excludes the '\n' of a hard break
        bool soft = false;   // ends because of wrapping (next line starts at end)
    };

    // Wrapped layout, rebuilt lazily when (text, wrap width, glyph widths)
    // change. Always holds at least one line.
    const std::vector<VisualLine>& lines() const;
    double wrapWidth() const; // +infinity when the frame has no width yet
    double glyphWidth(char32_t codePoint) const;
    // Measures every code point of text_ not cached yet (paint time).
    void measureNewGlyphs(const PaintContext& context) const;
    // Width of text_[from, to) (both on boundaries) by cached glyph widths.
    double spanWidth(std::size_t from, std::size_t to) const;

    std::size_t lineIndexForOffset(std::size_t offset, bool upstream) const;
    std::size_t boundaryOnLine(const VisualLine& line, double x) const;
    std::size_t offsetForPoint(core::Point local, bool* upstream) const;
    double contentHeight() const;
    double viewHeight() const;
    void clampScroll() const;
    void ensureCaretVisible();

    // Moves the caret (extending the selection when `extend`), then scrolls
    // it into view.
    void moveCaret(std::size_t offset, bool upstream, bool extend);
    void moveVertical(int direction, bool extend);
    void eraseSelection();
    // Inserts `insertion` replacing the selection, honouring maxBytes_.
    // Returns false (nothing changed) when nothing could be inserted.
    bool insertText(std::string insertion);
    void notifyTextChanged();
    void textChanged(); // bumps the revision (layout invalid)

    std::string text_;
    std::string placeholder_;
    std::size_t caret_ = 0;
    std::size_t anchor_ = 0;
    bool caretUpstream_ = false; // at a soft-wrap boundary: show on the earlier line
    std::optional<double> preferredX_; // sticky column for consecutive Up/Down
    std::size_t maxBytes_ = std::numeric_limits<std::size_t>::max();
    mutable double scrollY_ = 0.0;

    std::uint64_t textRevision_ = 0;
    mutable std::unordered_map<char32_t, double> glyphWidths_;
    mutable std::uint64_t glyphRevision_ = 0;
    mutable std::vector<VisualLine> lines_;
    mutable std::uint64_t layoutTextRevision_ = std::numeric_limits<std::uint64_t>::max();
    mutable std::uint64_t layoutGlyphRevision_ = 0;
    mutable double layoutWrapWidth_ = 0.0;

    std::function<void(const std::string&)> onTextChanged_;
    std::function<void()> onCommit_;
    std::function<void()> onEscape_;
    std::function<void()> onFocusRequested_;
    bool pressed_ = false;
};

} // namespace rivet::ui
