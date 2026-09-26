// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ShellContext.hpp"

#include "core/geometry/Rect.hpp"

namespace rivet::ui {
class Container;
class TextField;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

class TextLabel;

// Password prompt for protected documents: a panel centered over the
// viewport, shown while the active tab is NeedsPassword. The field is
// echo-masked; the password is never stored beyond the retry call (the
// field is cleared before the workspace retries).
//
// Main thread only.
class PasswordPromptController {
public:
    // Builds the (hidden) panel into `parent` (appended as its next child).
    PasswordPromptController(ShellContext& context, ui::Widget& parent);

    PasswordPromptController(const PasswordPromptController&) = delete;
    PasswordPromptController& operator=(const PasswordPromptController&) = delete;

    // Shows the prompt for a NeedsPassword tab (and focuses the field);
    // hides it for anything else, including null.
    void bindTab(DocumentTab* tab);

    // Centers the visible panel in `viewportFrame` (parent space).
    void layout(const core::Rect& viewportFrame);

    bool visible() const { return visible_; }

    // Unlock / Enter: retries the active NeedsPassword tab with the typed
    // password. No-op for any other tab state.
    void submit();

    ui::TextField& field() { return *field_; }

private:
    ShellContext& context_;
    // Raw pointers into widgets owned by the parent's tree.
    ui::Container* panel_ = nullptr;
    TextLabel* message_ = nullptr;
    ui::TextField* field_ = nullptr;
    bool visible_ = false;
    core::Rect viewportFrame_;
};

} // namespace rivet::app
