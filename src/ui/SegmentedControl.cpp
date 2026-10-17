// SPDX-License-Identifier: MPL-2.0
#include "ui/SegmentedControl.hpp"

#include "core/geometry/Insets.hpp"
#include "ui/Drawing.hpp"
#include "ui/Theme.hpp"

#include <algorithm>
#include <utility>

namespace rivet::ui {
namespace {

constexpr double kTrackInset = 2.0;
constexpr double kSegmentPadding = 14.0;
constexpr double kIconSegmentWidth = 34.0;

} // namespace

SegmentedControl::SegmentedControl(std::vector<Segment> segments, bool small)
    : segments_(std::move(segments)), small_(small) {}

double SegmentedControl::segmentWidth(std::size_t index) const {
    const Segment& s = segments_[index];
    if (s.width > 0.0) return s.width;
    if (s.icon.has_value()) return kIconSegmentWidth;
    // Layout runs without a paint context: a conservative per-character
    // estimate of the system UI font keeps labels inside their segment.
    const double perChar = small_ ? 6.6 : 7.4;
    return static_cast<double>(s.label.size()) * perChar + 2.0 * kSegmentPadding;
}

core::Size SegmentedControl::naturalSize() const {
    double width = 2.0 * kTrackInset;
    for (std::size_t i = 0; i < segments_.size(); ++i) width += segmentWidth(i);
    return core::Size{width, small_ ? metrics::kSmallControlHeight : metrics::kControlHeight};
}

core::Rect SegmentedControl::segmentRect(std::size_t index) const {
    const core::Size natural = naturalSize();
    const double available = std::max(0.0, frame().size.width - 2.0 * kTrackInset);
    const double scale = natural.width > 2.0 * kTrackInset ? available / (natural.width - 2.0 * kTrackInset) : 1.0;
    double x = kTrackInset;
    for (std::size_t i = 0; i < index; ++i) x += segmentWidth(i) * scale;
    return core::Rect{x, kTrackInset, segmentWidth(index) * scale,
                      std::max(0.0, frame().size.height - 2.0 * kTrackInset)};
}

std::optional<std::size_t> SegmentedControl::segmentAt(core::Point local) const {
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const core::Rect r = segmentRect(i);
        if (local.x >= r.minX() && local.x < r.maxX() && local.y >= 0.0 && local.y < frame().size.height) return i;
    }
    return std::nullopt;
}

void SegmentedControl::setSelected(std::optional<std::size_t> index) {
    if (index && *index >= segments_.size()) index.reset();
    if (selected_ == index) return;
    selected_ = index;
    invalidate();
}

void SegmentedControl::setSegmentEnabled(std::size_t index, bool enabled) {
    if (index >= segments_.size() || segments_[index].enabled == enabled) return;
    segments_[index].enabled = enabled;
    invalidate();
}

void SegmentedControl::setSegmentLabel(std::size_t index, std::string label) {
    if (index >= segments_.size() || segments_[index].label == label) return;
    segments_[index].label = std::move(label);
    invalidate();
}

bool SegmentedControl::onMouse(const PointerEvent& event) {
    const std::optional<std::size_t> at =
        bounds().contains(event.position) ? segmentAt(event.position) : std::nullopt;
    switch (event.type) {
    case PointerEventType::Down:
        if (!at) return false;
        pressed_ = at;
        break;
    case PointerEventType::Up: {
        if (!pressed_) return false;
        const std::size_t index = *pressed_;
        pressed_.reset();
        if (at == index && segments_[index].enabled && onSelect_) onSelect_(index);
        break;
    }
    case PointerEventType::Move:
    case PointerEventType::Entered:
    case PointerEventType::Exited: {
        const std::optional<std::size_t> next = event.type == PointerEventType::Exited ? std::nullopt : at;
        if (pressed_ && event.button == 0) pressed_.reset();
        if (next == hovered_ && !pressed_) return at.has_value();
        hovered_ = next;
        // The tooltip follows the hovered segment.
        setTooltip(hovered_ ? segments_[*hovered_].tooltip : std::string{});
        break;
    }
    case PointerEventType::Scroll: return false;
    }
    event.accepted = true;
    invalidate();
    return true;
}

void SegmentedControl::setKeyboardStop(int stop) {
    if (keyboardStop_ == stop) return;
    keyboardStop_ = stop;
    setFocused(stop >= 0);
    invalidate();
}

void SegmentedControl::activateKeyboardStop(int stop) {
    if (stop < 0 || static_cast<std::size_t>(stop) >= segments_.size()) return;
    const auto index = static_cast<std::size_t>(stop);
    if (segments_[index].enabled && onSelect_) onSelect_(index);
}

void SegmentedControl::paintSelf(PaintContext& context) const {
    const Palette& palette = colors();
    const core::Rect track = bounds();
    context.fillRoundedRect(track, palette.controlFill, kCornerRadius);
    const Font font = small_ ? fonts::kSmall : fonts::kBody;
    for (std::size_t i = 0; i < segments_.size(); ++i) {
        const core::Rect r = segmentRect(i);
        const bool selected = selected_ == i;
        const Segment& s = segments_[i];
        if (selected) {
            // Raised segment: a soft shadow line under a filled pill.
            context.fillRoundedRect(core::Rect{r.minX(), r.minY() + 0.5, r.size.width, r.size.height},
                                    palette.pageShadow, kCornerRadius - kTrackInset);
            context.fillRoundedRect(r, palette.controlSelectedFill, kCornerRadius - kTrackInset);
        } else if (s.enabled && pressed_ == i) {
            context.fillRoundedRect(r, palette.controlFillPressed, kCornerRadius - kTrackInset);
        } else if (s.enabled && hovered_ == i) {
            context.fillRoundedRect(r, palette.controlFillHover, kCornerRadius - kTrackInset);
        } else if (i > 0 && selected_ != i - 1) {
            // Hairline divider between two unselected segments.
            const double inset = r.size.height * 0.25;
            context.fillRect(core::Rect{r.minX() - 0.5, r.minY() + inset, 1.0, r.size.height - 2.0 * inset},
                             palette.controlBorder);
        }
        const Color color = !s.enabled ? palette.textDisabled : (selected ? palette.textPrimary : palette.textSecondary);
        if (s.icon.has_value()) {
            const double side = small_ ? 16.0 : 18.0;
            drawIcon(context, *s.icon,
                     core::Rect{r.minX() + (r.size.width - side) / 2.0, r.minY() + (r.size.height - side) / 2.0, side,
                                side},
                     color);
        } else {
            context.drawText(s.label, r, selected ? Font{font.size, Font::Weight::Semibold} : font, color,
                             TextAlign::Center);
        }
        if (keyboardStop_ == static_cast<int>(i)) drawFocusRing(context, r, kCornerRadius - kTrackInset);
    }
}

} // namespace rivet::ui
