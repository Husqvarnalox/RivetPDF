// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared doubles for the source editor tests (ui and app level).

#include "ui/SourceEditor.hpp"
#include "ui/UiTypes.hpp"

#include <string>
#include <utility>
#include <vector>

namespace rivet::ui::testing {

struct RecordedEdit {
    std::size_t offset = 0;
    std::size_t removeLength = 0;
    std::string removed;
    std::string insert;
    SourceEditKind kind = SourceEditKind::Other;
    bool merge = false;
};

class RecordingSink final : public ISourceEditSink {
public:
    std::vector<RecordedEdit> edits;
    bool accept = true;

    bool applyEdit(const SourceEdit& edit) override {
        if (!accept) return false;
        edits.push_back(RecordedEdit{edit.offset, edit.removeLength, std::string(edit.removed),
                                     std::string(edit.insert), edit.kind, edit.mergeWithPrevious});
        return true;
    }
};

class FakeTextClipboard final : public ITextClipboard {
public:
    std::string content;
    bool failSet = false;

    std::string text() const override { return content; }
    bool setText(const std::string& text) override {
        if (failSet) return false;
        content = text;
        return true;
    }
};

inline KeyEvent key(Key k, ModifierFlags modifiers = {}, std::string text = {}) {
    KeyEvent event;
    event.key = k;
    event.text = std::move(text);
    event.modifiers = modifiers;
    return event;
}

inline ModifierFlags mods(bool shift = false, bool option = false, bool command = false) {
    ModifierFlags m;
    m.shift = shift;
    m.option = option;
    m.command = command;
    return m;
}

inline KeyEvent chr(std::string text, ModifierFlags modifiers = {}) {
    return key(Key::Character, modifiers, std::move(text));
}

// Types `text` one code point at a time as Character events.
inline void typeString(Widget& widget, const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        std::size_t next = i + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xC0) == 0x80) ++next;
        const std::string piece = text.substr(i, next - i);
        if (piece == " ") {
            widget.onKey(key(Key::Space, {}, " "));
        } else if (piece == "\n") {
            widget.onKey(key(Key::Enter));
        } else {
            widget.onKey(chr(piece));
        }
        i = next;
    }
}

inline PointerEvent pointer(PointerEventType type, double x, double y, int button = 1, bool shift = false) {
    PointerEvent event;
    event.type = type;
    event.position = core::Point{x, y};
    event.button = button;
    event.modifiers.shift = shift;
    return event;
}

} // namespace rivet::ui::testing
