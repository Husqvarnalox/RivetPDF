// SPDX-License-Identifier: MPL-2.0
#include "ui/ZoomControl.hpp"

#include "ui/Drawing.hpp"
#include "ui/Icons.hpp"
#include "ui/Theme.hpp"

#include <utility>

namespace rivet::ui {

ZoomControl::ZoomControl() { setTooltip("Zoom"); }

void ZoomControl::setPercentText(std::string text) {
    if (percent_ == text) return;
    percent_ = std::move(text);
    invalidate();
}

void ZoomControl::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    hovered_ = Part::None;
    pressed_ = Part::None;
    invalidate();
}

core::Size ZoomControl::preferredSize(const PaintContext&) const {
    return core::Size{kWidth, metrics::kControlHeight};
}

core::Rect ZoomControl::partRect(Part part) const {
    const double h = frame().size.height;
    switch (part) {
    case Part::ZoomOut: return core::Rect{0.0, 0.0, kStepWidth, h};
    case Part::Percent: return core::Rect{kStepWidth, 0.0, kPercentWidth, h};
    case Part::ZoomIn: return core::Rect{kStepWidth + kPercentWidth, 0.0, kStepWidth, h};
    case Part::None: break;
    }
    return core::Rect{};
}

ZoomControl::Part ZoomControl::partAt(core::Point local) const {
    for (const Part part : {Part::ZoomOut, Part::Percent, Part::ZoomIn}) {
        if (partRect(part).contains(local)) return part;
    }
    return Part::None;
}

void ZoomControl::trigger(Part part) {
    if (!enabled_) return;
    switch (part) {
    case Part::ZoomOut:
        if (onZoomOut_) onZoomOut_();
        break;
    case Part::ZoomIn:
        if (onZoomIn_) onZoomIn_();
        break;
    case Part::Percent:
        if (onPercent_) onPercent_(partRect(Part::Percent));
        break;
    case Part::None: break;
    }
}

bool ZoomControl::onMouse(const PointerEvent& event) {
    const Part at = bounds().contains(event.position) ? partAt(event.position) : Part::None;
    switch (event.type) {
    case PointerEventType::Down:
        if (at == Part::None) return false;
        pressed_ = enabled_ ? at : Part::None;
        break;
    case PointerEventType::Up: {
        const Part was = pressed_;
        pressed_ = Part::None;
        if (was == Part::None) return at != Part::None;
        if (was == at) trigger(at);
        break;
    }
    case PointerEventType::Move:
    case PointerEventType::Entered:
    case PointerEventType::Exited: {
        const Part next = event.type == PointerEventType::Exited || !enabled_ ? Part::None : at;
        if (pressed_ != Part::None && event.button == 0) pressed_ = Part::None;
        if (next == hovered_) return at != Part::None;
        hovered_ = next;
        setTooltip(hovered_ == Part::ZoomOut ? "Zoom Out" : hovered_ == Part::ZoomIn ? "Zoom In" : "Zoom presets");
        break;
    }
    case PointerEventType::Scroll: return false;
    }
    event.accepted = true;
    invalidate();
    return true;
}

void ZoomControl::setKeyboardStop(int stop) {
    if (keyboardStop_ == stop) return;
    keyboardStop_ = stop;
    setFocused(stop >= 0);
    invalidate();
}

void ZoomControl::activateKeyboardStop(int stop) {
    trigger(stop == 0 ? Part::ZoomOut : stop == 1 ? Part::Percent : Part::ZoomIn);
}

void ZoomControl::paintSelf(PaintContext& context) const {
    const Palette& palette = colors();
    context.fillRoundedRect(bounds(), palette.controlFill, metrics::kRadiusControl);
    const Color content = enabled_ ? palette.textPrimary : palette.textDisabled;
    const Part parts[] = {Part::ZoomOut, Part::Percent, Part::ZoomIn};
    for (int i = 0; i < 3; ++i) {
        const Part part = parts[i];
        const core::Rect r = partRect(part);
        if (enabled_ && pressed_ == part && hovered_ == part) {
            context.fillRoundedRect(r, palette.controlFillPressed, metrics::kRadiusControl);
        } else if (enabled_ && hovered_ == part) {
            context.fillRoundedRect(r, palette.controlFillHover, metrics::kRadiusControl);
        }
        if (part == Part::Percent) {
            context.drawText(percent_, core::Rect{r.minX(), r.minY(), r.size.width - 10.0, r.size.height}, fonts::kBody,
                             content, TextAlign::Center);
            drawIcon(context, Icon::ChevronDown, core::Rect{r.maxX() - 15.0, r.minY() + (r.size.height - 10.0) / 2.0, 10.0, 10.0},
                     enabled_ ? palette.textSecondary : palette.textDisabled);
        } else {
            const double side = 16.0;
            drawIcon(context, part == Part::ZoomOut ? Icon::ZoomOut : Icon::ZoomIn,
                     core::Rect{r.minX() + (r.size.width - side) / 2.0, r.minY() + (r.size.height - side) / 2.0, side, side},
                     content);
        }
        if (keyboardStop_ == i) drawFocusRing(context, r, metrics::kRadiusControl);
    }
}

} // namespace rivet::ui
