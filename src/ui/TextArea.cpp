// SPDX-License-Identifier: MPL-2.0
#include "ui/TextArea.hpp"

#include "core/geometry/Insets.hpp"
#include "core/geometry/Point.hpp"
#include "ui/Utf8.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Font kTextFont{13.0, Font::Weight::Regular};

constexpr Color kUnfocusedBackground = Color::gray(0.96);
constexpr Color kFocusedBackground = Color::white();
constexpr Color kUnfocusedBorder = Color::rgba(0.0, 0.0, 0.0, 0.2);
constexpr Color kFocusedBorder = Color::rgba(0.0, 0.47, 1.0, 1.0); // accent blue
constexpr Color kTextColor = Color::black();
constexpr Color kPlaceholderColor = Color::gray(0.55);
constexpr Color kSelectionFill = Color::rgba(0.0, 0.47, 1.0, 0.25);
constexpr Color kCaretColor = Color::rgba(0.0, 0.47, 1.0, 1.0);
constexpr double kCaretWidth = 1.5;
constexpr double kStrokeWidth = 1.0;
// Width assumed for a code point not measured yet (typed since the last
// paint) and drawn for a selected line break.
constexpr double kEstimatedGlyphWidth = 7.0;
constexpr double kNewlineSelectionWidth = 5.0;

bool isSpace(char32_t codePoint) {
    return codePoint == U' ' || codePoint == U'\t';
}

} // namespace

TextArea::TextArea(std::string placeholder) : placeholder_(std::move(placeholder)) {}

TextArea::Selection TextArea::selection() const {
    if (anchor_ == caret_) return Selection{caret_, caret_};
    return Selection{std::min(anchor_, caret_), std::max(anchor_, caret_)};
}

void TextArea::setMaxBytes(std::size_t maxBytes) {
    maxBytes_ = maxBytes;
    if (text_.size() > maxBytes_) {
        text_.resize(utf8::floorBoundary(text_, maxBytes_));
        caret_ = std::min(caret_, text_.size());
        anchor_ = std::min(anchor_, text_.size());
        textChanged();
        invalidate();
    }
}

void TextArea::setText(std::string text) {
    if (text.size() > maxBytes_) text.resize(utf8::floorBoundary(text, maxBytes_));
    text_ = std::move(text);
    caret_ = anchor_ = text_.size();
    caretUpstream_ = false;
    preferredX_.reset();
    scrollY_ = 0.0;
    textChanged();
    ensureCaretVisible();
    invalidate();
}

void TextArea::setOnTextChanged(std::function<void(const std::string&)> onTextChanged) {
    onTextChanged_ = std::move(onTextChanged);
}

void TextArea::setOnCommit(std::function<void()> onCommit) {
    onCommit_ = std::move(onCommit);
}

void TextArea::setOnEscape(std::function<void()> onEscape) {
    onEscape_ = std::move(onEscape);
}

void TextArea::setOnFocusRequested(std::function<void()> onFocusRequested) {
    onFocusRequested_ = std::move(onFocusRequested);
}

bool TextArea::wantsFocus() const {
    return true;
}

core::Size TextArea::preferredSize(const PaintContext& /*context*/) const {
    const core::Size current = frame().size;
    return core::Size{current.width > 0.0 ? current.width : kPreferredWidth,
                      current.height > 0.0 ? current.height : kPreferredHeight};
}

void TextArea::layout() {
    clampScroll();
    ensureCaretVisible();
}

// --- layout -----------------------------------------------------------------

double TextArea::wrapWidth() const {
    const double width = frame().size.width;
    if (!(width > 0.0)) return std::numeric_limits<double>::infinity();
    return std::max(0.0, width - 2.0 * kPadding);
}

double TextArea::glyphWidth(char32_t codePoint) const {
    const auto it = glyphWidths_.find(codePoint);
    return it == glyphWidths_.end() ? kEstimatedGlyphWidth : it->second;
}

void TextArea::measureNewGlyphs(const PaintContext& context) const {
    bool added = false;
    std::string glyph;
    for (std::size_t at = 0; at < text_.size();) {
        const std::size_t next = utf8::nextBoundary(text_, at);
        const char32_t codePoint = utf8::decodeAt(text_, at);
        if (glyphWidths_.find(codePoint) == glyphWidths_.end()) {
            glyph.assign(text_, at, next - at);
            glyphWidths_.emplace(codePoint, context.measureText(glyph, kTextFont).width);
            added = true;
        }
        at = next;
    }
    if (added) ++glyphRevision_;
}

double TextArea::spanWidth(std::size_t from, std::size_t to) const {
    double width = 0.0;
    for (std::size_t at = from; at < to;) {
        width += glyphWidth(utf8::decodeAt(text_, at));
        at = utf8::nextBoundary(text_, at);
    }
    return width;
}

const std::vector<TextArea::VisualLine>& TextArea::lines() const {
    const double width = wrapWidth();
    if (layoutTextRevision_ == textRevision_ && layoutGlyphRevision_ == glyphRevision_ &&
        layoutWrapWidth_ == width) {
        return lines_;
    }
    lines_.clear();

    std::size_t hardBegin = 0;
    for (;;) {
        const std::size_t newline = text_.find('\n', hardBegin);
        const std::size_t hardEnd = newline == std::string::npos ? text_.size() : newline;

        std::size_t lineBegin = hardBegin;
        double x = 0.0;
        std::size_t lastBreak = std::string::npos; // offset just after the last space
        for (std::size_t at = hardBegin; at < hardEnd;) {
            const char32_t codePoint = utf8::decodeAt(text_, at);
            const double w = glyphWidth(codePoint);
            // Trailing spaces may overhang the width; everything else wraps.
            while (!isSpace(codePoint) && x + w > width && at > lineBegin) {
                if (lastBreak != std::string::npos && lastBreak > lineBegin) {
                    lines_.push_back(VisualLine{lineBegin, lastBreak, true});
                    lineBegin = lastBreak;
                    x = spanWidth(lineBegin, at);
                    lastBreak = std::string::npos;
                } else {
                    lines_.push_back(VisualLine{lineBegin, at, true});
                    lineBegin = at;
                    x = 0.0;
                }
            }
            x += w;
            at = utf8::nextBoundary(text_, at);
            if (isSpace(codePoint)) lastBreak = at;
        }
        lines_.push_back(VisualLine{lineBegin, hardEnd, false});
        if (newline == std::string::npos) break;
        hardBegin = newline + 1;
    }

    layoutTextRevision_ = textRevision_;
    layoutGlyphRevision_ = glyphRevision_;
    layoutWrapWidth_ = width;
    return lines_;
}

std::size_t TextArea::lineCount() const {
    return lines().size();
}

TextArea::Selection TextArea::lineRange(std::size_t index) const {
    const auto& all = lines();
    if (index >= all.size()) return Selection{};
    return Selection{all[index].begin, all[index].end};
}

std::size_t TextArea::lineIndexForOffset(std::size_t offset, bool upstream) const {
    const auto& all = lines();
    // Last line starting at or before the offset (the later of two lines
    // meeting at a soft-wrap boundary).
    std::size_t index = all.size() - 1;
    while (index > 0 && all[index].begin > offset) --index;
    if (upstream && index > 0 && all[index - 1].soft && all[index - 1].end == offset) --index;
    return index;
}

std::size_t TextArea::caretLine() const {
    return lineIndexForOffset(caret_, caretUpstream_);
}

std::size_t TextArea::boundaryOnLine(const VisualLine& line, double x) const {
    std::size_t best = line.begin;
    double bestDistance = std::fabs(x);
    double position = 0.0;
    for (std::size_t at = line.begin; at < line.end;) {
        position += glyphWidth(utf8::decodeAt(text_, at));
        at = utf8::nextBoundary(text_, at);
        const double distance = std::fabs(position - x);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = at;
        }
    }
    return best;
}

std::size_t TextArea::offsetForPoint(core::Point local, bool* upstream) const {
    const auto& all = lines();
    const double contentY = local.y - kPadding + scrollY_;
    const double row = std::floor(contentY / kLineHeight);
    const std::size_t index =
        row <= 0.0 ? 0 : std::min(all.size() - 1, static_cast<std::size_t>(row));
    const VisualLine& line = all[index];
    const std::size_t offset = boundaryOnLine(line, local.x - kPadding);
    if (upstream != nullptr) *upstream = line.soft && offset == line.end;
    return offset;
}

// --- scrolling --------------------------------------------------------------

double TextArea::contentHeight() const {
    return static_cast<double>(lines().size()) * kLineHeight;
}

double TextArea::viewHeight() const {
    return std::max(0.0, frame().size.height - 2.0 * kPadding);
}

void TextArea::clampScroll() const {
    const double maxScroll = std::max(0.0, contentHeight() - viewHeight());
    scrollY_ = std::clamp(scrollY_, 0.0, maxScroll);
}

void TextArea::ensureCaretVisible() {
    if (!(frame().size.height > 0.0)) {
        scrollY_ = 0.0;
        return;
    }
    const double top = static_cast<double>(caretLine()) * kLineHeight;
    const double bottom = top + kLineHeight;
    const double view = viewHeight();
    if (top < scrollY_) {
        scrollY_ = top;
    } else if (bottom > scrollY_ + view) {
        scrollY_ = bottom - view;
    }
    clampScroll();
}

// --- editing ----------------------------------------------------------------

void TextArea::textChanged() {
    ++textRevision_;
}

void TextArea::notifyTextChanged() {
    textChanged();
    if (onTextChanged_) onTextChanged_(text_);
}

void TextArea::moveCaret(std::size_t offset, bool upstream, bool extend) {
    caret_ = offset;
    caretUpstream_ = upstream;
    if (!extend) anchor_ = offset;
    ensureCaretVisible();
    invalidate();
}

void TextArea::moveVertical(int direction, bool extend) {
    const auto& all = lines();
    const std::size_t current = caretLine();
    const double x = preferredX_.value_or(spanWidth(all[current].begin, caret_));
    preferredX_ = x;
    if (direction < 0 && current == 0) {
        moveCaret(0, false, extend);
    } else if (direction > 0 && current + 1 == all.size()) {
        moveCaret(text_.size(), false, extend);
    } else {
        const VisualLine& target = all[direction < 0 ? current - 1 : current + 1];
        const std::size_t offset = boundaryOnLine(target, x);
        moveCaret(offset, target.soft && offset == target.end, extend);
    }
}

void TextArea::eraseSelection() {
    const Selection selected = selection();
    if (selected.begin == selected.end) return;
    text_.erase(selected.begin, selected.end - selected.begin);
    caret_ = anchor_ = selected.begin;
}

bool TextArea::insertText(std::string insertion) {
    // Control bytes other than '\n' never enter the text (stray '\r', ESC...).
    insertion.erase(std::remove_if(insertion.begin(), insertion.end(),
                                   [](char c) {
                                       const auto byte = static_cast<unsigned char>(c);
                                       return (byte < 0x20u && c != '\n') || byte == 0x7Fu;
                                   }),
                    insertion.end());
    if (insertion.empty()) return false;

    const Selection selected = selection();
    const std::size_t remaining = text_.size() - (selected.end - selected.begin);
    const std::size_t room = maxBytes_ > remaining ? maxBytes_ - remaining : 0;
    if (insertion.size() > room) insertion.resize(utf8::floorBoundary(insertion, room));
    if (insertion.empty()) return false; // full: keep the selection untouched

    eraseSelection();
    text_.insert(caret_, insertion);
    caret_ += insertion.size();
    anchor_ = caret_;
    caretUpstream_ = false;
    notifyTextChanged();
    ensureCaretVisible();
    return true;
}

// --- input ------------------------------------------------------------------

bool TextArea::onMouse(const PointerEvent& event) {
    const bool inside = bounds().contains(event.position);
    switch (event.type) {
    case PointerEventType::Down: {
        if (!inside) return false;
        if (onFocusRequested_) onFocusRequested_();
        preferredX_.reset();
        bool upstream = false;
        const std::size_t offset = offsetForPoint(event.position, &upstream);
        moveCaret(offset, upstream, event.modifiers.shift);
        pressed_ = true;
        event.accepted = true;
        return true;
    }

    case PointerEventType::Move: {
        if (!pressed_) return false;
        if (event.button != 1) {
            pressed_ = false; // the matching Up was lost elsewhere
            return false;
        }
        bool upstream = false;
        const std::size_t offset = offsetForPoint(event.position, &upstream);
        moveCaret(offset, upstream, true);
        event.accepted = true;
        return true;
    }

    case PointerEventType::Up: {
        if (!pressed_) return false;
        pressed_ = false;
        event.accepted = true;
        invalidate();
        return true;
    }

    case PointerEventType::Scroll: {
        if (!inside) return false;
        const double maxScroll = std::max(0.0, contentHeight() - viewHeight());
        if (maxScroll <= 0.0) return false; // nothing to scroll: let the parent have it
        scrollY_ = std::clamp(scrollY_ + event.scrollDelta.y, 0.0, maxScroll);
        event.accepted = true;
        invalidate();
        return true;
    }

    case PointerEventType::Entered:
    case PointerEventType::Exited:
        return false;
    }
    return false;
}

bool TextArea::onKey(const KeyEvent& event) {
    if (!isFocused()) return false;

    const bool commandHeld = event.modifiers.command || event.modifiers.control;
    const bool extend = event.modifiers.shift;

    if (event.key == Key::Escape) {
        if (onEscape_) onEscape_();
        event.accepted = true;
        return true;
    }
    if (event.key == Key::Enter && commandHeld) {
        if (onCommit_) onCommit_();
        event.accepted = true;
        return true;
    }

    if (event.key != Key::Up && event.key != Key::Down) preferredX_.reset();

    if (commandHeld) {
        bool handled = true;
        if (event.key == Key::Character && (event.text == "a" || event.text == "A")) {
            anchor_ = 0;
            caret_ = text_.size();
            caretUpstream_ = false;
            ensureCaretVisible();
        } else if (event.key == Key::Up) {
            moveCaret(0, false, extend);
        } else if (event.key == Key::Down) {
            moveCaret(text_.size(), false, extend);
        } else if (event.key == Key::Left) {
            moveCaret(lines()[caretLine()].begin, false, extend);
        } else if (event.key == Key::Right) {
            const VisualLine& line = lines()[caretLine()];
            moveCaret(line.end, line.soft, extend);
        } else {
            handled = false; // an application shortcut: pass it through
        }
        if (!handled) return false;
        event.accepted = true;
        invalidate();
        return true;
    }

    switch (event.key) {
    case Key::Left:
        moveCaret(utf8::previousBoundary(text_, caret_), false, extend);
        break;
    case Key::Right:
        moveCaret(utf8::nextBoundary(text_, caret_), false, extend);
        break;
    case Key::Up:
        moveVertical(-1, extend);
        break;
    case Key::Down:
        moveVertical(1, extend);
        break;
    case Key::Home:
        moveCaret(lines()[caretLine()].begin, false, extend);
        break;
    case Key::End: {
        const VisualLine& line = lines()[caretLine()];
        moveCaret(line.end, line.soft, extend);
        break;
    }
    case Key::Backspace: {
        const Selection selected = selection();
        if (selected.begin != selected.end) {
            eraseSelection();
        } else if (caret_ > 0) {
            const std::size_t start = utf8::previousBoundary(text_, caret_);
            text_.erase(start, caret_ - start); // whole code points only
            caret_ = anchor_ = start;
        } else {
            break; // a no-op is not an edit
        }
        caretUpstream_ = false;
        notifyTextChanged();
        ensureCaretVisible();
        break;
    }
    case Key::Delete: {
        const Selection selected = selection();
        if (selected.begin != selected.end) {
            eraseSelection();
        } else if (caret_ < text_.size()) {
            const std::size_t end = utf8::nextBoundary(text_, caret_);
            text_.erase(caret_, end - caret_); // whole code points only
            anchor_ = caret_;
        } else {
            break;
        }
        caretUpstream_ = false;
        notifyTextChanged();
        ensureCaretVisible();
        break;
    }
    case Key::Enter:
        insertText("\n");
        break;
    // The macOS key mapping reports Space, '+' and '-' as dedicated keys with
    // no text; a platform that fills event.text wins.
    case Key::Space:
        insertText(event.text.empty() ? std::string(" ") : event.text);
        break;
    case Key::Plus:
        insertText(!event.text.empty() ? event.text : std::string(event.modifiers.shift ? "+" : "="));
        break;
    case Key::Minus:
        insertText(event.text.empty() ? std::string("-") : event.text);
        break;
    case Key::Character:
        if (event.text.empty()) return false;
        insertText(event.text);
        break;
    default:
        return false; // Tab, Escape handled above, PageUp/Down, Unknown: not ours
    }

    event.accepted = true;
    invalidate();
    return true;
}

// --- paint ------------------------------------------------------------------

void TextArea::paintSelf(PaintContext& context) const {
    const core::Rect rect = bounds();
    measureNewGlyphs(context);
    clampScroll();
    const auto& all = lines();

    const bool focused = isFocused();
    context.fillRoundedRect(rect, focused ? kFocusedBackground : kUnfocusedBackground, kCornerRadius);
    context.strokeRect(rect.inset(core::Insets::uniform(0.5)),
                       focused ? kFocusedBorder : kUnfocusedBorder, kStrokeWidth);

    context.pushClip(rect);
    if (text_.empty()) {
        if (!focused && !placeholder_.empty()) {
            context.drawText(placeholder_,
                             core::Rect{core::Point{kPadding, kPadding},
                                        core::Size{rect.size.width, kLineHeight}},
                             kTextFont, kPlaceholderColor, TextAlign::Left);
        }
    } else {
        const Selection selected = selection();
        const std::size_t first =
            std::min(all.size() - 1, static_cast<std::size_t>(std::floor(scrollY_ / kLineHeight)));
        for (std::size_t index = first; index < all.size(); ++index) {
            const VisualLine& line = all[index];
            const double top = kPadding + static_cast<double>(index) * kLineHeight - scrollY_;
            if (top >= rect.size.height) break;

            if (selected.begin != selected.end && selected.begin <= line.end &&
                selected.end >= line.begin) {
                const std::size_t from = std::max(selected.begin, line.begin);
                const std::size_t to = std::min(selected.end, line.end);
                if (from <= to) {
                    double left = kPadding + spanWidth(line.begin, from);
                    double right = kPadding + spanWidth(line.begin, to);
                    // A selected line break shows as a small trailing stub.
                    if (!line.soft && selected.end > line.end) right += kNewlineSelectionWidth;
                    if (right > left) {
                        context.fillRect(core::Rect{core::Point{left, top},
                                                    core::Size{right - left, kLineHeight}},
                                         kSelectionFill);
                    }
                }
            }
            if (line.end > line.begin) {
                const core::Rect lineRect{core::Point{kPadding, top},
                                          core::Size{std::max(0.0, rect.size.width - kPadding), kLineHeight}};
                context.drawText(std::string_view{text_}.substr(line.begin, line.end - line.begin),
                                 lineRect, kTextFont, kTextColor, TextAlign::Left);
            }
        }
    }
    if (focused) {
        const VisualLine& line = all[caretLine()];
        const double caretX = kPadding + spanWidth(line.begin, caret_);
        const double top = kPadding + static_cast<double>(caretLine()) * kLineHeight - scrollY_;
        context.drawLine(core::Point{caretX, top}, core::Point{caretX, top + kLineHeight},
                         kCaretColor, kCaretWidth);
    }
    context.popClip();
}

} // namespace rivet::ui
