// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "ui/ScrollBar.hpp"
#include "ui/Widget.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::ui {

// One flattened row of an outline tree. The OWNER (app layer) flattens the
// document's outline nodes into rows: depth drives the indent, expanded rows
// reveal their children. The panel is data-driven and UI-agnostic - it never
// sees document types.
struct OutlineRow {
    std::string title;
    int depth = 0;
    bool hasChildren = false;
    bool expanded = false;
};

// Vertical outline list for the sidebar: indented rows, expand/collapse on
// the disclosure marker, click selects + fires onRowActivated. Virtualized
// row painting (uniform row height, only the visible range paints); wheel
// scrolling with an internal ScrollBar.
//
// setRows() replaces the model and preserves the scroll offset when the row
// count allows it. Fired callbacks (main thread):
//   - onRowActivated(index): plain click on a row body.
//   - onExpansionToggled(index, expanded): click on the disclosure marker;
//     the OWNER recomputes the row list (it owns the expansion state).
class OutlinePanel final : public Widget {
public:
    static constexpr double kRowHeight = 24.0;
    static constexpr double kIndent = 14.0;     // per depth level
    static constexpr double kPadding = 8.0;
    static constexpr double kMarkerWidth = 12.0;

    OutlinePanel();
    ~OutlinePanel() override;

    void setRows(std::vector<OutlineRow> rows);
    const std::vector<OutlineRow>& rows() const { return rows_; }

    void setSelectedIndex(std::optional<std::size_t> index);
    std::optional<std::size_t> selectedIndex() const { return selectedIndex_; }

    void setOnRowActivated(std::function<void(std::size_t)> onRowActivated);
    void setOnExpansionToggled(std::function<void(std::size_t, bool)> onExpansionToggled);

    // Scrolls the row into view (shell: current-page -> outline sync).
    void revealRow(std::size_t index);

    bool onMouse(const PointerEvent& event) override;
    void layout() override;
    void paintSelf(PaintContext& context) const override;

    // Current scroll offset (diagnostics/tests).
    double scrollOffset() const { return scrollOffset_; }

private:
    core::Rect rowRect(std::size_t index) const;
    // Disclosure-marker rect of a row (local coordinates).
    core::Rect markerRect(std::size_t index) const;
    std::pair<std::size_t, std::size_t> visibleRowRange() const;
    std::optional<std::size_t> rowIndexAt(const core::Point& localPoint) const;
    double contentHeight() const;
    void setScrollOffset(double offset);
    void syncScrollbar();
    void positionScrollbar();

    std::vector<OutlineRow> rows_;
    std::optional<std::size_t> selectedIndex_;
    std::function<void(std::size_t)> onRowActivated_;
    std::function<void(std::size_t, bool)> onExpansionToggled_;
    double scrollOffset_ = 0.0;
    ScrollBar* scrollBar_ = nullptr;
};

} // namespace rivet::ui
