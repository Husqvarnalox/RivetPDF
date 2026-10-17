// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ShellContext.hpp"

#include "core/geometry/Rect.hpp"

#include <cstddef>
#include <string>
#include <string_view>

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
    // Shown texts (tests, QA harness): the sentence naming the file and the
    // wrong-password feedback ("" when none).
    const std::string& messageText() const;
    const std::string& errorText() const;

    // `name` shortened in the middle with an ellipsis to at most
    // `maxCodePoints` code points (UTF-8 safe).
    static std::string shortenName(std::string_view name, std::size_t maxCodePoints);

private:
    ShellContext& context_;
    // Raw pointers into widgets owned by the parent's tree.
    ui::Container* panel_ = nullptr;
    TextLabel* title_ = nullptr;
    TextLabel* message_ = nullptr;
    TextLabel* error_ = nullptr;
    ui::TextField* field_ = nullptr;
    bool visible_ = false;
    core::Rect viewportFrame_;
};

} // namespace rivet::app
