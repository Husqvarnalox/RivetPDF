// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/MarkdownFind.hpp"
#include "app/SearchTarget.hpp"
#include "ui/SourceEditor.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::app {

// Find in the Markdown SOURCE text: adapts a ui::SourceEditor (the pane's
// mirror of the authoritative source) to the shell's ISearchTarget. ASCII
// case-insensitive plain-text search; matches are drawn through the editor's
// highlight API; the current match is revealed (scrolled into view) without
// moving the caret or the selection. Edits re-run the search lazily: call
// refresh() after the text may have changed (the host does on every state
// change). Main thread only.
class MarkdownSourceSearch final : public ISearchTarget {
public:
    explicit MarkdownSourceSearch(ui::SourceEditor& editor) : editor_(editor) {}
    ~MarkdownSourceSearch() override { editor_.clearHighlights(); }

    // The editor's text changed or a different tab was bound: recompute the
    // matches when the text revision moved (keeps the current match near
    // its old position). Cheap when nothing changed.
    void refresh();
    // Forgets matches and highlights but keeps the query (a different tab).
    void reset();

    void startSearch(std::string query) override;
    void cancelSearch() override {}
    void nextMatch() override { step(+1); }
    void previousMatch() override { step(-1); }
    const std::string& searchQuery() const override { return query_; }
    std::size_t matchCount() const override { return matches_.size(); }
    std::optional<std::size_t> currentMatch() const override { return current_; }
    bool searching() const override { return false; }
    void revealCurrentMatch() override;
    void setOnSearchResultsChanged(std::function<void()> onChanged) override { onChanged_ = std::move(onChanged); }

    const std::vector<TextMatch>& matches() const { return matches_; }

private:
    void recompute();
    void publish();
    void step(int delta);

    ui::SourceEditor& editor_;
    std::string query_;
    std::vector<TextMatch> matches_;
    std::optional<std::size_t> current_;
    std::uint64_t textRevision_ = static_cast<std::uint64_t>(-1);
    std::function<void()> onChanged_;
};

} // namespace rivet::app
