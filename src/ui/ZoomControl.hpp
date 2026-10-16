// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Widget.hpp"

#include <functional>
#include <string>

namespace rivet::ui {

// Toolbar zoom group (ADR-0033 §4): [−] [percent ▾] [+]. The minus and plus
// parts step the zoom; the percent part opens the owner's preset popover
// (onPercent receives the part's rect in this widget's coordinates).
// Borderless parts with hover/pressed fills on a quiet track.
class ZoomControl final : public Widget {
public:
    enum class Part { None, ZoomOut, Percent, ZoomIn };

    static constexpr double kStepWidth = 28.0;
    static constexpr double kPercentWidth = 62.0;
    static constexpr double kWidth = 2.0 * kStepWidth + kPercentWidth;

    ZoomControl();

    void setPercentText(std::string text);
    const std::string& percentText() const { return percent_; }
    void setOnZoomOut(std::function<void()> callback) { onZoomOut_ = std::move(callback); }
    void setOnZoomIn(std::function<void()> callback) { onZoomIn_ = std::move(callback); }
    void setOnPercent(std::function<void(const core::Rect&)> callback) { onPercent_ = std::move(callback); }
    void setEnabled(bool enabled);
    bool enabled() const { return enabled_; }

    core::Rect partRect(Part part) const;
    Part partAt(core::Point local) const;
    void trigger(Part part);

    core::Size preferredSize(const PaintContext&) const override;
    bool onMouse(const PointerEvent& event) override;
    void paint(PaintContext& context) const override { paintSelf(context); }
    void paintSelf(PaintContext& context) const override;

    int keyboardStopCount() const override { return enabled_ ? 3 : 0; }
    void setKeyboardStop(int stop) override;
    void activateKeyboardStop(int stop) override;

private:
    std::string percent_ = "100%";
    bool enabled_ = true;
    Part hovered_ = Part::None;
    Part pressed_ = Part::None;
    int keyboardStop_ = -1;
    std::function<void()> onZoomOut_;
    std::function<void()> onZoomIn_;
    std::function<void(const core::Rect&)> onPercent_;
};

} // namespace rivet::ui
