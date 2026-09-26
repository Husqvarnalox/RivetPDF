// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "editor/PageModel.hpp"

#include "core/StrongId.hpp"

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace rivet::editor {

// Page selection (thumbnails / page operations), independent of the text
// selection. A set of PageIds plus the ACTIVE page (keyboard focus, target
// of single-page operations) and the ANCHOR of Shift-range selection.
// Ids, not indices: the selection survives reorders untouched.
//
// Main-thread only; pure state.
class PageSelection {
public:
    // Plain click: selects exactly `page`; active = anchor = page.
    void select(core::PageId page);
    // Cmd/Ctrl-click: toggles `page`; active = anchor = page (the page gets
    // focus even when it was toggled off).
    void toggle(core::PageId page);
    // Shift-click: selects the range anchor..page in the snapshot's CURRENT
    // order (replacing the selection; Cmd/Ctrl+Shift extension is the
    // caller's union). active = page, anchor unchanged. Without a valid
    // anchor it behaves like select().
    void selectRange(core::PageId page, const PageModelSnapshot& snapshot);
    // Every page; active kept when valid (else the first page), anchor =
    // active.
    void selectAll(const PageModelSnapshot& snapshot);
    void clear();

    bool contains(core::PageId page) const { return selected_.count(page) != 0; }
    std::size_t count() const { return selected_.size(); }
    bool empty() const { return selected_.empty(); }
    core::PageId active() const { return active_; }
    core::PageId anchor() const { return anchor_; }

    // Selected ids in the snapshot's order (ids not in the snapshot skipped).
    std::vector<core::PageId> inOrder(const PageModelSnapshot& snapshot) const;

    // Post-edit policy (call with every PageModelChange):
    //   - deleted ids are dropped from the selection;
    //   - moved / rotated / cropped ids stay selected (ids are stable);
    //   - a deleted ACTIVE page moves focus to the nearest surviving page
    //     (the next page in the previous order, else the previous one),
    //     which is selected when the selection became empty;
    //   - a deleted anchor is replaced by the active page;
    //   - added pages (duplicate/insert/undo of delete) are not selected
    //     implicitly - the app selects created ids explicitly if desired.
    void applyChange(const PageModelChange& change);

private:
    std::unordered_set<core::PageId> selected_;
    core::PageId active_;
    core::PageId anchor_;
};

} // namespace rivet::editor
