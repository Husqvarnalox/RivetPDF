// SPDX-License-Identifier: MPL-2.0
#include "app/AnnotationBarController.hpp"

#include "app/AnnotationTools.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kButtonHeight = 26.0;
constexpr double kSwatchSize = 24.0;
constexpr double kPadding = 10.0;
constexpr double kGroupGap = 14.0;

double toolButtonWidth(AnnotationTool tool) {
    switch (tool) {
    case AnnotationTool::Highlight: return 72.0;
    case AnnotationTool::Underline: return 74.0;
    case AnnotationTool::Ellipse: return 62.0;
    case AnnotationTool::Select: return 58.0;
    default: return 52.0;
    }
}

bool appliesColor(AnnotationTool tool) { return tool != AnnotationTool::Select; }
bool appliesOpacity(AnnotationTool tool) { return toolUsesOpacity(tool); }
bool appliesWidth(AnnotationTool tool) { return toolUsesWidth(tool); }
bool appliesFill(AnnotationTool tool) { return toolUsesFill(tool); }
bool appliesStamp(AnnotationTool tool) { return toolUsesStampName(tool); }

bool sameColor(const pdf::PdfColor& a, const pdf::PdfColor& b) { return a == b; }

ui::Color toUiColor(const pdf::PdfColor& color) {
    return ui::Color::rgba(static_cast<double>(color.r), static_cast<double>(color.g),
                           static_cast<double>(color.b), 1.0);
}

std::string percentLabel(float value) {
    return std::format("{}%", static_cast<int>(std::lround(static_cast<double>(value) * 100.0)));
}

std::string widthLabel(float value) { return std::format("{}pt", static_cast<int>(std::lround(static_cast<double>(value)))); }

// Horizontal strip with a hairline under it; the controller positions the
// items (some of them hidden by tool), so layout() is left alone.
class Strip final : public ui::Container {
public:
    void paintSelf(ui::PaintContext& context) const override {
        ui::Container::paintSelf(context);
        const core::Rect rect = bounds();
        if (rect.size.height > 0.0) {
            const double y = rect.maxY() - 0.5;
            context.drawLine(core::Point{rect.minX(), y}, core::Point{rect.maxX(), y},
                             ui::Color::rgba(0.0, 0.0, 0.0, 0.12), 1.0);
        }
    }
};

} // namespace

AnnotationBarController::AnnotationBarController(ShellContext& context, ui::Widget& parent,
                                                 AnnotationController& annotations)
    : context_(context), annotations_(annotations) {
    auto strip = std::make_unique<Strip>();
    strip_ = strip.get();
    strip_->setBackgroundColor(ui::Color::rgba(0.95, 0.95, 0.95, 1.0));
    parent.addChild(std::move(strip));

    for (std::size_t i = 0; i < kAnnotationToolCount; ++i) {
        const AnnotationTool tool = static_cast<AnnotationTool>(i);
        const double gap = (tool == AnnotationTool::Select || tool == AnnotationTool::StrikeOut) ? kGroupGap : 4.0;
        toolButtons_.push_back(addButton(
            annotationToolLabel(tool), toolButtonWidth(tool), [this, tool] { annotations_.setTool(tool); }, gap));
    }
    items_.back().gapAfter = kGroupGap;

    for (const pdf::PdfColor& color : kPresetColors) {
        ui::Button* swatch = addButton(
            "", kSwatchSize, [this, color] { annotations_.setColor(color); }, 4.0, appliesColor);
        swatch->setSwatch(toUiColor(color));
        swatches_.push_back(swatch);
    }
    items_.back().gapAfter = kGroupGap;

    for (const float opacity : kOpacitySteps) {
        opacityButtons_.push_back(addButton(
            percentLabel(opacity), 46.0, [this, opacity] { annotations_.setOpacity(opacity); }, 4.0,
            appliesOpacity));
    }
    items_.back().gapAfter = kGroupGap;

    for (const float width : kWidthSteps) {
        widthButtons_.push_back(addButton(
            widthLabel(width), 40.0, [this, width] { annotations_.setBorderWidth(width); }, 4.0,
            appliesWidth));
    }
    items_.back().gapAfter = kGroupGap;

    fillButton_ = addButton(
        "Fill", 44.0,
        [this] { annotations_.setFillEnabled(!annotations_.displayedStyle().interiorColor.has_value()); }, 4.0,
        appliesFill);
    stampButton_ = addButton(
        "", 150.0,
        [this] {
            const pdf::PdfStampName current = annotations_.displayedStampName();
            std::size_t index = 0;
            for (std::size_t i = 0; i < kStampNames.size(); ++i) {
                if (kStampNames[i] == current) index = i;
            }
            annotations_.setStampName(kStampNames[(index + 1) % kStampNames.size()]);
        },
        4.0, appliesStamp);

    annotations_.setOnStateChanged([this] { refresh(); });
    strip_->setFrame(kHiddenFrame);
    refresh();
}

ui::Button* AnnotationBarController::addButton(std::string label, double width, std::function<void()> onClick,
                                               double gapAfter, bool (*applies)(AnnotationTool)) {
    auto button = std::make_unique<ui::Button>(std::move(label));
    ui::Button* raw = button.get();
    button->setOnClick(std::move(onClick));
    button->setFrame(core::Rect{0.0, 0.0, width, kButtonHeight});
    strip_->addChild(std::move(button));
    items_.push_back(Item{raw, width, gapAfter, applies});
    return raw;
}

void AnnotationBarController::setVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    if (!visible_) annotations_.setTool(AnnotationTool::Select);
    context_.relayout();
    refresh();
    if (onVisibilityChanged_) onVisibilityChanged_();
}

void AnnotationBarController::layout(const core::Rect& frame) {
    strip_->setFrame(visible_ ? frame : kHiddenFrame);
    positionItems();
}

bool AnnotationBarController::controlShown(const ui::Button& button) const {
    return !button.frame().isEmpty();
}

void AnnotationBarController::refresh() {
    const AnnotationTool current = annotations_.tool();
    for (std::size_t i = 0; i < toolButtons_.size(); ++i) {
        toolButtons_[i]->setActive(static_cast<AnnotationTool>(i) == current);
    }
    const editor::AnnotationStyle style = annotations_.displayedStyle();
    for (std::size_t i = 0; i < swatches_.size(); ++i) {
        swatches_[i]->setActive(sameColor(style.color, kPresetColors[i]));
    }
    for (std::size_t i = 0; i < opacityButtons_.size(); ++i) {
        opacityButtons_[i]->setActive(std::fabs(static_cast<double>(style.opacity - kOpacitySteps[i])) < 0.01);
    }
    for (std::size_t i = 0; i < widthButtons_.size(); ++i) {
        widthButtons_[i]->setActive(std::fabs(static_cast<double>(style.borderWidth - kWidthSteps[i])) < 0.01);
    }
    fillButton_->setActive(style.interiorColor.has_value());
    stampButton_->setLabel(std::format("Stamp: {}", pdf::stampNameText(annotations_.displayedStampName())));
    positionItems();
    strip_->invalidate();
}

// Tool buttons always show; a style control shows when the tool the style
// addresses (selection's kind, else the current tool) uses it.
void AnnotationBarController::positionItems() {
    const AnnotationTool addressed = annotations_.styleTool();
    const double y = std::max(0.0, (strip_->frame().size.height - kButtonHeight) / 2.0);
    double x = kPadding;
    for (const Item& item : items_) {
        const bool shown = visible_ && (item.applies == nullptr || item.applies(addressed));
        if (!shown) {
            item.button->setFrame(kHiddenFrame);
            continue;
        }
        const double width = item.width;
        item.button->setFrame(core::Rect{x, y, width, kButtonHeight});
        x += width + item.gapAfter;
    }
}

} // namespace rivet::app
