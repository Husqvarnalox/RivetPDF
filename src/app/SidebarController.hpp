// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "app/ShellContext.hpp"

#include "core/StrongId.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfNavigation.hpp"
#include "ui/OutlinePanel.hpp"

#include <cstddef>
#include <set>
#include <vector>

namespace rivet::ui {
class Button;
class Container;
class PageThumbnailList;
class Widget;
} // namespace rivet::ui

namespace rivet::app {

// Path of an outline node: child indexes from the synthetic root down. The
// expansion state keys on paths, not on flattened row indexes, so it stays
// stable while rows above a node expand or collapse.
using OutlinePath = std::vector<std::size_t>;

// The visible rows of an outline tree plus, per row, the node's path and
// its destination page index (0 when the node has no usable destination).
struct FlattenedOutline {
    std::vector<ui::OutlineRow> rows;
    std::vector<OutlinePath> paths;
    std::vector<std::size_t> destinations;
};

// Depth-first flatten of `root`'s children (the synthetic root itself is
// skipped): a node shows its children only when its path is in `expanded`.
// Destinations at or beyond `pageCount` map to page 0.
FlattenedOutline flattenOutline(const pdf::PdfOutlineNode& root, const std::set<OutlinePath>& expanded,
                                std::size_t pageCount);

// The left sidebar: a mode header (Pages | Outline) above the active panel.
// Pages mode shows the ACTIVE tab's page thumbnails (selection follows the
// current page); Outline mode shows the document outline, loaded
// asynchronously on the session's link stream. Row activation navigates
// the viewport. Expansion state belongs to the bound document and resets
// when a different document is bound.
//
// Main thread only.
class SidebarController {
public:
    enum class Mode { Pages, Outline };

    static constexpr double kWidth = 220.0;
    static constexpr double kHeaderHeight = 34.0;

    // Builds the sidebar into `parent` (appended as its next child).
    SidebarController(ShellContext& context, ui::Widget& parent);

    SidebarController(const SidebarController&) = delete;
    SidebarController& operator=(const SidebarController&) = delete;

    // Binds the active tab: a Ready tab shows its thumbnails, page labels,
    // outline and current page; anything else (or null) clears both panels.
    void bindTab(DocumentTab* tab);

    // Current-page funnel: selects and reveals the page's thumbnail.
    void setCurrentPage(std::size_t page);

    // Positions the sidebar (frame in the parent's space; kHiddenFrame hides
    // it) and the active panel below the header.
    void layout(const core::Rect& frame);

    Mode mode() const { return mode_; }
    void setMode(Mode mode);

    // Outline row interaction (the panel's callbacks; public for tests).
    void setRowExpanded(std::size_t row, bool expanded);
    void activateRow(std::size_t row);
    const std::vector<ui::OutlineRow>& outlineRows() const { return outline_.rows; }

private:
    // Re-flattens the active tab's outline into the panel; requests it
    // (asynchronously) when it is not loaded yet.
    void rebuildOutlineRows();
    void clearOutline();

    ShellContext& context_;
    // Raw pointers into widgets owned by the parent's tree.
    ui::Container* container_ = nullptr;
    ui::Button* pagesButton_ = nullptr;
    ui::Button* outlineButton_ = nullptr;
    ui::PageThumbnailList* thumbnails_ = nullptr;
    ui::OutlinePanel* outlinePanel_ = nullptr;
    Mode mode_ = Mode::Pages;
    core::Rect panelFrame_;

    FlattenedOutline outline_;
    std::set<OutlinePath> expandedPaths_;
    // Document the expansion state belongs to; the top level starts
    // expanded once per document (the user may collapse it afterwards).
    core::DocumentId outlineDocument_;
    bool topLevelExpanded_ = false;
};

} // namespace rivet::app
