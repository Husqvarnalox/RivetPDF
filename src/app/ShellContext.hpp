// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "core/geometry/Rect.hpp"
#include "platform/PlatformKit.hpp"

#include <functional>
#include <string>

namespace rivet::ui {
class PdfViewport;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// Frame of a hidden widget: a zero-size frame paints nothing and consumes no
// input (the tree skips empty frames), so "hidden" is purely a frame state.
inline constexpr core::Rect kHiddenFrame{-1.0, -1.0, 0.0, 0.0};

// What the shell's feature controllers share: the workspace, the platform
// services, the document viewport and a few shell-level sinks. The shell
// owns every referenced object and outlives all controllers (it constructs
// them after, and destroys them before, the referenced members). Main
// thread only.
struct ShellContext {
    DocumentWorkspace& workspace;
    const platform::ShellServices& services;
    ui::PdfViewport& viewport;

    // Status bar message sink.
    std::function<void(std::string)> setStatus;
    // Keyboard focus routing (nullptr = viewport focus).
    std::function<void(ui::Widget*)> setFocus;
    // Re-runs the shell layout (e.g. a panel appeared or disappeared).
    std::function<void()> relayout;

    // The active tab when it is Ready (session attached), otherwise null.
    DocumentTab* readyActiveTab() const {
        DocumentTab* tab = workspace.activeTab();
        return (tab != nullptr && tab->state() == DocumentTab::State::Ready) ? tab : nullptr;
    }
};

} // namespace rivet::app
