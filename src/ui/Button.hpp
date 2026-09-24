#pragma once

#include "ui/Widget.hpp"

#include <functional>
#include <string>

namespace rivet::ui {

// Rounded push button. Tracks hover/press state, consumes the mouse events
// it handles, and fires its callback on mouse-up inside the button after a
// press that started inside (drag out cancels the pending click).
class Button : public Widget {
public:
    static constexpr double kHorizontalPadding = 14.0;
    static constexpr double kVerticalPadding = 7.0;
    static constexpr double kMinHeight = 24.0;
    static constexpr double kCornerRadius = 6.0;

    explicit Button(std::string label);

    void setLabel(std::string label);
    const std::string& label() const { return label_; }

    void setOnClick(std::function<void()> onClick);
    bool hasOnClick() const { return onClick_ != nullptr; }

    // Text extents plus padding; height never below kMinHeight.
    core::Size preferredSize(const PaintContext& context) const override;

    bool onMouse(const PointerEvent& event) override;

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
};

} // namespace rivet::ui
