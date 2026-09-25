// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "ui/TabStrip.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::ui::TabStrip;
using rivet::ui::testing::FakePaintContext;

namespace {

// A primed strip: paints once so the layout cache exists for hit-testing.
struct Fixture {
    TabStrip strip;
    FakePaintContext context;

    explicit Fixture(std::vector<TabStrip::Tab> tabs, std::optional<std::size_t> active = 0) {
        strip.setTabs(std::move(tabs), active);
        strip.setFrame(Rect{0.0, 0.0, 800.0, TabStrip::kTabHeight});
        strip.paint(context); // primes the cached tab rects
    }

    PointerEvent downAt(Point p) {
        PointerEvent event;
        event.type = PointerEventType::Down;
        event.button = 1;
        event.position = p;
        return event;
    }
    PointerEvent upAt(Point p) {
        PointerEvent event;
        event.type = PointerEventType::Up;
        event.button = 1;
        event.position = p;
        return event;
    }
};

std::vector<TabStrip::Tab> tabsOf(std::initializer_list<const char*> titles) {
    std::vector<TabStrip::Tab> tabs;
    for (const char* title : titles) tabs.push_back(TabStrip::Tab{title});
    return tabs;
}

} // namespace

RIVET_TEST(tabStripInvalidActiveIndexResets) {
    TabStrip strip;
    strip.setTabs(tabsOf({"a"}), 0);
    CHECK_EQ(strip.activeIndex().has_value(), true);

    strip.setActiveIndex(5); // out of range -> nullopt
    CHECK_EQ(strip.activeIndex().has_value(), false);

    strip.setTabs(tabsOf({"a", "b"}), 7); // out of range at set time
    CHECK_EQ(strip.activeIndex().has_value(), false);
}

RIVET_TEST(tabStripLayoutIsLeftToRightAndClamped) {
    Fixture f({{"a"}, {"longer-title-here"}, {"c"}});
    // FakePaintContext measures 7 px per byte: "a" tab is 7 + 12 + 14 = 33 ->
    // clamped to kMinTabWidth; the longer title is 17*7 + 26 = 145 (inside the
    // clamp bounds).
    const double min = TabStrip::kMinTabWidth;
    const double gap = TabStrip::kTabGap;

    // Geometry is reachable via tabIndexAt probes against the primed cache.
    // Tab 0 starts at x=0 with width >= kMinTabWidth.
    CHECK_EQ(f.strip.tabIndexAt(Point{2.0, 14.0}), std::optional<std::size_t>(0));
    // Tab 1 starts right after tab 0's width and the gap.
    CHECK_EQ(f.strip.tabIndexAt(Point{min + gap + 2.0, 14.0}), std::optional<std::size_t>(1));
    // Outside every tab: below the strip body.
    CHECK_EQ(f.strip.tabIndexAt(Point{2.0, 14.0 + 100.0}), std::nullopt);
}

RIVET_TEST(tabStripClickActivates) {
    Fixture f(tabsOf({"a", "b"}), 0);
    std::optional<std::size_t> activated;
    f.strip.setOnTabActivated([&activated](std::size_t index) { activated = index; });

    PointerEvent event = f.downAt(Point{TabStrip::kMinTabWidth + TabStrip::kTabGap + 4.0, 14.0});
    CHECK_EQ(f.strip.onMouse(event), true);
    CHECK_EQ(activated.has_value(), true);
    CHECK_EQ(*activated, std::size_t{1});

    // Clicks outside the tabs are not consumed.
    PointerEvent miss = f.downAt(Point{700.0, 14.0});
    CHECK_EQ(f.strip.onMouse(miss), false);
}

RIVET_TEST(tabStripCloseButtonRequestsCloseOnUpInside) {
    Fixture f(tabsOf({"a", "b"}), 0);
    std::optional<std::size_t> closed;
    f.strip.setOnTabCloseRequested([&closed](std::size_t index) { closed = index; });

    const Rect closeRect = f.strip.closeButtonRect(1);
    CHECK(!closeRect.isEmpty());

    // Down inside the close button, up inside: fires.
    CHECK_EQ(f.strip.onMouse(f.downAt(closeRect.center())), true);
    CHECK_EQ(closed.has_value(), false); // only armed so far
    CHECK_EQ(f.strip.onMouse(f.upAt(closeRect.center())), true);
    CHECK_EQ(closed.has_value(), true);
    CHECK_EQ(*closed, std::size_t{1});

    // Down inside, drag out, up outside: drag-out cancels.
    closed.reset();
    const Rect closeRect0 = f.strip.closeButtonRect(0);
    CHECK_EQ(f.strip.onMouse(f.downAt(closeRect0.center())), true);
    CHECK_EQ(f.strip.onMouse(f.upAt(Point{closeRect0.center().x + 200.0, 14.0})), true);
    CHECK_EQ(closed.has_value(), false);

    // Down on the tab body does not arm the close button; a stray Up with
    // nothing armed is not consumed.
    closed.reset();
    CHECK_EQ(f.strip.onMouse(f.downAt(Point{4.0, 14.0})), true);
    CHECK_EQ(f.strip.onMouse(f.upAt(Point{4.0, 14.0})), false);
    CHECK_EQ(closed.has_value(), false);
}

RIVET_TEST(tabStripOverflowsWithoutCrashing) {
    std::vector<TabStrip::Tab> tabs;
    for (int i = 0; i < 100; ++i) tabs.push_back(TabStrip::Tab{"tab-" + std::to_string(i)});
    Fixture f(std::move(tabs));

    FakePaintContext second;
    f.strip.paint(second); // must not crash

    // Tabs beyond the strip width are not hit-testable.
    CHECK_EQ(f.strip.tabIndexAt(Point{799.0, 14.0}), std::nullopt);
    CHECK_EQ(f.strip.tabIndexAt(Point{2.0, 14.0}), std::optional<std::size_t>(0));
}

RIVET_TEST(tabStripEllipsizesLongTitles) {
    Fixture f({{"a-very-long-document-title-that-will-not-fit-in-one-tab"}});
    // Re-paint and inspect the drawn title: the fake context records it.
    f.context.texts.clear();
    f.strip.paint(f.context);

    bool sawTitle = false;
    for (const auto& text : f.context.texts) {
        if (text.text.find("a-very-long") == 0) {
            sawTitle = true;
            CHECK(text.text != "a-very-long-document-title-that-will-not-fit-in-one-tab");
            const std::string ellipsis = "\xE2\x80\xA6"; // U+2026
            CHECK(text.text.size() < 46 + ellipsis.size() &&
                  text.text != "a-very-long-document-title-that-will-not-fit-in-one-tab");
        }
    }
    CHECK(sawTitle);
}

RIVET_TEST(tabStripEmptyPaintsAndDoesNotCrash) {
    TabStrip strip;
    strip.setFrame(Rect{0.0, 0.0, 800.0, TabStrip::kTabHeight});
    FakePaintContext context;
    strip.paint(context);
    CHECK_EQ(strip.tabIndexAt(Point{4.0, 14.0}), std::nullopt);

    PointerEvent event;
    event.type = PointerEventType::Down;
    event.position = Point{4.0, 14.0};
    CHECK_EQ(strip.onMouse(event), false);
}
