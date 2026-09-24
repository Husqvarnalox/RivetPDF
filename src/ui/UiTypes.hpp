#pragma once

#include "core/geometry/Point.hpp"

#include <cstdint>
#include <string>

namespace rivet::ui {

// RGBA color, channels 0..1, straight (non-premultiplied) alpha.
struct Color {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    double a = 1.0;

    static constexpr Color rgba(double r, double g, double b, double a) { return Color{r, g, b, a}; }
    static constexpr Color white() { return {1.0, 1.0, 1.0, 1.0}; }
    static constexpr Color black() { return {0.0, 0.0, 0.0, 1.0}; }
    static constexpr Color gray(double v) { return {v, v, v, 1.0}; }

    bool operator==(const Color&) const = default;
};

enum class TextAlign : std::uint8_t { Left, Center, Right };

struct Font {
    double size = 13.0;

    enum class Weight : std::uint8_t { Regular, Semibold, Bold };
    Weight weight = Weight::Regular;

    bool operator==(const Font&) const = default;
};

struct ModifierFlags {
    bool shift = false;
    bool control = false;
    bool option = false;
    bool command = false;

    bool any() const { return shift || control || option || command; }
    bool operator==(const ModifierFlags&) const = default;
};

enum class PointerEventType : std::uint8_t { Move, Down, Up, Entered, Exited, Scroll };

struct PointerEvent {
    PointerEventType type = PointerEventType::Move;
    core::Point position;    // widget-local logical coordinates
    int button = 0;          // 0 none, 1 left, 2 right (for Move: the held button)
    core::Point scrollDelta; // for Scroll: logical pixels to scroll
                             // (positive = content moves up/left)
    ModifierFlags modifiers;

    // Set by handlers to mark consumption. Mutable so a handler receiving
    // the event by const reference can flag it; routers may inspect the flag
    // on their copy after dispatch. The bool return value of onMouse/onKey
    // remains the primary consumption signal.
    mutable bool accepted = false;
};

enum class Key : std::uint8_t {
    Unknown,
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Home,
    End,
    Escape,
    Enter,
    Tab,
    Backspace,
    Delete,
    Space,
    Plus,
    Minus,
    Character,
};

struct KeyEvent {
    Key key = Key::Unknown;
    std::string text; // UTF-8, only meaningful for Key::Character
    ModifierFlags modifiers;

    // See PointerEvent::accepted.
    mutable bool accepted = false;
};

} // namespace rivet::ui
