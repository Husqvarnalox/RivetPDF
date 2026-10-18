// SPDX-License-Identifier: MPL-2.0
#include "ui/TextField.hpp"
#include "ui/Theme.hpp"

#include "core/SecureWipe.hpp"
#include "core/geometry/Insets.hpp"
#include "core/geometry/Point.hpp"
#include "ui/Drawing.hpp"
#include "ui/Utf8.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::ui {
namespace {

constexpr Font kTextFont{13.0, Font::Weight::Regular};

constexpr double kCaretWidth = 1.5;
constexpr double kStrokeWidth = 1.0;

} // namespace

TextField::TextField(std::string placeholder) : placeholder_(std::move(placeholder)) {}

void TextField::setPlaceholder(std::string placeholder) {
    if (placeholder == placeholder_) return;
    placeholder_ = std::move(placeholder);
    invalidate();
}

void TextField::setText(std::string text) {
    // The previous contents may be a typed password (masked fields): zero the
    // old storage before it is released.
    core::wipeString(text_);
    text_ = std::move(text);
    caret_ = text_.size();
    anchor_ = caret_;
    prefixWidths_.clear();
    invalidate();
}

void TextField::selectAll() {
    anchor_ = 0;
    caret_ = text_.size();
    invalidate();
}

TextField::Selection TextField::selection() const {
    if (anchor_ == caret_) return Selection{caret_, caret_};
    return Selection{std::min(anchor_, caret_), std::max(anchor_, caret_)};
}

void TextField::setOnTextChanged(std::function<void(const std::string&)> onTextChanged) {
    onTextChanged_ = std::move(onTextChanged);
}

void TextField::setOnEnter(std::function<void()> onEnter) {
    onEnter_ = std::move(onEnter);
}

void TextField::setOnEscape(std::function<void()> onEscape) {
    onEscape_ = std::move(onEscape);
}

void TextField::setOnFocusRequested(std::function<void()> onFocusRequested) {
    onFocusRequested_ = std::move(onFocusRequested);
}

std::size_t TextField::previousBoundary(const std::string& s, std::size_t byteIndex) {
    return utf8::previousBoundary(s, byteIndex);
}

std::size_t TextField::nextBoundary(const std::string& s, std::size_t byteIndex) {
    return utf8::nextBoundary(s, byteIndex);
}

core::Size TextField::preferredSize(const PaintContext& context) const {
    const std::string_view measured =
        text_.empty() ? std::string_view{placeholder_} : std::string_view{text_};
    const core::Size textSize = context.measureText(measured, kTextFont);
    return core::Size{textSize.width + 2.0 * kHorizontalPadding, std::max(textSize.height, kMinHeight)};
}

bool TextField::wantsFocus() const {
    return true;
}

void TextField::rebuildPrefixWidths(const PaintContext& context) const {
    if (!prefixWidths_.empty() && prefixWidths_.size() == text_.size() + 1) return;
    prefixWidths_.assign(text_.size() + 1, 0.0);
    double width = 0.0;
    std::size_t at = 0;
    while (at < text_.size()) {
        const std::size_t next = nextBoundary(text_, at);
        // Measure each code point individually and accumulate: per-glyph
        // widths are what caret mapping needs, even if the total differs
        // slightly from measuring the whole string at once.
        width += context.measureText(std::string_view{text_}.substr(at, next - at), kTextFont).width;
        prefixWidths_[next] = width;
        at = next;
    }
}

double TextField::prefixWidthUpTo(std::size_t byteIndex) const {
    if (byteIndex >= prefixWidths_.size()) {
        return prefixWidths_.empty() ? 0.0 : prefixWidths_.back();
    }
    return prefixWidths_[byteIndex];
}

void TextField::updateHorizontalScroll(const core::Rect& bounds) const {
    const double viewWidth = std::max(0.0, bounds.size.width - leadingPadding() - kHorizontalPadding);
    const double caretX = prefixWidthUpTo(caret_);
    double scroll = horizontalScroll_;
    if (caretX < scroll) {
        scroll = caretX; // caret scrolled off the left: pin to the left edge
    } else if (caretX - scroll > viewWidth) {
        scroll = caretX - viewWidth; // caret off the right: pin to the right edge
    }
    horizontalScroll_ = std::max(0.0, scroll);
}

std::size_t TextField::boundaryForClickX(double localX) const {
    if (prefixWidths_.size() != text_.size() + 1) {
        // No cached measurement (never painted since the last text change):
        // right of the text start maps to the end, at/before it to 0.
        return localX > leadingPadding() ? text_.size() : 0;
    }
    // Click position in text space (text origin sits at the padded left edge
    // minus the scroll offset).
    const double textX = localX - leadingPadding() + horizontalScroll_;
    std::size_t bestByte = 0;
    double bestDistance = 0.0;
    bool found = false;
    for (std::size_t at = 0;; at = nextBoundary(text_, at)) {
        const double distance = std::fabs(prefixWidths_[at] - textX);
        if (!found || distance < bestDistance) {
            found = true;
            bestDistance = distance;
            bestByte = at;
        }
        if (at >= text_.size()) break;
    }
    return bestByte;
}

void TextField::eraseSelection() {
    const Selection selected = selection();
    if (selected.begin == selected.end) return;
    text_.erase(selected.begin, selected.end - selected.begin);
    caret_ = anchor_ = selected.begin;
}

void TextField::notifyTextChanged() {
    prefixWidths_.clear();
    if (onTextChanged_) onTextChanged_(text_);
}

bool TextField::onMouse(const PointerEvent& event) {
    const bool inside = bounds().contains(event.position);
    switch (event.type) {
    case PointerEventType::Down: {
        if (!inside) return false;
        // Always report focus intent; the host decides what to do with it.
        if (onFocusRequested_) onFocusRequested_();
        const std::size_t boundary = boundaryForClickX(event.position.x);
        if (event.modifiers.shift) {
            caret_ = boundary; // extend: the anchor stays where it was
        } else {
            caret_ = boundary;
            anchor_ = boundary;
        }
        pressed_ = true;
        event.accepted = true;
        invalidate();
        return true;
    }

    case PointerEventType::Move: {
        if (!pressed_) return false;
        if (event.button != 1) {
            // The matching Up was lost elsewhere; stop dragging.
            pressed_ = false;
            return false;
        }
        caret_ = boundaryForClickX(event.position.x);
        event.accepted = true;
        invalidate();
        return true;
    }

    case PointerEventType::Up: {
        if (!pressed_) return false;
        pressed_ = false;
        event.accepted = true;
        invalidate();
        return true;
    }

    case PointerEventType::Entered:
    case PointerEventType::Exited:
    case PointerEventType::Scroll:
        return false;
    }
    return false;
}

bool TextField::onKey(const KeyEvent& event) {
    if (!isFocused()) return false;

    if (event.key == Key::Enter) {
        if (onEnter_) onEnter_();
        event.accepted = true;
        return true;
    }
    if (event.key == Key::Escape) {
        if (onEscape_) onEscape_();
        event.accepted = true;
        return true;
    }

    const bool commandHeld = event.modifiers.command || event.modifiers.control;

    if (commandHeld && (event.text == "a" || event.text == "A")) {
        anchor_ = 0;
        caret_ = text_.size();
        event.accepted = true;
        invalidate();
        return true;
    }

    // Any other command/control combination is an application shortcut:
    // pass it through unconsumed.
    if (commandHeld) return false;

    switch (event.key) {
    case Key::Left: {
        caret_ = previousBoundary(text_, caret_);
        if (!event.modifiers.shift) anchor_ = caret_;
        break;
    }
    case Key::Right: {
        caret_ = nextBoundary(text_, caret_);
        if (!event.modifiers.shift) anchor_ = caret_;
        break;
    }
    case Key::Home: {
        caret_ = 0;
        if (!event.modifiers.shift) anchor_ = caret_;
        break;
    }
    case Key::End: {
        caret_ = text_.size();
        if (!event.modifiers.shift) anchor_ = caret_;
        break;
    }
    case Key::Backspace: {
        const Selection selected = selection();
        bool edited = false;
        if (selected.begin != selected.end) {
            eraseSelection();
            edited = true;
        } else if (caret_ > 0) {
            const std::size_t start = previousBoundary(text_, caret_);
            text_.erase(start, caret_ - start); // whole code points only
            caret_ = anchor_ = start;
            edited = true;
        }
        if (edited) notifyTextChanged(); // a no-op is not an edit
        break;
    }
    case Key::Delete: {
        const Selection selected = selection();
        bool edited = false;
        if (selected.begin != selected.end) {
            eraseSelection();
            edited = true;
        } else if (caret_ < text_.size()) {
            const std::size_t end = nextBoundary(text_, caret_);
            text_.erase(caret_, end - caret_); // whole code points only
            anchor_ = caret_;
            edited = true;
        }
        if (edited) notifyTextChanged(); // a no-op is not an edit
        break;
    }
    case Key::Space:
    case Key::Plus:
    case Key::Minus:
    case Key::Character: {
        // Space, '+' and '-' have dedicated keys (the viewport zooms on
        // them); the platform fills event.text with the typed character, and
        // a platform that does not gets the plain character here.
        std::string typed = event.text;
        if (typed.empty()) {
            if (event.key == Key::Space) typed = " ";
            else if (event.key == Key::Minus) typed = "-";
            else if (event.key == Key::Plus) typed = event.modifiers.shift ? "+" : "=";
            else return false;
        }
        eraseSelection();
        text_.insert(caret_, typed); // may be multi-byte UTF-8
        caret_ += typed.size();
        anchor_ = caret_;
        notifyTextChanged();
        break;
    }
    default:
        // Tab, arrows we do not handle, etc. — not ours.
        return false;
    }

    event.accepted = true;
    invalidate();
    return true;
}

void TextField::paintSelf(PaintContext& context) const {
    const core::Rect rect = bounds();
    rebuildPrefixWidths(context);
    updateHorizontalScroll(rect);

    // Displayed text: the real text, or the echo character repeated per code
    // point for password fields. Caret/selection math stays in REAL text
    // coordinates (prefixWidths_ is built from text_).
    std::string displayText = text_;
    if (echoCharacter_ != 0 && !text_.empty()) {
        displayText.clear();
        std::size_t at = 0;
        while (at < text_.size()) {
            utf8::appendCodePoint(displayText, echoCharacter_);
            at = nextBoundary(text_, at);
        }
    }

    const bool focused = isFocused();
    context.fillRoundedRect(rect, colors().fieldBackground, kCornerRadius);
    strokeRoundedRect(context, rect, colors().fieldBorder, kStrokeWidth, kCornerRadius);
    if (focused) drawFocusRing(context, rect, kCornerRadius);
    if (leadingIcon_.has_value()) {
        const double side = 14.0;
        drawIcon(context, *leadingIcon_,
                 core::Rect{kHorizontalPadding + 1.0, (rect.size.height - side) / 2.0, side, side}, colors().textSecondary);
    }

    context.pushClip(rect);
    const double textOriginX = leadingPadding() - horizontalScroll_;
    if (!text_.empty()) {
        const Selection selected = selection();
        if (selected.begin != selected.end) {
            const double selLeft = textOriginX + prefixWidthUpTo(selected.begin);
            const double selRight = textOriginX + prefixWidthUpTo(selected.end);
            const core::Rect selectionRect{
                core::Point{selLeft, rect.minY()},
                core::Size{std::max(0.0, selRight - selLeft), rect.size.height}};
            context.fillRect(selectionRect, colors().selectionHighlight);
        }
        const core::Rect textRect{core::Point{textOriginX, rect.minY()},
                                  core::Size{rect.size.width, rect.size.height}};
        context.drawText(displayText, textRect, kTextFont, colors().textPrimary, TextAlign::Left);
    } else if (!focused && !placeholder_.empty()) {
        const core::Rect textRect{core::Point{textOriginX, rect.minY()},
                                  core::Size{rect.size.width, rect.size.height}};
        context.drawText(placeholder_, textRect, kTextFont, colors().textSecondary, TextAlign::Left);
    }
    if (focused) {
        const double caretX = textOriginX + prefixWidthUpTo(caret_);
        context.drawLine(core::Point{caretX, rect.minY()}, core::Point{caretX, rect.maxY()},
                         colors().textPrimary, kCaretWidth);
    }
    context.popClip();
}

} // namespace rivet::ui

namespace rivet::ui {

void TextField::setLeadingIcon(std::optional<Icon> icon) {
    if (leadingIcon_ == icon) return;
    leadingIcon_ = icon;
    invalidate();
}

} // namespace rivet::ui
