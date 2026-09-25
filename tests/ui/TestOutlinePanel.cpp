// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "ui/OutlinePanel.hpp"

#include <cstddef>
#include <optional>
#include <vector>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::ui::OutlinePanel;
using rivet::ui::OutlineRow;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::testing::FakePaintContext;

namespace {
OutlineRow rowOf(const char* title, int depth, bool hasChildren, bool expanded = false) {
    return OutlineRow{title, depth, hasChildren, expanded};
}

PointerEvent downAt(double x, double y) {
    PointerEvent event;
    event.type = PointerEventType::Down;
    event.button = 1;
    event.position = Point{x, y};
    return event;
}
} // namespace

RIVET_TEST(outlinePanelActivatesRows) {
    OutlinePanel panel;
    panel.setFrame(Rect{0.0, 0.0, 200.0, 240.0});
    panel.setRows({rowOf("Chapter 1", 0, true), rowOf("Section 1.1", 1, false),
                   rowOf("Chapter 2", 0, false)});

    std::optional<std::size_t> activated;
    panel.setOnRowActivated([&activated](std::size_t index) { activated = index; });

    // Second row starts at y = kRowHeight.
    CHECK_EQ(panel.onMouse(downAt(60.0, OutlinePanel::kRowHeight + 5.0)), true);
    CHECK_EQ(activated.has_value(), true);
    CHECK_EQ(*activated, std::size_t{1});
    CHECK_EQ(panel.selectedIndex(), std::optional<std::size_t>(1));

    // Empty area below the rows: not consumed.
    CHECK_EQ(panel.onMouse(downAt(60.0, 500.0)), false);
}

RIVET_TEST(outlinePanelExpansionToggle) {
    OutlinePanel panel;
    panel.setFrame(Rect{0.0, 0.0, 200.0, 240.0});
    // Expanded parent: marker click fires the toggle with the new state.
    panel.setRows({rowOf("Chapter 1", 0, true, true)});

    std::optional<std::pair<std::size_t, bool>> toggled;
    panel.setOnExpansionToggled([&toggled](std::size_t index, bool expanded) {
        toggled = {index, expanded};
    });

    // Marker sits at (kPadding, row middle).
    const double markerX = OutlinePanel::kPadding + OutlinePanel::kMarkerWidth / 2.0;
    CHECK_EQ(panel.onMouse(downAt(markerX, OutlinePanel::kRowHeight / 2.0)), true);
    CHECK_EQ(toggled.has_value(), true);
    if (toggled) {
        CHECK_EQ(toggled->first, std::size_t{0});
        CHECK_EQ(toggled->second, false); // was expanded -> now collapses
    }

    // Click on the body (past the marker) activates instead of toggling.
    std::optional<std::size_t> activated;
    panel.setOnRowActivated([&activated](std::size_t index) { activated = index; });
    CHECK_EQ(panel.onMouse(downAt(80.0, OutlinePanel::kRowHeight / 2.0)), true);
    CHECK_EQ(activated.has_value(), true);
    CHECK_EQ(toggled->second, false); // unchanged by the body click
}

RIVET_TEST(outlinePanelVirtualizesLargeOutlines) {
    OutlinePanel panel;
    panel.setFrame(Rect{0.0, 0.0, 200.0, 240.0});
    std::vector<OutlineRow> rows;
    for (int i = 0; i < 10000; ++i) {
        rows.push_back(rowOf("row", 0, false));
    }
    panel.setRows(std::move(rows));

    FakePaintContext context;
    panel.paint(context);
    // Only the visible rows (plus none extra - uniform rows) draw text:
    // 240 / 24 = 10.
    CHECK_GE(context.texts.size(), std::size_t{1});
    CHECK_LE(context.texts.size(), std::size_t{12});
}

RIVET_TEST(outlinePanelRevealScrollsToRow) {
    OutlinePanel panel;
    panel.setFrame(Rect{0.0, 0.0, 200.0, 240.0});
    std::vector<OutlineRow> rows;
    for (int i = 0; i < 100; ++i) rows.push_back(rowOf("row", 0, false));
    panel.setRows(std::move(rows));

    panel.revealRow(80);
    // Row 80's top (80 * 24 = 1920) is beyond the 240pt viewport: the offset
    // clamps to content - viewport = 2400 - 240 = 2160, showing rows through
    // the end.
    CHECK(panel.scrollOffset() >= 1920.0 - 240.0);
    CHECK_LE(panel.scrollOffset(), 2400.0 - 240.0 + 1.0);

    // Reveal a mid-list row: its top becomes the offset exactly.
    panel.revealRow(40);
    CHECK_NEAR(panel.scrollOffset(), 40.0 * OutlinePanel::kRowHeight, 1e-9);
}
