#pragma once

#include "ui/UiTypes.hpp"
#include "ui/Widget.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace rivet::ui {

// Rounded push button (text, or a color swatch). Tracks hover/press state,
// consumes the mouse events it handles, and fires its callback on mouse-up
// inside the button after a press that started inside (drag out cancels the
// pending click). Colors are theme tokens (ADR-0033): a quiet controlFill at
// rest, stronger fills on hover/press, accentSubtle + accentText when active,
// textDisabled when disabled (a disabled button never clicks).
class Button : public Widget {
public:
    static constexpr double kHorizontalPadding = 12.0;
    static constexpr double kVerticalPadding = 5.0;
    static constexpr double kMinHeight = 22.0;
    static constexpr double kCornerRadius = 6.0;

    explicit Button(std::string label);

    void setLabel(std::string label);
    const std::string& label() const { return label_; }

    void setOnClick(std::function<void()> onClick);
    bool hasOnClick() const { return onClick_ != nullptr; }

    // Toggle look: an active button is drawn with the accent tint (tool
    // strips mark the current choice with it). Purely visual; the click
    // callback is unaffected.
    void setActive(bool active);
    bool active() const { return active_; }
    // Disabled buttons draw in textDisabled and ignore clicks.
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    // A color chip drawn instead of the label text (palette swatches). The
    // swatch color is document data (an annotation color), not a token.
    void setSwatch(std::optional<Color> swatch);
    const std::optional<Color>& swatch() const { return swatch_; }
    // A primary button (the welcome view's "Open…"): accent fill.
    void setPrimary(bool primary);
    void setFont(const Font& font);

    // Text extents plus padding; height never below kMinHeight.
    core::Size preferredSize(const PaintContext& context) const override;

    bool onMouse(const PointerEvent& event) override;

    // Keyboard activation (Space/Enter while focused by the toolbar).
    void activate();
    bool hovered() const { return hovered_; }

    // Leaf widget: paints itself only (children would be empty anyway).
    void paint(PaintContext& context) const override;
    void paintSelf(PaintContext& context) const override;

private:
    enum class VisualState : std::uint8_t { Normal, Hovered, Pressed };

    VisualState visualState() const;
    void fireClick();

    std::string label_;
    std::function<void()> onClick_;
    bool armed_ = false;   // mouse went down inside; the matching up may click
    bool hovered_ = false; // pointer is currently inside the button
    bool active_ = false;
    bool enabled_ = true;
    bool primary_ = false;
    std::optional<Color> swatch_;
    Font font_;
};

} // namespace rivet::ui
