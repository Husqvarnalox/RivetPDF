// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Animation.hpp"
#include "ui/Widget.hpp"

#include <cstdint>
#include <string>

namespace rivet::ui {

// Tooltip bubble + its timing (ADR-0033 §4/§7): hovering a widget that has
// tooltip() text for 600 ms shows the text in a small bubble under it,
// fading in over 120 ms; moving to another widget, pressing or leaving
// hides it at once. The widget's frame is the bubble (empty while hidden);
// it never takes input. The owner forwards hover changes; the delay runs on
// an IDelayedRunner. Main thread only.
class Tooltip final : public Widget {
public:
    explicit Tooltip(IDelayedRunner* runner) : runner_(runner) {}

    // The widget under the pointer changed (nullptr = none). `anchor` is the
    // widget's rect in the parent's (root) coordinates; `area` the parent's
    // bounds.
    void hoverTarget(const Widget* target, const std::string& text, const core::Rect& anchor, const core::Rect& area);
    // Hides immediately (press, key, scroll) and cancels a pending show.
    void dismiss();

    bool visible() const { return visible_; }
    const std::string& text() const { return text_; }
    const Widget* target() const { return target_; }
    // Shows now without the delay (tests, and the runner's callback).
    void showNow();

    bool onMouse(const PointerEvent&) override { return false; }
    void paint(PaintContext& context) const override { paintSelf(context); }
    void paintSelf(PaintContext& context) const override;

private:
    IDelayedRunner* runner_;
    const Widget* target_ = nullptr;
    std::string text_;
    core::Rect anchor_;
    core::Rect area_;
    bool visible_ = false;
    std::uint64_t token_ = 0;
    double shownAt_ = 0.0;
};

} // namespace rivet::ui
