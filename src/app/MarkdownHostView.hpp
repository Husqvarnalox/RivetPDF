// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/MarkdownParseCoordinator.hpp"
#include "app/MarkdownTabState.hpp"
#include "app/SearchTarget.hpp"
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
    // Live-preview tuning (debounce, synchronous-parse threshold) and the test
    // seam for where parse jobs run (default: `scheduler`).
    MarkdownParseTuning parseTuning;
    std::function<void(std::function<void()>)> runBackground;
};

class MarkdownPreviewView;
class MarkdownSourcePane;

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

    // Find in the displayed Markdown: the rendered text in Rendered mode, the
    // SOURCE text in Source and Split mode (the source is authoritative).
    // One stable target for the whole binding; it re-targets itself and keeps
    // its query when the mode changes. Null while nothing is bound.
    virtual ISearchTarget* searchTarget() { return nullptr; }
    // Edit-menu actions on the displayed view; false when not applicable.
    virtual bool copySelection() { return false; }
    virtual bool selectAll() { return false; }

    // Child geometry in host-local coordinates (empty rect = hidden).
    struct Panes {
        core::Rect source;
        core::Rect preview;
        core::Rect divider; // Split mode: the 1 px line between the two
    };
    virtual Panes panes() const { return {}; }
    // The hosted views and the parser (tests and shell wiring); null when the
    // implementation has none.
    virtual MarkdownPreviewView* previewView() { return nullptr; }
    virtual MarkdownSourcePane* sourcePane() { return nullptr; }
    virtual const MarkdownParseCoordinator* parseCoordinator() const { return nullptr; }

    MarkdownTabState* state() const { return state_; }
    const MarkdownHostEnvironment& environment() const { return environment_; }

protected:
    MarkdownTabState* state_ = nullptr;

private:
    MarkdownHostEnvironment environment_;
};

// Real host:
//   Rendered: MarkdownPreviewView over the parsed document;
//   Source:   MarkdownSourcePane (the editor) full size;
//   Split:    editor | 1 px divider | preview, 50/50 by default, the divider
//             can be dragged (clamped; the ratio is remembered per host).
// The preview is fed by a MarkdownParseCoordinator: small sources parse
// synchronously on bind, edits are debounced and parsed on the TaskScheduler
// (stale results dropped), so typing never waits for a parse. Switching modes
// or tabs keeps each tab's caret and scroll; in Split the two panes scroll
// together (see MarkdownSourceMap for the mapping).
std::unique_ptr<MarkdownHostView> createMarkdownHostView(MarkdownHostEnvironment environment);

} // namespace rivet::app
