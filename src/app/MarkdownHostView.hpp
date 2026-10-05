// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/MarkdownTabState.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "platform/PlatformKit.hpp"
#include "ui/Widget.hpp"

#include <functional>
#include <memory>
#include <string>

namespace rivet::app {

// What a Markdown view may use from the shell. All pointers are shell-owned
// and outlive the view. Main thread only (workers must go through
// `dispatcher` to touch the view or the state).
struct MarkdownHostEnvironment {
    core::IMainThreadDispatcher* dispatcher = nullptr; // marshal worker results to the main thread
    core::TaskScheduler* scheduler = nullptr;          // shared worker pool (parse/layout/highlight jobs)
    const platform::ShellServices* services = nullptr; // clipboard, url opener, ...
    std::function<void(std::string)> setStatus;        // status bar message
    std::function<void(ui::Widget*)> setFocus;         // keyboard focus routing (nullptr = no widget)
};

// The shell's single slot for the Markdown experience: one widget that fills
// the content area while a Markdown tab is active (the shell hides all PDF
// chrome then) and draws/edits whatever the tab's display mode asks for
// (Rendered, Source, Split; see MarkdownTabState::mode()).
//
// Contract:
//  - The shell owns ONE view for its lifetime and rebinds it on every tab
//    switch: bind(&state) for a Ready Markdown tab, bind(nullptr) otherwise
//    (a view must drop every pointer into the state on bind(nullptr)).
//  - The bound state outlives the binding (the shell unbinds before a tab is
//    destroyed). Per-tab view state (scroll, caret) belongs in the state or a
//    side table keyed by the state, not in the view.
//  - stateChanged() fires (main thread) after every observable state change:
//    source mutation (undo/redo/commands), display-mode change, dirty flip.
//    Edits are made by running commands: state->execute(make_unique<
//    TextEditCommand>(...)). Views key caches on state->revision().
//  - The view receives mouse/key input through the normal Widget methods
//    while it is the active content (the shell routes unhandled keys to it).
//
// To replace the placeholder, implement createMarkdownHostView() with the
// real widget (the factory lives in MarkdownHostView.cpp).
class MarkdownHostView : public ui::Widget {
public:
    explicit MarkdownHostView(MarkdownHostEnvironment environment) : environment_(std::move(environment)) {}

    virtual void bind(MarkdownTabState* state) { state_ = state; invalidate(); }
    virtual void stateChanged() { invalidate(); }

    MarkdownTabState* state() const { return state_; }
    const MarkdownHostEnvironment& environment() const { return environment_; }

protected:
    MarkdownTabState* state_ = nullptr;

private:
    MarkdownHostEnvironment environment_;
};

// Placeholder implementation: draws the mode and the source's first lines.
std::unique_ptr<MarkdownHostView> createMarkdownHostView(MarkdownHostEnvironment environment);

} // namespace rivet::app
