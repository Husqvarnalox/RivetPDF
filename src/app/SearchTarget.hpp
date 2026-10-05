// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace rivet::app {

// What the find bar drives: one searchable content (a PDF tab's text search,
// the rendered Markdown preview). Main thread only.
class ISearchTarget {
public:
    virtual ~ISearchTarget() = default;

    // Starts (replacing a running search). An empty query clears the matches.
    virtual void startSearch(std::string query) = 0;
    // Stops a running search; matches found so far are kept.
    virtual void cancelSearch() = 0;
    // Step the current match (wraps around); no-ops without matches.
    virtual void nextMatch() = 0;
    virtual void previousMatch() = 0;

    virtual const std::string& searchQuery() const = 0;
    virtual std::size_t matchCount() const = 0;
    // Index of the current match; nullopt when there is none.
    virtual std::optional<std::size_t> currentMatch() const = 0;
    // True while results are still being produced (counter shows "Searching…").
    virtual bool searching() const = 0;

    // Scrolls the current match into view.
    virtual void revealCurrentMatch() = 0;

    // Fired (main thread) when matches or the current match changed.
    virtual void setOnSearchResultsChanged(std::function<void()> onChanged) = 0;
};

} // namespace rivet::app
