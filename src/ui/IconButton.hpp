// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Animation.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"
#include "ui/Widget.hpp"

#include <functional>
#include <optional>
#include <string>

namespace rivet::ui {

// Borderless toolbar button showing an icon (ADR-0033 §4): no fill at rest,
// controlFillHover on hover (120 ms fade), controlFillPressed while pressed,
// the icon in accent with an accentSubtle tint when active, textDisabled
// when disabled (shown, never hidden; a disabled button never clicks). An
// optional text label follows the icon ("Open…" style buttons). The
// tooltip (title + shortcut) is shown by the shell's tooltip controller.
class IconButton : public Widget {
public:
    static constexpr double kSize = 28.0;      // square hit target
    static constexpr double kIconSide = 18.0;  // drawn icon size
    static constexpr double kCornerRadius = 6.0;

    explicit IconButton(Icon icon, std::string tooltip = {});

    void setIcon(Icon icon);
    Icon icon() const { return icon_; }
    // Optional text after the icon (the button grows by its measured width).
    void setText(std::string text);
    const std::string& text() const { return text_; }

    void setOnClick(std::function<void()> onClick) { onClick_ = std::move(onClick); }
    void setActive(bool active);
    bool active() const { return active_; }
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }
    bool hovered() const { return hovered_; }
    bool pressed() const { return armed_ && hovered_; }

    // Keyboard activation (the toolbar's Space/Enter).
    void activate();
    int keyboardStopCount() const override { return enabled_ ? 1 : 0; }
    void setKeyboardStop(int stop) override { setFocused(stop == 0); }
    void activateKeyboardStop(int) override { activate(); }

    core::Size preferredSize(const PaintContext& context) const override;
    bool onMouse(const PointerEvent& event) override;
    void paint(PaintContext& context) const override { paintSelf(context); }
    void paintSelf(PaintContext& context) const override;

private:
    Icon icon_;
    std::string text_;
    std::function<void()> onClick_;
    bool armed_ = false;
    bool hovered_ = false;
    bool active_ = false;
    bool enabled_ = true;
    Fade hoverFade_{metrics::kHoverFadeSeconds};

};

} // namespace rivet::ui
