// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/DocumentWorkspace.hpp"
#include "core/geometry/Rect.hpp"
#include "platform/PlatformKit.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace rivet::ui {
class PdfViewport;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// Frame of a hidden widget: a zero-size frame paints nothing and consumes no
// input (the tree skips empty frames), so "hidden" is purely a frame state.
inline constexpr core::Rect kHiddenFrame{-1.0, -1.0, 0.0, 0.0};

// Controllers that hold edits which are not part of the document yet (an open
// note editor) register a commit hook here. Everything that reads the
// document as a whole (save, Save As, export, print, closing a tab or window,
// quitting) first runs commitAll() so those edits are not lost. Main thread
// only; hooks run outside any lock.
class PendingEdits {
public:
    // Commits the controller's pending edits. Returns false when it could not
    // (the session is locked by a running save): the edits then stay pending
    // in the controller instead of being dropped.
    using Hook = std::function<bool()>;

    // Returns the token to pass to remove().
    std::size_t add(Hook hook) {
        hooks_.emplace_back(nextToken_, std::move(hook));
        return nextToken_++;
    }
    void remove(std::size_t token) {
        std::erase_if(hooks_, [token](const auto& item) { return item.first == token; });
    }
    // True when every hook committed (or had nothing to commit).
    bool commitAll() const {
        const auto hooks = hooks_; // a hook may (un)register others
        bool all = true;
        for (const auto& item : hooks) {
            if (item.second && !item.second()) all = false;
        }
        return all;
    }

private:
    std::vector<std::pair<std::size_t, Hook>> hooks_;
    std::size_t nextToken_ = 1;
};

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

    // Uncommitted-edit hooks (see PendingEdits). Last member with a default
    // initializer so aggregate initialization of the sinks above stays valid.
    PendingEdits pendingEdits{};

    // The active tab when it is a Ready PDF tab (session attached), otherwise
    // null. Every PDF feature controller goes through this, so a Markdown (or
    // loading/failed) active tab leaves them inert.
    DocumentTab* readyPdfTab() const {
        DocumentTab* tab = workspace.activeTab();
        return (tab != nullptr && tab->isPdf() && tab->state() == DocumentTab::State::Ready &&
                tab->session() != nullptr)
                   ? tab
                   : nullptr;
    }
    // Historical name of readyPdfTab() (PDF-only), kept for existing callers.
    DocumentTab* readyActiveTab() const { return readyPdfTab(); }
    // The active tab when it is a Ready Markdown tab, otherwise null.
    DocumentTab* readyMarkdownTab() const {
        DocumentTab* tab = workspace.activeTab();
        return (tab != nullptr && tab->isMarkdown() && tab->state() == DocumentTab::State::Ready &&
                tab->markdown() != nullptr)
                   ? tab
                   : nullptr;
    }
};

} // namespace rivet::app
