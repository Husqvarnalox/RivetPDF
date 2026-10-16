// SPDX-License-Identifier: MPL-2.0
#include "ui/Tooltip.hpp"

#include "ui/Theme.hpp"

#include <algorithm>
#include <chrono>

namespace rivet::ui {
namespace {

constexpr double kPaddingX = 8.0;
constexpr double kHeight = 22.0;
constexpr double kGap = 6.0;
constexpr double kPerChar = 6.3; // 11 px UI font estimate (layout has no paint context)
constexpr core::Rect kHidden{-1.0, -1.0, 0.0, 0.0};

double estimateWidth(const std::string& text) {
    double count = 0.0;
    for (const char c : text) count += ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ? 1.0 : 0.0;
    return count * kPerChar + 2.0 * kPaddingX;
}

} // namespace

void Tooltip::hoverTarget(const Widget* target, const std::string& text, const core::Rect& anchor,
                          const core::Rect& area) {
    if (target == target_ && text == text_) return;
    dismiss();
    target_ = target;
    text_ = text;
    anchor_ = anchor;
    area_ = area;
    if (target_ == nullptr || text_.empty() || runner_ == nullptr) return;
    const std::uint64_t token = token_;
    runner_->runAfter(std::chrono::milliseconds(static_cast<long long>(metrics::kTooltipDelaySeconds * 1000.0)),
                      [this, token] {
                          if (token == token_ && target_ != nullptr) showNow();
                      });
}

void Tooltip::dismiss() {
    ++token_; // a pending show no longer applies
    if (visible_) {
        visible_ = false;
        setFrame(kHidden);
        invalidate();
    }
    target_ = nullptr;
    text_.clear();
}

void Tooltip::showNow() {
    if (text_.empty()) return;
    const double width = std::min(estimateWidth(text_), std::max(0.0, area_.size.width - 8.0));
    double x = anchor_.minX() + (anchor_.size.width - width) / 2.0;
    x = std::clamp(x, area_.minX() + 4.0, std::max(area_.minX() + 4.0, area_.maxX() - 4.0 - width));
    double y = anchor_.maxY() + kGap;
    if (y + kHeight > area_.maxY() - 4.0) y = anchor_.minY() - kGap - kHeight;
    visible_ = true;
    shownAt_ = animationNow();
    setFrame(core::Rect{x, y, width, kHeight});
    invalidate();
}

void Tooltip::paintSelf(PaintContext& context) const {
    if (!visible_) return;
    const Palette& palette = colors();
    const double t = fadeProgress(shownAt_, metrics::kHoverFadeSeconds);
    const core::Rect r = bounds();
    const Color background = palette.tooltipBackground;
    context.fillRoundedRect(r, withAlpha(background, background.a * t), metrics::kRadiusSmall + 1.0);
    const Color text = palette.tooltipText;
    context.drawText(text_, r, fonts::kSmall, withAlpha(text, text.a * t), TextAlign::Center);
    if (t < 1.0) invalidate(); // next fade frame
}

} // namespace rivet::ui
