// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "ui/PaintContext.hpp"
#include "ui/ScrollBar.hpp"
#include "ui/TextBuffer.hpp"
#include "ui/Widget.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::ui {

// What produced an edit; the host uses it (with mergeWithPrevious) to build
// undo steps.
enum class SourceEditKind : std::uint8_t {
    Typing,
    Backspace,
    DeleteForward,
    Newline,
    Paste,
    Cut,
    Indent,
    DeleteWord,
    DeleteLine,
    Other,
};

// One mutation: replace [offset, offset+removeLength) by `insert`. Offsets are
// code-point-aligned byte offsets into the text as it is BEFORE the edit;
// `insert` is already normalized (LF only, valid UTF-8). The views are only
// valid during the call.
struct SourceEdit {
    std::size_t offset = 0;
    std::size_t removeLength = 0;
    std::string_view removed;
    std::string_view insert;
    SourceEditKind kind = SourceEditKind::Other;
    // The widget's coalescing verdict: this edit continues the previous one
    // (consecutive typing / backspace / forward-delete at the same spot, no
    // caret jump in between, no word boundary). The host may fold it into the
    // previous undo step if it can, or ignore the hint.
    bool mergeWithPrevious = false;
};

// Receives every user edit BEFORE the widget changes its own buffer, so one
// authority (the app's command stack) decides. Returning false refuses the
// edit (e.g. editing locked during a save): the widget then changes nothing.
// The sink must not call back into the widget from applyEdit().
class ISourceEditSink {
public:
    virtual bool applyEdit(const SourceEdit& edit) = 0;

protected:
    ~ISourceEditSink() = default;
};

// Text clipboard as the widget needs it (adapted from platform::IClipboard by
// the app, so the ui layer stays platform-free).
class ITextClipboard {
public:
    virtual std::string text() const = 0;
    virtual bool setText(const std::string& text) = 0;

protected:
    ~ITextClipboard() = default;
};

// Monospace multi-line source editor (Markdown source pane).
//
// Layout: optional line-number gutter, text area, vertical and horizontal
// ScrollBars (children, always reserved). NO soft wrap: long lines scroll
// horizontally (a wrap mode would need a visual-line index; deferred). The
// columns model is one cell per code point (tab = to the next multiple of 4,
// East Asian wide / emoji = 2 cells, combining marks = 0); the cell width is
// the monospace font's measured advance. Wide/fallback glyphs that do not
// match that advance can drift horizontally within a line (caret/selection
// are cell-based).
//
// Text lives in an owned TextBuffer (valid UTF-8, LF only). User edits are
// reported to the ISourceEditSink first and applied to the buffer only if the
// sink accepts; the host keeps its own authoritative copy and tells the widget
// about changes it makes itself (undo/redo, reload) with applyExternalEdit().
//
// Keys (when focused): arrows (+Shift extend; Opt = word, Cmd = line start/end
// for Left/Right and document start/end for Up/Down), Home/End, Page Up/Down,
// Backspace/Delete (+Opt word, +Cmd to line start/end), Enter (keeps the
// previous line's indentation), Tab (4 spaces; with a selection indents every
// touched line), Shift+Tab (outdent), Cmd+A/C/X/V. Cmd+Z, Cmd+Shift+Z and
// every other Cmd/Ctrl shortcut are NOT consumed: the shell routes undo/redo
// to the tab's command stack. Mouse: click, drag, shift-click, double-click
// (word), triple-click (line), wheel. Not supported: IME composition,
// soft wrap, bidi, drag-and-drop of text.
//
// Contents are untrusted: nothing here logs them. Main thread only.
class SourceEditor final : public Widget {
public:
    static constexpr double kPadding = 6.0;
    static constexpr std::size_t kIndentWidth = 4;
    static constexpr std::size_t kTabStop = 4;
    // Edits that would grow the text beyond this are refused (mirrors the
    // 64 MiB file cap of the app).
    static constexpr std::size_t kMaxBytes = 64ull * 1024 * 1024;

    struct Selection {
        std::size_t begin = 0;
        std::size_t end = 0;
        bool operator==(const Selection&) const = default;
    };

    SourceEditor();

    void setSink(ISourceEditSink* sink) { sink_ = sink; }
    void setClipboard(ITextClipboard* clipboard) { clipboard_ = clipboard; }
    void setOnFocusRequested(std::function<void()> callback) { onFocusRequested_ = std::move(callback); }
    // Fired when the scroll position changed (wheel, scrollbar, caret
    // tracking, scrollToOffset) for sync-scroll.
    void setOnScrolled(std::function<void()> callback) { onScrolled_ = std::move(callback); }
    // Fired after the widget's own text changed (an accepted user edit, an
    // applyExternalEdit() or setText()), i.e. when buffer() is up to date.
    // Hosts refresh text-derived state (search matches) from it.
    void setOnTextChanged(std::function<void()> callback) { onTextChanged_ = std::move(callback); }
    // Test seam for multi-click timing.
    void setClock(std::function<std::chrono::steady_clock::time_point()> clock) { clock_ = std::move(clock); }

    void setShowLineNumbers(bool show);
    bool showLineNumbers() const { return showLineNumbers_; }
    void setFontSize(double size);

    // --- Content -------------------------------------------------------------
    const TextBuffer& buffer() const { return buffer_; }
    // Replaces everything without notifying the sink (a load); caret to 0.
    void setText(std::string_view text);
    // The host changed its text itself (undo/redo): replace the range in the
    // widget's buffer without notifying the sink. Caret/anchor are remapped.
    // False when the range is invalid.
    bool applyExternalEdit(std::size_t offset, std::size_t removeLength, std::string_view insert);

    // --- Search highlights ------------------------------------------------------
    // Ranges [begin, end) in byte offsets of the current text, sorted and
    // non-overlapping (e.g. find matches); `current` indexes the one drawn
    // emphasized. They describe the text as it is NOW: any later edit of the
    // buffer hides them until the host sets new ones.
    struct Highlight {
        std::size_t begin = 0;
        std::size_t end = 0;
    };
    void setHighlights(std::vector<Highlight> ranges, std::optional<std::size_t> current = std::nullopt);
    void clearHighlights() { setHighlights({}, std::nullopt); }
    std::size_t highlightCount() const { return highlights_.size(); }

    // --- Caret / selection ----------------------------------------------------
    std::size_t caretOffset() const { return caret_; }
    Selection selection() const;
    void setSelection(std::size_t anchor, std::size_t caret); // aligned + clamped
    void setCaretOffset(std::size_t offset) { setSelection(offset, offset); }
    // Column of the caret in cells (tabs expanded), and its line.
    std::size_t caretColumn() const;
    std::size_t caretLine() const { return buffer_.lineOfOffset(caret_); }

    // --- Scrolling (sync-scroll hooks) -----------------------------------------
    // Byte offset of the first character of the topmost (partially) visible line.
    std::size_t topVisibleOffset() const;
    // Makes the line containing `offset` the top visible line (clamped).
    void scrollToOffset(std::size_t offset);
    // Scrolls minimally so `offset` is visible.
    void revealOffset(std::size_t offset);
    double scrollY() const { return scrollY_; }
    double scrollX() const { return scrollX_; }
    std::size_t firstVisibleLine() const;
    std::size_t lastVisibleLine() const; // inclusive

    // Layout metrics (tests / hosts).
    double lineHeight() const { return lineHeight_; }
    double cellWidth() const { return cellWidth_; }
    core::Rect textRect() const;
    double gutterWidth() const;

    bool wantsFocus() const override { return true; }
    bool onMouse(const PointerEvent& event) override;
    bool onKey(const KeyEvent& event) override;
    void paintSelf(PaintContext& context) const override;
    void layout() override;

private:
    struct EditTrack {
        bool valid = false;
        SourceEditKind kind = SourceEditKind::Other;
        std::size_t end = 0; // caret right after the edit
    };

    // Columns and hit testing.
    std::size_t columnBetween(std::size_t lineStartOffset, std::size_t offset) const;
    std::size_t offsetAtColumn(std::size_t line, double column) const;
    std::size_t offsetForPoint(core::Point local) const;
    double contentHeight() const;
    double contentWidth() const;
    double viewHeight() const;
    double viewWidth() const;
    void clampScroll();
    void setScroll(double x, double y);
    void syncScrollbars() const;
    void ensureCaretVisible();
    void measureCell(PaintContext& context) const;

    // Movement.
    void moveCaret(std::size_t offset, bool extend);
    void moveVertical(long delta, bool extend, bool toEdges);
    std::size_t wordLeft(std::size_t pos) const;
    std::size_t wordRight(std::size_t pos) const;
    Selection wordRangeAt(std::size_t offset) const;
    Selection lineRangeAt(std::size_t offset) const; // includes the trailing '\n'

    // Editing.
    bool commitEdit(std::size_t offset, std::size_t removeLength, std::string insert,
                    SourceEditKind kind, std::size_t caretAfter);
    void typeText(std::string text);
    void insertNewline();
    void eraseBackward(bool word, bool line);
    void eraseForward(bool word, bool line);
    void indentSelection(bool outdent);
    void copySelection() const;
    void cutSelection();
    void paste();
    void invalidateTrack() { track_.valid = false; }

    TextBuffer buffer_;
    std::vector<Highlight> highlights_;
    std::optional<std::size_t> currentHighlight_;
    std::uint64_t highlightsRevision_ = 0; // buffer revision the highlights describe
    std::size_t caret_ = 0;
    std::size_t anchor_ = 0;
    std::optional<double> preferredColumn_; // sticky cells for Up/Down
    EditTrack track_;

    ISourceEditSink* sink_ = nullptr;
    ITextClipboard* clipboard_ = nullptr;
    std::function<void()> onFocusRequested_;
    std::function<void()> onScrolled_;
    std::function<void()> onTextChanged_;
    std::function<std::chrono::steady_clock::time_point()> clock_;

    ScrollBar* vScroll_ = nullptr;
    ScrollBar* hScroll_ = nullptr;
    double scrollX_ = 0.0;
    double scrollY_ = 0.0;
    bool showLineNumbers_ = true;
    double fontSize_ = 13.0;
    double lineHeight_ = 20.0;
    mutable double cellWidth_ = 7.8;
    mutable bool cellMeasured_ = false;
    mutable std::uint64_t colsRevision_ = static_cast<std::uint64_t>(-1);
    mutable std::size_t longestColumns_ = 0;
    std::size_t caretColumns_ = 0; // caret's column + 1 (keeps the caret inside the scroll range)

    // Mouse state.
    enum class DragMode : std::uint8_t { None, Char, Word, Line };
    DragMode drag_ = DragMode::None;
    Selection dragAnchor_;
    std::chrono::steady_clock::time_point lastClickTime_{};
    core::Point lastClickPoint_;
    int clickCount_ = 0;
};

} // namespace rivet::ui
