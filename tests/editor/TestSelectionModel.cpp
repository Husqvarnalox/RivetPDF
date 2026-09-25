// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "editor/SelectionModel.hpp"

#include <utility>

using rivet::editor::SelectionModel;
using rivet::editor::TextPosition;

namespace {
constexpr rivet::core::PageId kPageA{1};
constexpr rivet::core::PageId kPageB{2};
} // namespace

RIVET_TEST(selectionStartActivatesWithAnchorAndFocus) {
    SelectionModel selection;
    CHECK(selection.empty());

    int changes = 0;
    selection.setCallback([&changes] { ++changes; });

    selection.start(TextPosition{kPageA, 5});
    CHECK(!selection.empty());
    CHECK(selection.selection().empty()); // active but zero-length
    CHECK_EQ(selection.anchor(), (TextPosition{kPageA, 5}));
    CHECK_EQ(selection.focus(), (TextPosition{kPageA, 5}));
    CHECK_EQ(changes, 1);
}

RIVET_TEST(selectionFocusMovesAndDirectionIsPreserved) {
    SelectionModel selection;
    selection.start(TextPosition{kPageA, 10});
    selection.setFocus(TextPosition{kPageA, 3});
    // Backwards drag: anchor stays at the start point, focus moved.
    CHECK_EQ(selection.anchor(), (TextPosition{kPageA, 10}));
    CHECK_EQ(selection.focus(), (TextPosition{kPageA, 3}));
    CHECK(!selection.selection().empty());

    // Same focus: no callback (no change).
    int changes = 0;
    selection.setCallback([&changes] { ++changes; });
    selection.setFocus(TextPosition{kPageA, 3});
    CHECK_EQ(changes, 0);
}

RIVET_TEST(selectionExtendFromInactiveAnchorsAtThePosition) {
    SelectionModel selection;
    selection.extendTo(TextPosition{kPageB, 7});
    CHECK(!selection.empty());
    CHECK_EQ(selection.anchor(), (TextPosition{kPageB, 7}));
    CHECK_EQ(selection.focus(), (TextPosition{kPageB, 7}));

    // Extend again moves only the focus.
    selection.extendTo(TextPosition{kPageB, 2});
    CHECK_EQ(selection.anchor(), (TextPosition{kPageB, 7}));
    CHECK_EQ(selection.focus(), (TextPosition{kPageB, 2}));
}

RIVET_TEST(selectionClearDeactivates) {
    SelectionModel selection;
    selection.start(TextPosition{kPageA, 1});
    selection.setFocus(TextPosition{kPageB, 9});
    selection.clear();
    CHECK(selection.empty());

    int changes = 0;
    selection.setCallback([&changes] { ++changes; });
    selection.clear();
    CHECK_EQ(changes, 0); // already cleared: no callback
}
