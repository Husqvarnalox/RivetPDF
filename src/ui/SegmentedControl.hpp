// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/Icons.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::ui {

// Segmented control (ADR-0033 §4): a quiet track with one raised selected
// segment — modes (Rendered/Source/Split), the sidebar panels, the tool
// groups. Segments show a text label or an icon. Clicking an enabled
// segment fires onSelect(index); the owner decides the selection (it may be
// none). Disabled segments draw in textDisabled.
class SegmentedControl : public Widget {
public:
    struct Segment {
        std::string label;
        std::optional<Icon> icon; // drawn instead of the label when set
        std::string tooltip;
        double width = 0.0;       // 0 = estimated from the label
        bool enabled = true;
    };

    static constexpr double kCornerRadius = 6.0;

    explicit SegmentedControl(std::vector<Segment> segments, bool small = false);

    void setOnSelect(std::function<void(std::size_t)> onSelect) { onSelect_ = std::move(onSelect); }
    void setSelected(std::optional<std::size_t> index);
    std::optional<std::size_t> selected() const { return selected_; }
    void setSegmentEnabled(std::size_t index, bool enabled);
    void setSegmentLabel(std::size_t index, std::string label);
    std::size_t segmentCount() const { return segments_.size(); }
    const Segment& segment(std::size_t index) const { return segments_[index]; }

    // Sum of the segment widths, control height (28, or 22 when small).
    core::Size naturalSize() const;
    core::Size preferredSize(const PaintContext&) const override { return naturalSize(); }
    // Segment rect in local coordinates (widths scale to the frame).
    core::Rect segmentRect(std::size_t index) const;
    std::optional<std::size_t> segmentAt(core::Point local) const;

    bool onMouse(const PointerEvent& event) override;
    void paint(PaintContext& context) const override { paintSelf(context); }
    void paintSelf(PaintContext& context) const override;

    int keyboardStopCount() const override { return static_cast<int>(segments_.size()); }
    void setKeyboardStop(int stop) override;
    void activateKeyboardStop(int stop) override;

private:
    double segmentWidth(std::size_t index) const;

    std::vector<Segment> segments_;
    bool small_;
    std::optional<std::size_t> selected_;
    std::optional<std::size_t> hovered_;
    std::optional<std::size_t> pressed_;
    int keyboardStop_ = -1;
    std::function<void(std::size_t)> onSelect_;
};

} // namespace rivet::ui
