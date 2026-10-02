// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ContentController.hpp"
#include "app/ShellContext.hpp"

#include "core/geometry/Rect.hpp"

#include <functional>
#include <string>
#include <vector>

namespace rivet::ui {
class Button;
class Container;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// The properties bar of the content tools (ADR-0014/0015): a thin strip under
// the toolbar that shows while a content tool is active. Its controls follow
// the selection:
//   - text block (or the Add Text tool with nothing selected): font family
//     (Sans / Serif / Mono), Bold, size -/+, six colors; Edit Text and, for
//     blocks Rivet wrote, Front;
//   - image: pixel size, Replace Image..., Delete;
//   - any other selectable object: Delete;
//   - nothing selected under Select/Edit: a one-line hint.
// It reads and writes only through ContentController; it never touches the
// document. Main thread only.
class ContentBarController {
public:
    static constexpr double kHeight = 36.0;

    // Takes over ContentController::setOnStateChanged.
    ContentBarController(ShellContext& context, ui::Widget& parent, ContentController& content);

    ContentBarController(const ContentBarController&) = delete;
    ContentBarController& operator=(const ContentBarController&) = delete;

    // Height the bar takes in the shell layout (0 while no content tool is active).
    double height() const { return visible() ? kHeight : 0.0; }
    bool visible() const { return content_.tool() != ContentTool::None; }

    void layout(const core::Rect& frame);
    // Re-reads the controller: highlights, labels, which controls show.
    void refresh();

    // Test access: the button with this label (nullptr when none) and whether
    // it is currently laid out (shown).
    ui::Button* button(const std::string& label) const;
    bool controlShown(const ui::Button& button) const;
    const ui::Container& strip() const { return *strip_; }
    const std::string& hintText() const;

private:
    struct Item {
        ui::Widget* widget = nullptr;
        ui::Button* button = nullptr; // set when the widget is a Button
        double width = 0.0;
        double gapAfter = 4.0;
        std::function<bool()> applies;
    };

    ui::Button* addButton(std::string label, double width, std::function<void()> onClick, double gapAfter,
                          std::function<bool()> applies);
    void positionItems();

    ShellContext& context_;
    ContentController& content_;
    core::Rect frame_;
    ui::Container* strip_ = nullptr;
    std::vector<Item> items_;
    std::vector<ui::Button*> familyButtons_;
    std::vector<ui::Button*> swatches_;
    ui::Button* boldButton_ = nullptr;
    ui::Button* sizeLabel_ = nullptr;
    ui::Button* pixelLabel_ = nullptr;
    ui::Widget* hint_ = nullptr;
    bool wasVisible_ = false;
};

} // namespace rivet::app
