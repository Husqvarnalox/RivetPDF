// SPDX-License-Identifier: MPL-2.0
#include "ui/SourceEditor.hpp"

#include "ui/Utf8.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rivet::ui {

namespace {

constexpr Color kBackground = Color::white();
constexpr Color kGutterBackground = Color::rgba(0.96, 0.96, 0.96, 1.0);
constexpr Color kGutterText = Color::rgba(0.55, 0.55, 0.55, 1.0);
constexpr Color kGutterSeparator = Color::rgba(0.0, 0.0, 0.0, 0.10);
constexpr Color kTextColor = Color::rgba(0.10, 0.10, 0.10, 1.0);
constexpr Color kSelectionFocused = Color::rgba(0.0, 0.47, 1.0, 0.25);
constexpr Color kSelectionUnfocused = Color::rgba(0.5, 0.5, 0.5, 0.25);
constexpr Color kCaretColor = Color::black();
constexpr double kCaretWidth = 1.5;
constexpr auto kMultiClickWindow = std::chrono::milliseconds(500);
constexpr double kMultiClickSlop = 4.0;

enum class CharClass : std::uint8_t { Space, Word, Punct };

CharClass classOf(char32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == 0xA0 || cp == 0x3000) return CharClass::Space;
    if (cp < 0x80) {
        const bool word = (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                          cp == '_';
        return word ? CharClass::Word : CharClass::Punct;
    }
    return CharClass::Word; // letters of every script, emoji, ...
}

// Cells a code point occupies (tab handled by the caller).
std::size_t cellsOf(char32_t cp) {
    if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
        (cp >= 0x20D0 && cp <= 0x20FF)) {
        return 0;
    }
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) || (cp >= 0x1F900 && cp <= 0x1F9FF) ||
        (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

std::size_t advance(std::size_t column, char32_t cp) {
    if (cp == '\t') return column + (SourceEditor::kTabStop - column % SourceEditor::kTabStop);
    return column + cellsOf(cp);
}

bool isBlank(char c) { return c == ' ' || c == '\t'; }

// The part of `line` that overlaps columns [firstCol, lastCol), tabs expanded
// to spaces, control characters shown as U+FFFD. *startCol = column of the
// first emitted character.
std::string visibleSlice(std::string_view line, std::size_t firstCol, std::size_t lastCol,
                         std::size_t* startCol) {
    std::string out;
    std::size_t column = 0;
    bool started = false;
    *startCol = 0;
    for (std::size_t i = 0; i < line.size() && column < lastCol;) {
        const char32_t cp = utf8::decodeAt(line, i);
        const std::size_t next = utf8::nextBoundary(line, i);
        const std::size_t after = advance(column, cp);
        if (after > firstCol || (started && after == column)) {
            if (!started) {
                started = true;
                *startCol = column;
            }
            if (cp == '\t') {
                out.append(after - column, ' ');
            } else if (cp < 0x20 || cp == 0x7F) {
                out.append("\xEF\xBF\xBD");
            } else {
                out.append(line.substr(i, next - i));
            }
        }
        column = after;
        i = next;
    }
    return out;
}

} // namespace

SourceEditor::SourceEditor() {
    auto vertical = std::make_unique<ScrollBar>(ScrollOrientation::Vertical);
    auto horizontal = std::make_unique<ScrollBar>(ScrollOrientation::Horizontal);
    vScroll_ = vertical.get();
    hScroll_ = horizontal.get();
    vScroll_->setOnScroll([this](double value) { setScroll(scrollX_, value); });
    hScroll_->setOnScroll([this](double value) { setScroll(value, scrollY_); });
    addChild(std::move(vertical));
    addChild(std::move(horizontal));
    lineHeight_ = std::round(fontSize_ * 1.55);
    cellWidth_ = fontSize_ * 0.6;
}

// --- configuration ------------------------------------------------------------

void SourceEditor::setShowLineNumbers(bool show) {
    if (showLineNumbers_ == show) return;
    showLineNumbers_ = show;
    clampScroll();
    invalidate();
}

void SourceEditor::setFontSize(double size) {
    if (!(size >= 6.0 && size <= 72.0) || size == fontSize_) return;
    fontSize_ = size;
    lineHeight_ = std::round(size * 1.55);
    cellWidth_ = size * 0.6;
    cellMeasured_ = false; // re-measured at the next paint
    clampScroll();
    invalidate();
}

// --- content --------------------------------------------------------------------

void SourceEditor::setText(std::string_view text) {
    buffer_.assign(text);
    caret_ = anchor_ = 0;
    preferredColumn_.reset();
    invalidateTrack();
    scrollX_ = scrollY_ = 0.0;
    clampScroll();
    syncScrollbars();
    invalidate();
    if (onTextChanged_) onTextChanged_();
}

bool SourceEditor::applyExternalEdit(std::size_t offset, std::size_t removeLength, std::string_view insert) {
    std::size_t inserted = 0;
    if (!buffer_.replace(offset, removeLength, insert, &inserted)) return false;
    const auto remap = [&](std::size_t p) {
        if (p <= offset) return p;
        if (p >= offset + removeLength) return p - removeLength + inserted;
        return offset + inserted;
    };
    caret_ = std::min(remap(caret_), buffer_.size());
    anchor_ = std::min(remap(anchor_), buffer_.size());
    preferredColumn_.reset();
    invalidateTrack();
    clampScroll();
    syncScrollbars();
    invalidate();
    if (onTextChanged_) onTextChanged_();
    return true;
}

SourceEditor::Selection SourceEditor::selection() const {
    return Selection{std::min(caret_, anchor_), std::max(caret_, anchor_)};
}

void SourceEditor::setSelection(std::size_t anchor, std::size_t caret) {
    const std::string_view text = buffer_.text();
    anchor_ = utf8::floorBoundary(text, std::min(anchor, text.size()));
    caret_ = utf8::floorBoundary(text, std::min(caret, text.size()));
    preferredColumn_.reset();
    invalidateTrack();
    ensureCaretVisible();
    invalidate();
}

std::size_t SourceEditor::caretColumn() const {
    return columnBetween(buffer_.lineStart(buffer_.lineOfOffset(caret_)), caret_);
}

// --- columns / geometry ---------------------------------------------------------

std::size_t SourceEditor::columnBetween(std::size_t lineStartOffset, std::size_t offset) const {
    const std::string_view text = buffer_.text();
    std::size_t column = 0;
    for (std::size_t i = lineStartOffset; i < offset && i < text.size();) {
        column = advance(column, utf8::decodeAt(text, i));
        i = utf8::nextBoundary(text, i);
    }
    return column;
}

std::size_t SourceEditor::offsetAtColumn(std::size_t line, double target) const {
    const std::string_view text = buffer_.text();
    const std::size_t end = buffer_.lineEnd(line);
    std::size_t column = 0;
    std::size_t i = buffer_.lineStart(line);
    while (i < end) {
        const std::size_t after = advance(column, utf8::decodeAt(text, i));
        const double width = static_cast<double>(after - column);
        if (target < static_cast<double>(column) + width / 2.0) return i;
        column = after;
        i = utf8::nextBoundary(text, i);
    }
    return end;
}

double SourceEditor::gutterWidth() const {
    if (!showLineNumbers_) return 0.0;
    std::size_t digits = 2;
    for (std::size_t n = buffer_.lineCount(); n >= 100; n /= 10) ++digits;
    return static_cast<double>(digits) * cellWidth_ + 16.0;
}

core::Rect SourceEditor::textRect() const {
    const double gutter = gutterWidth();
    const double width = std::max(0.0, frame().size.width - gutter - ScrollBar::kThickness);
    const double height = std::max(0.0, frame().size.height - ScrollBar::kThickness);
    return core::Rect{gutter, 0.0, width, height};
}

double SourceEditor::viewHeight() const { return textRect().size.height; }
double SourceEditor::viewWidth() const { return textRect().size.width; }
double SourceEditor::contentHeight() const { return static_cast<double>(buffer_.lineCount()) * lineHeight_; }

double SourceEditor::contentWidth() const {
    if (colsRevision_ != buffer_.revision()) {
        const std::size_t line = buffer_.longestLine();
        longestColumns_ = columnBetween(buffer_.lineStart(line), buffer_.lineEnd(line));
        colsRevision_ = buffer_.revision();
    }
    return 2.0 * kPadding + static_cast<double>(std::max(longestColumns_, caretColumns_) + 1) * cellWidth_;
}

std::size_t SourceEditor::firstVisibleLine() const {
    const double line = std::max(0.0, scrollY_) / lineHeight_;
    return std::min(buffer_.lineCount() - 1, static_cast<std::size_t>(line));
}

std::size_t SourceEditor::lastVisibleLine() const {
    const double bottom = std::max(scrollY_, scrollY_ + viewHeight() - 0.001);
    return std::max(firstVisibleLine(),
                    std::min(buffer_.lineCount() - 1, static_cast<std::size_t>(bottom / lineHeight_)));
}

std::size_t SourceEditor::topVisibleOffset() const { return buffer_.lineStart(firstVisibleLine()); }

void SourceEditor::scrollToOffset(std::size_t offset) {
    setScroll(scrollX_, static_cast<double>(buffer_.lineOfOffset(offset)) * lineHeight_);
}

void SourceEditor::revealOffset(std::size_t offset) {
    const std::size_t line = buffer_.lineOfOffset(offset);
    const double top = static_cast<double>(line) * lineHeight_;
    const double height = viewHeight();
    double y = scrollY_;
    if (top < y) {
        y = top;
    } else if (height >= lineHeight_ && top + lineHeight_ > y + height) {
        y = top + lineHeight_ - height;
    }
    double x = scrollX_;
    const double width = viewWidth();
    const double caretX = kPadding + static_cast<double>(columnBetween(buffer_.lineStart(line), offset)) * cellWidth_;
    if (caretX < x + kPadding) {
        x = std::max(0.0, caretX - kPadding);
    } else if (width > 0.0 && caretX + cellWidth_ + kPadding > x + width) {
        x = caretX + cellWidth_ + kPadding - width;
    }
    // The caret's own column may be beyond the cached longest-line estimate.
    const std::size_t column = columnBetween(buffer_.lineStart(line), offset);
    caretColumns_ = column + 1;
    setScroll(x, y);
}

void SourceEditor::ensureCaretVisible() { revealOffset(caret_); }

void SourceEditor::clampScroll() { setScroll(scrollX_, scrollY_); }

void SourceEditor::setScroll(double x, double y) {
    const double maxY = std::max(0.0, contentHeight() - viewHeight());
    const double maxX = std::max(0.0, contentWidth() - viewWidth());
    x = std::clamp(std::isfinite(x) ? x : 0.0, 0.0, maxX);
    y = std::clamp(std::isfinite(y) ? y : 0.0, 0.0, maxY);
    const bool changed = x != scrollX_ || y != scrollY_;
    scrollX_ = x;
    scrollY_ = y;
    syncScrollbars();
    if (changed) {
        invalidate();
        if (onScrolled_) onScrolled_();
    }
}

void SourceEditor::syncScrollbars() const {
    vScroll_->setExtents(viewHeight(), contentHeight());
    vScroll_->setOffset(scrollY_);
    hScroll_->setExtents(viewWidth(), contentWidth());
    hScroll_->setOffset(scrollX_);
}

void SourceEditor::layout() {
    const double w = frame().size.width;
    const double h = frame().size.height;
    const double t = ScrollBar::kThickness;
    vScroll_->setFrame(core::Rect{std::max(0.0, w - t), 0.0, t, std::max(0.0, h - t)});
    hScroll_->setFrame(core::Rect{0.0, std::max(0.0, h - t), std::max(0.0, w - t), t});
    clampScroll();
}

void SourceEditor::measureCell(PaintContext& context) const {
    if (cellMeasured_) return;
    cellMeasured_ = true;
    Font font;
    font.size = fontSize_;
    font.monospace = true;
    const double measured = context.measureText("0000000000", font).width / 10.0;
    if (measured > 1.0 && std::isfinite(measured)) cellWidth_ = measured;
    syncScrollbars();
}

std::size_t SourceEditor::offsetForPoint(core::Point local) const {
    const core::Rect area = textRect();
    const double row = std::floor((local.y - area.origin.y + scrollY_) / lineHeight_);
    const std::size_t line =
        row <= 0.0 ? 0 : std::min(buffer_.lineCount() - 1, static_cast<std::size_t>(row));
    const double column = (local.x - area.origin.x - kPadding + scrollX_) / cellWidth_;
    return offsetAtColumn(line, column);
}

// --- words / lines ----------------------------------------------------------------

std::size_t SourceEditor::wordLeft(std::size_t pos) const {
    const std::string_view text = buffer_.text();
    if (pos == 0) return 0;
    if (text[pos - 1] == '\n') return pos - 1;
    const auto classBefore = [&](std::size_t p) { return classOf(utf8::decodeAt(text, utf8::previousBoundary(text, p))); };
    while (pos > 0 && text[pos - 1] != '\n' && classBefore(pos) != CharClass::Word) {
        pos = utf8::previousBoundary(text, pos);
    }
    while (pos > 0 && classBefore(pos) == CharClass::Word) pos = utf8::previousBoundary(text, pos);
    return pos;
}

std::size_t SourceEditor::wordRight(std::size_t pos) const {
    const std::string_view text = buffer_.text();
    const std::size_t n = text.size();
    if (pos >= n) return n;
    if (text[pos] == '\n') return pos + 1;
    while (pos < n && text[pos] != '\n' && classOf(utf8::decodeAt(text, pos)) != CharClass::Word) {
        pos = utf8::nextBoundary(text, pos);
    }
    while (pos < n && classOf(utf8::decodeAt(text, pos)) == CharClass::Word) pos = utf8::nextBoundary(text, pos);
    return pos;
}

SourceEditor::Selection SourceEditor::wordRangeAt(std::size_t offset) const {
    const std::string_view text = buffer_.text();
    const std::size_t line = buffer_.lineOfOffset(offset);
    const std::size_t ls = buffer_.lineStart(line);
    const std::size_t le = buffer_.lineEnd(line);
    if (ls == le) return Selection{ls, ls};
    std::size_t pos = std::min(offset, le);
    if (pos == le) pos = utf8::previousBoundary(text, pos);
    const CharClass cls = classOf(utf8::decodeAt(text, pos));
    std::size_t begin = pos;
    while (begin > ls) {
        const std::size_t prev = utf8::previousBoundary(text, begin);
        if (classOf(utf8::decodeAt(text, prev)) != cls) break;
        begin = prev;
    }
    std::size_t end = pos;
    while (end < le && classOf(utf8::decodeAt(text, end)) == cls) end = utf8::nextBoundary(text, end);
    return Selection{begin, end};
}

SourceEditor::Selection SourceEditor::lineRangeAt(std::size_t offset) const {
    const std::size_t line = buffer_.lineOfOffset(offset);
    return Selection{buffer_.lineStart(line), std::min(buffer_.lineEnd(line) + 1, buffer_.size())};
}

// --- movement ----------------------------------------------------------------------

void SourceEditor::moveCaret(std::size_t offset, bool extend) {
    caret_ = std::min(offset, buffer_.size());
    if (!extend) anchor_ = caret_;
    invalidateTrack();
    ensureCaretVisible();
    invalidate();
}

void SourceEditor::moveVertical(long delta, bool extend, bool /*toEdges*/) {
    const std::size_t line = buffer_.lineOfOffset(caret_);
    if (!preferredColumn_) {
        preferredColumn_ = static_cast<double>(columnBetween(buffer_.lineStart(line), caret_));
    }
    const long target = static_cast<long>(line) + delta;
    std::size_t offset = 0;
    if (target < 0) {
        offset = 0;
    } else if (static_cast<std::size_t>(target) >= buffer_.lineCount()) {
        offset = buffer_.size();
    } else {
        offset = offsetAtColumn(static_cast<std::size_t>(target), *preferredColumn_);
    }
    moveCaret(offset, extend);
}

// --- editing ------------------------------------------------------------------------

bool SourceEditor::commitEdit(std::size_t offset, std::size_t removeLength, std::string insert,
                              SourceEditKind kind, std::size_t caretAfter) {
    insert = TextBuffer::normalize(insert);
    const std::size_t size = buffer_.size();
    if (offset > size || removeLength > size - offset) return false;
    if (!buffer_.isBoundary(offset) || !buffer_.isBoundary(offset + removeLength)) return false;
    if (removeLength == 0 && insert.empty()) return false;
    if (size - removeLength + insert.size() > kMaxBytes) return false;

    bool merge = false;
    const bool mergeable = kind == SourceEditKind::Typing || kind == SourceEditKind::Backspace ||
                           kind == SourceEditKind::DeleteForward;
    if (mergeable && track_.valid && track_.kind == kind && anchor_ == caret_ && caret_ == track_.end) {
        merge = true;
        if (kind == SourceEditKind::Typing && !insert.empty() && offset > 0) {
            // A word typed after whitespace starts a new undo step.
            const bool afterBlank = isBlank(buffer_.text()[offset - 1]);
            if (afterBlank && !isBlank(insert.front())) merge = false;
        }
    }

    const SourceEdit edit{offset, removeLength, buffer_.view(offset, removeLength), insert, kind, merge};
    if (sink_ != nullptr && !sink_->applyEdit(edit)) return false;
    if (!buffer_.replace(offset, removeLength, insert)) return false; // unreachable: validated above

    caret_ = anchor_ = std::min(caretAfter, buffer_.size());
    preferredColumn_.reset();
    track_ = EditTrack{mergeable, kind, caret_};
    ensureCaretVisible();
    invalidate();
    if (onTextChanged_) onTextChanged_();
    return true;
}

void SourceEditor::typeText(std::string text) {
    text = TextBuffer::normalize(text);
    if (text.empty()) return;
    const Selection sel = selection();
    const bool multiline = text.find('\n') != std::string::npos;
    commitEdit(sel.begin, sel.end - sel.begin, text, multiline ? SourceEditKind::Paste : SourceEditKind::Typing,
               sel.begin + text.size());
}

void SourceEditor::insertNewline() {
    const Selection sel = selection();
    const std::string_view text = buffer_.text();
    const std::size_t ls = buffer_.lineStart(buffer_.lineOfOffset(sel.begin));
    std::size_t i = ls;
    while (i < sel.begin && isBlank(text[i])) ++i;
    std::string insert = "\n";
    insert.append(text.substr(ls, i - ls));
    commitEdit(sel.begin, sel.end - sel.begin, insert, SourceEditKind::Newline, sel.begin + insert.size());
}

void SourceEditor::eraseBackward(bool word, bool line) {
    const Selection sel = selection();
    if (sel.begin != sel.end) {
        commitEdit(sel.begin, sel.end - sel.begin, {}, SourceEditKind::Backspace, sel.begin);
        return;
    }
    if (caret_ == 0) return;
    std::size_t start = utf8::previousBoundary(buffer_.text(), caret_);
    SourceEditKind kind = SourceEditKind::Backspace;
    if (line) {
        const std::size_t ls = buffer_.lineStart(buffer_.lineOfOffset(caret_));
        if (ls < caret_) start = ls;
        kind = SourceEditKind::DeleteLine;
    } else if (word) {
        start = wordLeft(caret_);
        kind = SourceEditKind::DeleteWord;
    }
    commitEdit(start, caret_ - start, {}, kind, start);
}

void SourceEditor::eraseForward(bool word, bool line) {
    const Selection sel = selection();
    if (sel.begin != sel.end) {
        commitEdit(sel.begin, sel.end - sel.begin, {}, SourceEditKind::DeleteForward, sel.begin);
        return;
    }
    if (caret_ >= buffer_.size()) return;
    std::size_t end = utf8::nextBoundary(buffer_.text(), caret_);
    SourceEditKind kind = SourceEditKind::DeleteForward;
    if (line) {
        const std::size_t le = buffer_.lineEnd(buffer_.lineOfOffset(caret_));
        if (le > caret_) end = le;
        kind = SourceEditKind::DeleteLine;
    } else if (word) {
        end = wordRight(caret_);
        kind = SourceEditKind::DeleteWord;
    }
    commitEdit(caret_, end - caret_, {}, kind, caret_);
}

void SourceEditor::indentSelection(bool outdent) {
    const Selection sel = selection();
    const std::size_t firstLine = buffer_.lineOfOffset(sel.begin);
    std::size_t lastLine = buffer_.lineOfOffset(sel.end);
    if (sel.end > sel.begin && lastLine > firstLine && sel.end == buffer_.lineStart(lastLine)) --lastLine;
    const std::size_t blockStart = buffer_.lineStart(firstLine);
    const std::size_t blockEnd = buffer_.lineEnd(lastLine);

    std::string block;
    std::vector<long> deltas;
    std::vector<std::size_t> newStarts; // new offset of each line start
    bool changed = false;
    for (std::size_t line = firstLine; line <= lastLine; ++line) {
        const std::string_view view = buffer_.view(buffer_.lineStart(line), buffer_.lineEnd(line) - buffer_.lineStart(line));
        newStarts.push_back(blockStart + block.size());
        long delta = 0;
        if (!outdent) {
            if (!view.empty()) {
                block.append(kIndentWidth, ' ');
                delta = static_cast<long>(kIndentWidth);
            }
            block.append(view);
        } else {
            std::size_t remove = 0;
            if (!view.empty() && view.front() == '\t') {
                remove = 1;
            } else {
                while (remove < kIndentWidth && remove < view.size() && view[remove] == ' ') ++remove;
            }
            delta = -static_cast<long>(remove);
            block.append(view.substr(remove));
        }
        changed = changed || delta != 0;
        deltas.push_back(delta);
        if (line != lastLine) block.push_back('\n');
    }
    if (!changed) return;

    long total = 0;
    for (const long d : deltas) total += d;
    const auto map = [&](std::size_t offset) {
        if (offset > blockEnd) return static_cast<std::size_t>(static_cast<long>(offset) + total);
        const std::size_t line = std::clamp(buffer_.lineOfOffset(offset), firstLine, lastLine);
        const long column = static_cast<long>(offset - buffer_.lineStart(line));
        const long moved = std::max(0L, column + deltas[line - firstLine]);
        return newStarts[line - firstLine] + static_cast<std::size_t>(moved);
    };
    const std::size_t newAnchor = map(anchor_);
    const std::size_t newCaret = map(caret_);
    if (commitEdit(blockStart, blockEnd - blockStart, std::move(block), SourceEditKind::Indent, newCaret)) {
        anchor_ = newAnchor;
        caret_ = newCaret;
    }
}

void SourceEditor::copySelection() const {
    const Selection sel = selection();
    if (clipboard_ == nullptr || sel.begin == sel.end) return;
    clipboard_->setText(buffer_.slice(sel.begin, sel.end - sel.begin));
}

void SourceEditor::cutSelection() {
    const Selection sel = selection();
    if (clipboard_ == nullptr || sel.begin == sel.end) return;
    if (!clipboard_->setText(buffer_.slice(sel.begin, sel.end - sel.begin))) return; // never lose the text
    commitEdit(sel.begin, sel.end - sel.begin, {}, SourceEditKind::Cut, sel.begin);
}

void SourceEditor::paste() {
    if (clipboard_ == nullptr) return;
    const std::string text = TextBuffer::normalize(clipboard_->text());
    if (text.empty()) return;
    const Selection sel = selection();
    commitEdit(sel.begin, sel.end - sel.begin, text, SourceEditKind::Paste, sel.begin + text.size());
}

// --- keyboard ------------------------------------------------------------------------

bool SourceEditor::onKey(const KeyEvent& event) {
    if (!isFocused()) return false;
    const ModifierFlags& mods = event.modifiers;
    const bool primary = mods.command || mods.control;
    const bool shift = mods.shift;
    const bool option = mods.option;

    const bool vertical = event.key == Key::Up || event.key == Key::Down;
    if (!vertical && event.key != Key::PageUp && event.key != Key::PageDown) preferredColumn_.reset();

    // Application shortcuts: the clipboard/select-all set is ours; everything
    // else (undo/redo, save, find, mode switches...) goes to the shell.
    if (primary && event.key == Key::Character) {
        if (event.text == "a" && !shift) {
            anchor_ = 0;
            caret_ = buffer_.size();
            invalidateTrack();
            ensureCaretVisible();
            invalidate();
        } else if (event.text == "c") {
            copySelection();
        } else if (event.text == "x") {
            cutSelection();
        } else if (event.text == "v") {
            paste();
        } else {
            return false;
        }
        event.accepted = true;
        return true;
    }

    const std::string_view text = buffer_.text();
    switch (event.key) {
    case Key::Left:
        if (mods.command) {
            moveCaret(buffer_.lineStart(buffer_.lineOfOffset(caret_)), shift);
        } else if (option) {
            moveCaret(wordLeft(caret_), shift);
        } else if (!shift && caret_ != anchor_) {
            moveCaret(selection().begin, false);
        } else {
            moveCaret(utf8::previousBoundary(text, caret_), shift);
        }
        break;
    case Key::Right:
        if (mods.command) {
            moveCaret(buffer_.lineEnd(buffer_.lineOfOffset(caret_)), shift);
        } else if (option) {
            moveCaret(wordRight(caret_), shift);
        } else if (!shift && caret_ != anchor_) {
            moveCaret(selection().end, false);
        } else {
            moveCaret(utf8::nextBoundary(text, caret_), shift);
        }
        break;
    case Key::Up:
        if (mods.command) {
            moveCaret(0, shift);
        } else {
            moveVertical(-1, shift, false);
        }
        break;
    case Key::Down:
        if (mods.command) {
            moveCaret(buffer_.size(), shift);
        } else {
            moveVertical(1, shift, false);
        }
        break;
    case Key::Home:
        moveCaret(mods.command ? 0 : buffer_.lineStart(buffer_.lineOfOffset(caret_)), shift);
        break;
    case Key::End:
        moveCaret(mods.command ? buffer_.size() : buffer_.lineEnd(buffer_.lineOfOffset(caret_)), shift);
        break;
    case Key::PageUp:
    case Key::PageDown: {
        const long page = std::max(1L, static_cast<long>(viewHeight() / lineHeight_) - 1);
        const long delta = event.key == Key::PageUp ? -page : page;
        setScroll(scrollX_, scrollY_ + static_cast<double>(delta) * lineHeight_);
        moveVertical(delta, shift, false);
        break;
    }
    case Key::Backspace:
        eraseBackward(option, mods.command);
        break;
    case Key::Delete:
        eraseForward(option, mods.command);
        break;
    case Key::Enter:
        if (primary) return false;
        insertNewline();
        break;
    case Key::Tab:
        if (primary) return false; // Ctrl+Tab cycles tabs
        if (shift) {
            indentSelection(true);
        } else if (caret_ != anchor_) {
            indentSelection(false);
        } else {
            typeText(std::string(kIndentWidth, ' '));
        }
        break;
    case Key::Space:
        typeText(event.text.empty() ? std::string(" ") : event.text);
        break;
    case Key::Plus:
        typeText(!event.text.empty() ? event.text : std::string(shift ? "+" : "="));
        break;
    case Key::Minus:
        typeText(event.text.empty() ? std::string("-") : event.text);
        break;
    case Key::Character: {
        if (primary || event.text.empty()) return false;
        const auto first = static_cast<unsigned char>(event.text.front());
        if ((first < 0x20 && first != '\t') || first == 0x7F) return false;
        // NSEvent function-key private-use code points (U+F700..U+F8FF).
        if (event.text.size() >= 3 && first == 0xEF && static_cast<unsigned char>(event.text[1]) >= 0x9C &&
            static_cast<unsigned char>(event.text[1]) <= 0xA3) {
            return false;
        }
        typeText(event.text);
        break;
    }
    default:
        return false; // Escape, Unknown
    }
    event.accepted = true;
    return true;
}

// --- mouse ----------------------------------------------------------------------------

bool SourceEditor::onMouse(const PointerEvent& event) {
    if (drag_ == DragMode::None && Widget::onMouse(event)) return true; // scrollbars

    switch (event.type) {
    case PointerEventType::Down: {
        if (event.button != 1) return false;
        if (onFocusRequested_) onFocusRequested_();
        const auto now = clock_ ? clock_() : std::chrono::steady_clock::now();
        const bool repeat = clickCount_ > 0 && (now - lastClickTime_) < kMultiClickWindow &&
                            std::abs(event.position.x - lastClickPoint_.x) <= kMultiClickSlop &&
                            std::abs(event.position.y - lastClickPoint_.y) <= kMultiClickSlop;
        clickCount_ = repeat ? (clickCount_ % 3) + 1 : 1;
        lastClickTime_ = now;
        lastClickPoint_ = event.position;

        const std::size_t offset = offsetForPoint(event.position);
        if (clickCount_ == 1) {
            drag_ = DragMode::Char;
            if (!event.modifiers.shift) anchor_ = offset;
            caret_ = offset;
        } else {
            drag_ = clickCount_ == 2 ? DragMode::Word : DragMode::Line;
            dragAnchor_ = clickCount_ == 2 ? wordRangeAt(offset) : lineRangeAt(offset);
            anchor_ = dragAnchor_.begin;
            caret_ = dragAnchor_.end;
        }
        preferredColumn_.reset();
        invalidateTrack();
        ensureCaretVisible();
        invalidate();
        event.accepted = true;
        return true;
    }
    case PointerEventType::Move: {
        if (drag_ == DragMode::None) return false;
        if (event.button == 0) { // the Up went elsewhere
            drag_ = DragMode::None;
            return false;
        }
        const std::size_t offset = offsetForPoint(event.position);
        if (drag_ == DragMode::Char) {
            caret_ = offset;
        } else {
            const Selection range = drag_ == DragMode::Word ? wordRangeAt(offset) : lineRangeAt(offset);
            if (range.begin < dragAnchor_.begin) {
                anchor_ = dragAnchor_.end;
                caret_ = range.begin;
            } else {
                anchor_ = dragAnchor_.begin;
                caret_ = std::max(range.end, dragAnchor_.end);
            }
        }
        ensureCaretVisible();
        invalidate();
        event.accepted = true;
        return true;
    }
    case PointerEventType::Up:
        if (drag_ == DragMode::None) return false;
        drag_ = DragMode::None;
        event.accepted = true;
        return true;
    case PointerEventType::Scroll: {
        if (!bounds().contains(event.position)) return false;
        const double maxY = std::max(0.0, contentHeight() - viewHeight());
        const double maxX = std::max(0.0, contentWidth() - viewWidth());
        if (maxX <= 0.0 && maxY <= 0.0) return false;
        setScroll(scrollX_ + event.scrollDelta.x, scrollY_ + event.scrollDelta.y);
        event.accepted = true;
        return true;
    }
    case PointerEventType::Entered:
    case PointerEventType::Exited:
        return false;
    }
    return false;
}

// --- paint -----------------------------------------------------------------------------

void SourceEditor::setHighlights(std::vector<Highlight> ranges, std::optional<std::size_t> current) {
    highlights_ = std::move(ranges);
    currentHighlight_ = current && *current < highlights_.size() ? current : std::nullopt;
    highlightsRevision_ = buffer_.revision();
    invalidate();
}

void SourceEditor::paintSelf(PaintContext& context) const {
    measureCell(context);
    Font font;
    font.size = fontSize_;
    font.monospace = true;

    const core::Rect all = bounds();
    context.fillRect(all, kBackground);

    const core::Rect area = textRect();
    const double gutter = gutterWidth();
    const std::size_t first = firstVisibleLine();
    const std::size_t last = lastVisibleLine();
    const Selection sel = selection();
    const bool focused = isFocused();
    const std::string_view text = buffer_.text();

    if (gutter > 0.0) {
        context.pushClip(core::Rect{0.0, 0.0, gutter, area.size.height});
        context.fillRect(core::Rect{0.0, 0.0, gutter, area.size.height}, kGutterBackground);
        for (std::size_t line = first; line <= last; ++line) {
            const double y = static_cast<double>(line) * lineHeight_ - scrollY_;
            context.drawText(std::to_string(line + 1), core::Rect{0.0, y, gutter - 8.0, lineHeight_}, font,
                             kGutterText, TextAlign::Right);
        }
        context.drawLine(core::Point{gutter - 0.5, 0.0}, core::Point{gutter - 0.5, area.size.height},
                         kGutterSeparator, 1.0);
        context.popClip();
    }

    context.pushClip(area);
    const std::size_t firstCol = static_cast<std::size_t>(std::max(0.0, scrollX_ - kPadding) / cellWidth_);
    const std::size_t lastCol = firstCol + static_cast<std::size_t>(area.size.width / cellWidth_) + 3;
    const std::size_t caretLineIndex = buffer_.lineOfOffset(caret_);
    for (std::size_t line = first; line <= last; ++line) {
        const double y = static_cast<double>(line) * lineHeight_ - scrollY_;
        const std::size_t ls = buffer_.lineStart(line);
        const std::size_t le = buffer_.lineEnd(line);

        if (sel.begin != sel.end && sel.begin <= le && sel.end > ls) {
            const std::size_t from = columnBetween(ls, std::max(sel.begin, ls));
            const std::size_t to = columnBetween(ls, std::min(sel.end, le)) + (sel.end > le ? 1 : 0);
            if (to > from) {
                context.fillRect(core::Rect{kPadding + static_cast<double>(from) * cellWidth_ - scrollX_, y,
                                            static_cast<double>(to - from) * cellWidth_, lineHeight_},
                                 focused ? kSelectionFocused : kSelectionUnfocused);
            }
        }

        if (!highlights_.empty() && highlightsRevision_ == buffer_.revision()) {
            auto it = std::lower_bound(highlights_.begin(), highlights_.end(), ls,
                                       [](const Highlight& h, std::size_t value) { return h.end <= value; });
            for (; it != highlights_.end() && it->begin <= le; ++it) {
                const std::size_t from = columnBetween(ls, std::max(it->begin, ls));
                const std::size_t to = columnBetween(ls, std::min(it->end, le));
                if (to <= from) continue;
                const bool isCurrent = currentHighlight_ && static_cast<std::size_t>(it - highlights_.begin()) == *currentHighlight_;
                context.fillRect(core::Rect{kPadding + static_cast<double>(from) * cellWidth_ - scrollX_, y,
                                            static_cast<double>(to - from) * cellWidth_, lineHeight_},
                                 isCurrent ? Color::rgba(1.0, 0.55, 0.0, 0.6) : Color::rgba(1.0, 0.85, 0.0, 0.45));
            }
        }

        std::size_t startCol = 0;
        const std::string slice = visibleSlice(text.substr(ls, le - ls), firstCol, lastCol, &startCol);
        if (!slice.empty()) {
            const double x = kPadding + static_cast<double>(startCol) * cellWidth_ - scrollX_;
            context.drawText(slice, core::Rect{x, y, (static_cast<double>(lastCol - startCol) + 2.0) * cellWidth_,
                                               lineHeight_},
                             font, kTextColor, TextAlign::Left);
        }

        if (focused && line == caretLineIndex) {
            const double x = kPadding + static_cast<double>(columnBetween(ls, caret_)) * cellWidth_ - scrollX_;
            context.fillRect(core::Rect{x, y, kCaretWidth, lineHeight_}, kCaretColor);
        }
    }
    context.popClip();
}

} // namespace rivet::ui
