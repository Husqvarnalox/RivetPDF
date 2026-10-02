// SPDX-License-Identifier: MPL-2.0
#include "app/ContentBarController.hpp"

#include "app/TextLabel.hpp"
#include "ui/Button.hpp"
#include "ui/Container.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace rivet::app {

namespace {

constexpr double kButtonHeight = 26.0;
constexpr double kSwatchSize = 24.0;
constexpr double kPadding = 10.0;
constexpr double kGroupGap = 14.0;

// Black, red, green, blue, orange, gray.
constexpr std::array<pdf::PdfColor, 6> kTextColors = {{
    {0.0F, 0.0F, 0.0F},
    {0.85F, 0.15F, 0.15F},
    {0.15F, 0.55F, 0.25F},
    {0.15F, 0.35F, 0.85F},
    {0.95F, 0.50F, 0.0F},
    {0.45F, 0.45F, 0.45F},
}};

constexpr const char* kSelectHint =
    "Click an object to select it. Drag to move, Delete to remove, double-click text to edit.";
constexpr const char* kAddTextHint = "Click the page to type a text block, or drag to draw a box.";

ui::Color toUiColor(const pdf::PdfColor& color) {
    return ui::Color::rgba(static_cast<double>(color.r), static_cast<double>(color.g),
                           static_cast<double>(color.b), 1.0);
}

double sizeStep(double size) { return size >= 36.0 ? 4.0 : (size >= 18.0 ? 2.0 : 1.0); }

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

ContentBarController::ContentBarController(ShellContext& context, ui::Widget& parent, ContentController& content)
    : context_(context), content_(content) {
    auto strip = std::make_unique<Strip>();
    strip_ = strip.get();
    strip_->setBackgroundColor(ui::Color::rgba(0.95, 0.95, 0.95, 1.0));
    parent.addChild(std::move(strip));

    const auto textContext = [this] {
        return content_.selectionIsText() ||
               (content_.tool() == ContentTool::AddText && !content_.selected().has_value());
    };

    using Family = ContentController::FontFamily;
    const std::array<std::pair<const char*, Family>, 3> families = {
        {{"Sans", Family::Sans}, {"Serif", Family::Serif}, {"Mono", Family::Mono}}};
    for (const auto& [label, family] : families) {
        const Family chosen = family;
        familyButtons_.push_back(
            addButton(label, 52.0, [this, chosen] { content_.setFontFamily(chosen); }, 4.0, textContext));
    }
    boldButton_ = addButton(
        "B", 30.0, [this] { content_.setBold(!content_.displayedStyle().bold); }, kGroupGap, textContext);

    addButton("−", 28.0,
              [this] {
                  const double size = content_.displayedStyle().size;
                  content_.setFontSize(size - sizeStep(size - 1.0));
              },
              2.0, textContext);
    sizeLabel_ = addButton("12 pt", 52.0, nullptr, 2.0, textContext);
    addButton("+", 28.0,
              [this] {
                  const double size = content_.displayedStyle().size;
                  content_.setFontSize(size + sizeStep(size));
              },
              kGroupGap, textContext);

    for (const pdf::PdfColor& color : kTextColors) {
        ui::Button* swatch =
            addButton("", kSwatchSize, [this, color] { content_.setTextColor(color); }, 4.0, textContext);
        swatch->setSwatch(toUiColor(color));
        swatches_.push_back(swatch);
    }
    items_.back().gapAfter = kGroupGap;

    addButton("Edit Text", 78.0, [this] { content_.perform(ContentCommand::EditText); }, 4.0,
              [this] { return content_.canPerform(ContentCommand::EditText); });
    addButton("Front", 52.0, [this] { content_.perform(ContentCommand::BringToFront); }, kGroupGap,
              [this] { return content_.canPerform(ContentCommand::BringToFront); });

    pixelLabel_ = addButton("", 110.0, nullptr, 4.0, [this] { return content_.selectionIsImage(); });
    addButton("Replace Image…", 120.0, [this] { content_.perform(ContentCommand::ReplaceImage); }, 4.0,
              [this] { return content_.canPerform(ContentCommand::ReplaceImage); });
    addButton("Delete", 60.0, [this] { content_.perform(ContentCommand::DeleteObject); }, 4.0,
              [this] { return content_.canPerform(ContentCommand::DeleteObject); });

    auto hint = std::make_unique<TextLabel>(kSelectHint);
    hint_ = hint.get();
    strip_->addChild(std::move(hint));

    content_.setOnStateChanged([this] { refresh(); });
    strip_->setFrame(kHiddenFrame);
    refresh();
}

ui::Button* ContentBarController::addButton(std::string label, double width, std::function<void()> onClick,
                                            double gapAfter, std::function<bool()> applies) {
    auto button = std::make_unique<ui::Button>(std::move(label));
    ui::Button* raw = button.get();
    if (onClick) button->setOnClick(std::move(onClick));
    button->setFrame(core::Rect{0.0, 0.0, width, kButtonHeight});
    strip_->addChild(std::move(button));
    items_.push_back(Item{raw, raw, width, gapAfter, std::move(applies)});
    return raw;
}

void ContentBarController::layout(const core::Rect& frame) {
    frame_ = frame;
    strip_->setFrame(visible() ? frame : kHiddenFrame);
    positionItems();
}

ui::Button* ContentBarController::button(const std::string& label) const {
    for (const Item& item : items_) {
        if (item.button != nullptr && item.button->label() == label) return item.button;
    }
    return nullptr;
}

bool ContentBarController::controlShown(const ui::Button& button) const { return !button.frame().isEmpty(); }

const std::string& ContentBarController::hintText() const { return static_cast<const TextLabel*>(hint_)->text(); }

void ContentBarController::refresh() {
    if (visible() != wasVisible_) {
        wasVisible_ = visible();
        context_.relayout(); // the bar appeared or disappeared: the shell re-lays out (and calls layout())
    }
    const ContentController::TextStyle style = content_.displayedStyle();
    using Family = ContentController::FontFamily;
    const std::array<Family, 3> order = {Family::Sans, Family::Serif, Family::Mono};
    for (std::size_t i = 0; i < familyButtons_.size(); ++i) familyButtons_[i]->setActive(style.family == order[i]);
    boldButton_->setActive(style.bold);
    for (std::size_t i = 0; i < swatches_.size(); ++i) swatches_[i]->setActive(style.color == kTextColors[i]);
    sizeLabel_->setLabel(std::format("{} pt", static_cast<int>(std::lround(style.size))));
    const auto [w, h] = content_.selectedImagePixels();
    pixelLabel_->setLabel(w > 0 ? std::format("{} × {} px", w, h) : std::string());
    static_cast<TextLabel*>(hint_)->setText(content_.tool() == ContentTool::AddText ? kAddTextHint : kSelectHint);
    strip_->setFrame(visible() && !frame_.isEmpty() ? frame_ : kHiddenFrame);
    positionItems();
    strip_->invalidate();
}

void ContentBarController::positionItems() {
    const double y = std::max(0.0, (strip_->frame().size.height - kButtonHeight) / 2.0);
    double x = kPadding;
    bool any = false;
    const bool barVisible = visible() && !strip_->frame().isEmpty();
    for (const Item& item : items_) {
        const bool shown = barVisible && item.applies && item.applies();
        if (!shown) {
            item.widget->setFrame(kHiddenFrame);
            continue;
        }
        item.widget->setFrame(core::Rect{x, y, item.width, kButtonHeight});
        x += item.width + item.gapAfter;
        any = true;
    }
    if (barVisible && !any) {
        hint_->setFrame(core::Rect{kPadding, y, std::max(0.0, strip_->frame().size.width - 2.0 * kPadding),
                                   kButtonHeight});
    } else {
        hint_->setFrame(kHiddenFrame);
    }
}

} // namespace rivet::app
