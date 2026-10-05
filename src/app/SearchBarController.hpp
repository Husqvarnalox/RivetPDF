// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/SearchTarget.hpp"
#include "app/ShellContext.hpp"

#include "core/geometry/Rect.hpp"

#include <functional>
#include <string>

namespace rivet::ui {
class Container;
class TextField;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

class TextLabel;

// The find bar: query field, previous/next buttons and a match counter,
// floating at the viewport's top-right while visible. Drives the ACTIVE
// Ready tab's TextSearchController; results of background tabs never touch
// the bar or the view.
//
// Main thread only.
class SearchBarController {
public:
    // Builds the (hidden) bar into `parent` (appended as its next child).
    SearchBarController(ShellContext& context, ui::Widget& parent);

    SearchBarController(const SearchBarController&) = delete;
    SearchBarController& operator=(const SearchBarController&) = delete;

    // Binds a freshly activated Ready tab: its search notifies the bar and
    // repaints the view while it is the active tab. A different tab has a
    // different search, so the bar closes.
    void bindTab(DocumentTab& tab);

    // Markdown tabs: the bar drives the ISearchTarget returned by `provider`
    // (null when the displayed mode has nothing searchable) instead of a PDF
    // TextSearchController. The provider is queried on every use.
    void setMarkdownTargetProvider(std::function<ISearchTarget*()> provider);
    // Binds the bar to a freshly activated Markdown tab: clears the query and
    // closes the bar.
    void bindMarkdownTab();

    // Places the bar inside `viewportFrame` (parent space) when visible.
    void layout(const core::Rect& viewportFrame);

    bool visible() const { return visible_; }
    // Hiding cancels the active tab's running search (matches are kept) and
    // drops focus from the field. Relayouts the shell.
    void setVisible(bool visible);
    // Cmd+F: toggles the bar; showing it focuses the query field.
    void toggle();
    // Escape priority: closes a visible bar. Returns true when consumed.
    bool handleEscape();

    // Re-reads the active tab's search state into the match counter:
    // "" (no query), "Searching…", "No matches" or "n / m".
    void updateSearchUi();
    // Scrolls the active match into view (cached page text only).
    void revealActiveMatch();

    ui::TextField& field() { return *field_; }
    const std::string& countText() const;

private:
    // Enter / arrow buttons: steps the active match and reveals it.
    void step(int delta);

    // The Markdown search target while a Ready Markdown tab is active (hooks
    // the results callback on first use), else null.
    ISearchTarget* markdownTarget();

    ShellContext& context_;
    std::function<ISearchTarget*()> markdownProvider_;
    ISearchTarget* hookedTarget_ = nullptr;
    // Raw pointers into widgets owned by the parent's tree.
    ui::Container* bar_ = nullptr;
    ui::TextField* field_ = nullptr;
    TextLabel* countLabel_ = nullptr;
    bool visible_ = false;
};

} // namespace rivet::app
