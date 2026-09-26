// SPDX-License-Identifier: MPL-2.0
#include "editor/PageSelection.hpp"

#include <algorithm>

namespace rivet::editor {

void PageSelection::select(core::PageId page) {
    selected_.clear();
    selected_.insert(page);
    active_ = page;
    anchor_ = page;
}

void PageSelection::toggle(core::PageId page) {
    if (!selected_.erase(page)) selected_.insert(page);
    active_ = page;
    anchor_ = page;
}

void PageSelection::selectRange(core::PageId page, const PageModelSnapshot& snapshot) {
    const std::size_t to = snapshot.indexOf(page);
    const std::size_t from = anchor_ ? snapshot.indexOf(anchor_) : PageModelSnapshot::kInvalidIndex;
    if (to == PageModelSnapshot::kInvalidIndex) return;
    if (from == PageModelSnapshot::kInvalidIndex) {
        select(page);
        return;
    }
    selected_.clear();
    for (std::size_t i = std::min(from, to); i <= std::max(from, to); ++i) {
        selected_.insert(snapshot.at(i).id);
    }
    active_ = page;
}

void PageSelection::selectAll(const PageModelSnapshot& snapshot) {
    selected_.clear();
    for (const PageEntry& entry : snapshot.entries()) selected_.insert(entry.id);
    if (!snapshot.contains(active_)) active_ = snapshot.size() > 0 ? snapshot.at(0).id : core::PageId{};
    anchor_ = active_;
}

void PageSelection::clear() {
    selected_.clear();
    active_ = core::PageId{};
    anchor_ = core::PageId{};
}

std::vector<core::PageId> PageSelection::inOrder(const PageModelSnapshot& snapshot) const {
    std::vector<core::PageId> ids;
    ids.reserve(selected_.size());
    if (selected_.size() * 8 < snapshot.size()) {
        // Small selection: sort by index instead of walking every page.
        std::vector<std::pair<std::size_t, core::PageId>> indexed;
        for (const core::PageId id : selected_) {
            const std::size_t index = snapshot.indexOf(id);
            if (index != PageModelSnapshot::kInvalidIndex) indexed.emplace_back(index, id);
        }
        std::sort(indexed.begin(), indexed.end());
        for (const auto& item : indexed) ids.push_back(item.second);
        return ids;
    }
    for (const PageEntry& entry : snapshot.entries()) {
        if (selected_.count(entry.id) != 0) ids.push_back(entry.id);
    }
    return ids;
}

void PageSelection::applyChange(const PageModelChange& change) {
    if (change.current == nullptr) return;
    const PageModelSnapshot& current = *change.current;
    for (const core::PageId id : change.removed) selected_.erase(id);

    if (active_ && !current.contains(active_)) {
        core::PageId replacement;
        const std::size_t old = change.previous ? change.previous->indexOf(active_)
                                                : PageModelSnapshot::kInvalidIndex;
        if (old != PageModelSnapshot::kInvalidIndex) {
            const auto& previous = change.previous->entries();
            for (std::size_t i = old + 1; i < previous.size() && !replacement; ++i) {
                if (current.contains(previous[i].id)) replacement = previous[i].id;
            }
            for (std::size_t i = old; i > 0 && !replacement; --i) {
                if (current.contains(previous[i - 1].id)) replacement = previous[i - 1].id;
            }
        }
        if (!replacement && current.size() > 0) replacement = current.at(0).id;
        active_ = replacement;
        if (selected_.empty() && active_) selected_.insert(active_);
    }
    if (anchor_ && !current.contains(anchor_)) anchor_ = active_;
}

} // namespace rivet::editor
