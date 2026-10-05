// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownSourceSearch.hpp"

#include <algorithm>
#include <utility>

namespace rivet::app {

void MarkdownSourceSearch::publish() {
    std::vector<ui::SourceEditor::Highlight> ranges;
    ranges.reserve(matches_.size());
    for (const TextMatch& m : matches_) ranges.push_back({m.begin, m.end});
    editor_.setHighlights(std::move(ranges), current_);
    textRevision_ = editor_.buffer().revision();
}

void MarkdownSourceSearch::recompute() {
    const std::size_t anchor = current_ && *current_ < matches_.size() ? matches_[*current_].begin : editor_.caretOffset();
    matches_ = findInText(editor_.buffer().text(), query_);
    current_.reset();
    if (!matches_.empty()) {
        // First match at or after the anchor (caret on a fresh search), wrapping to the first.
        const auto it = std::lower_bound(matches_.begin(), matches_.end(), anchor,
                                         [](const TextMatch& m, std::size_t value) { return m.begin < value; });
        current_ = it == matches_.end() ? 0 : static_cast<std::size_t>(it - matches_.begin());
    }
    publish();
}

void MarkdownSourceSearch::refresh() {
    if (query_.empty() || editor_.buffer().revision() == textRevision_) return;
    const std::size_t before = matches_.size();
    const std::optional<std::size_t> oldCurrent = current_;
    recompute();
    if (matches_.size() != before || current_ != oldCurrent) {
        if (onChanged_) onChanged_();
    }
}

void MarkdownSourceSearch::reset() {
    matches_.clear();
    current_.reset();
    editor_.clearHighlights();
    textRevision_ = static_cast<std::uint64_t>(-1);
    if (!query_.empty() && onChanged_) onChanged_();
}

void MarkdownSourceSearch::startSearch(std::string query) {
    query_ = std::move(query);
    current_.reset();
    matches_.clear();
    if (query_.empty()) {
        editor_.clearHighlights();
        textRevision_ = static_cast<std::uint64_t>(-1);
    } else {
        recompute();
    }
    if (onChanged_) onChanged_();
}

void MarkdownSourceSearch::step(int delta) {
    if (matches_.empty()) return;
    refresh();
    if (matches_.empty()) return;
    const std::size_t n = matches_.size();
    if (!current_) {
        current_ = delta > 0 ? 0 : n - 1;
    } else {
        current_ = (*current_ + (delta > 0 ? 1 : n - 1)) % n;
    }
    publish();
    revealCurrentMatch();
    if (onChanged_) onChanged_();
}

void MarkdownSourceSearch::revealCurrentMatch() {
    if (!current_ || *current_ >= matches_.size()) return;
    editor_.revealOffset(matches_[*current_].begin);
}

} // namespace rivet::app
