// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/AnnotationController.hpp"
#include "app/ShellContext.hpp"

#include "core/geometry/Rect.hpp"

#include <functional>
#include <vector>

namespace rivet::ui {
class Button;
class Container;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// The annotation strip under the main toolbar (ADR-0013): tool buttons, then
// the style controls that apply to the current tool or selection (color
// swatches, opacity, width, fill, stamp name). The active tool and the
// current values are marked with the Button's active state. Purely a view of
// AnnotationController: every click calls the controller, and the controller's
// state notification re-reads everything back.
//
// Main thread only.
class AnnotationBarController {
public:
    static constexpr double kHeight = 36.0;

    // Builds the (hidden) strip into `parent` and takes over the controller's
    // state notification.
    AnnotationBarController(ShellContext& context, ui::Widget& parent, AnnotationController& annotations);

    AnnotationBarController(const AnnotationBarController&) = delete;
    AnnotationBarController& operator=(const AnnotationBarController&) = delete;

    bool visible() const { return visible_; }
    // Hiding returns to the Select tool (no hidden tool keeps grabbing the
    // pointer). Relayouts the shell.
    void setVisible(bool visible);
    void toggle() { setVisible(!visible_); }
    // Layout slot height in the shell: kHeight while visible, else 0.
    double height() const { return visible_ ? kHeight : 0.0; }
    void setOnVisibilityChanged(std::function<void()> callback) { onVisibilityChanged_ = std::move(callback); }

    // Places the strip (parent space); kHiddenFrame hides it.
    void layout(const core::Rect& frame);

    // Re-reads tool, style and selection into the buttons and repositions
    // the style controls that apply.
    void refresh();

    // Test access.
    ui::Button& toolButton(AnnotationTool tool) { return *toolButtons_[static_cast<std::size_t>(tool)]; }
    const ui::Container& strip() const { return *strip_; }
    bool controlShown(const ui::Button& button) const;

private:
    struct Item {
        ui::Button* button = nullptr;
        double width = 0.0;
        double gapAfter = 4.0;
        // Style controls only: which tools they apply to.
        bool (*applies)(AnnotationTool) = nullptr;
    };

    ui::Button* addButton(std::string label, double width, std::function<void()> onClick, double gapAfter,
                          bool (*applies)(AnnotationTool) = nullptr);
    void positionItems();

    ShellContext& context_;
    AnnotationController& annotations_;
    ui::Container* strip_ = nullptr;
    std::vector<Item> items_;
    std::vector<ui::Button*> toolButtons_;
    std::vector<ui::Button*> swatches_;
    std::vector<ui::Button*> opacityButtons_;
    std::vector<ui::Button*> widthButtons_;
    ui::Button* fillButton_ = nullptr;
    ui::Button* stampButton_ = nullptr;
    bool visible_ = false;
    std::function<void()> onVisibilityChanged_;
};

} // namespace rivet::app
