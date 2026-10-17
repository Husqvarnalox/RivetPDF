// SPDX-License-Identifier: MPL-2.0
#include "Fakes.hpp"

#include "RivetTest.h"

#include "core/StrongId.hpp"
#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Size.hpp"
#include "render/PageLayout.hpp"
#include "ui/PageThumbnailList.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using rivet::core::DocumentId;
using rivet::core::PageId;
using rivet::core::Point;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::render::PageLayout;
using rivet::ui::PageThumbnailList;
using rivet::ui::PointerEvent;
using rivet::ui::PointerEventType;
using rivet::render::testing::FakeRenderSource;

namespace {

PointerEvent downAt(Point position) {
    PointerEvent event;
    event.type = PointerEventType::Down;
    event.button = 1;
    event.position = position;
    return event;
}

PointerEvent scrollBy(double dy) {
    PointerEvent event;
    event.type = PointerEventType::Scroll;
    event.position = Point{80.0, 100.0};
    event.scrollDelta = Point{0.0, dy};
    return event;
}
} // namespace

// Regression: a press is list-local; after the list scrolled, the click must
// select the row drawn under the pointer (the offset was applied twice).
RIVET_TEST(pageThumbnailListClickAfterScrollHitsVisibleRow) {
    PageLayout layout;
    std::vector<PageLayout::PageInfo> pages;
    for (std::uint32_t i = 0; i < 12; ++i) {
        pages.push_back(PageLayout::PageInfo{PageId{100 + i}, Size{612.0, 792.0}});
    }
    layout.setPages(pages);

    FakeRenderSource source;
    PageThumbnailList list;
    list.setFrame(Rect{0.0, 0.0, 200.0, 640.0});
    list.setDocument(DocumentId{1}, &layout, &source, [] { return std::uint64_t{1}; });

    std::optional<std::size_t> clicked;
    list.setOnRowClicked([&clicked](std::size_t row, PageThumbnailList::ClickGesture) {
        clicked = row;
    });

    const double rowHeight = list.rowFrame(1).minY() - list.rowFrame(0).minY();
    list.onMouse(scrollBy(rowHeight * 2.5)); // well past one row
    CHECK_NEAR(list.scrollOffset(), rowHeight * 2.5, 1e-9);

    // Press in the middle of each fully visible row: that exact row is reported.
    for (std::size_t row = 3; row <= 4; ++row) {
        clicked.reset();
        const Rect frame = list.rowFrame(row);
        CHECK(frame.minY() >= 0.0);
        CHECK(frame.maxY() <= 640.0);
        list.onMouse(downAt(Point{80.0, frame.minY() + frame.size.height / 2.0}));
        CHECK_EQ(clicked.has_value(), true);
        if (clicked) CHECK_EQ(*clicked, row);
        // End the press so the next Down starts clean.
        PointerEvent up;
        up.type = PointerEventType::Up;
        up.button = 1;
        up.position = Point{80.0, frame.minY() + frame.size.height / 2.0};
        list.onMouse(up);
    }
}

// QA bugs 4 and 20: unsaved overlays (edited form values, a placed
// signature) are painted over the thumbnail, in thumbnail-local coordinates
// with the thumbnail's zoom, inside a clip.
RIVET_TEST(pageThumbnailListPaintsThePageOverlayOverCachedThumbnails) {
    PageLayout layout;
    layout.setPages({PageLayout::PageInfo{PageId{7}, Size{600.0, 800.0}}});
    FakeRenderSource source;
    PageThumbnailList list;
    list.setFrame(Rect{0.0, 0.0, 200.0, 640.0});
    list.setDocument(DocumentId{1}, &layout, &source, [] { return std::uint64_t{1}; });

    struct Call {
        std::size_t page;
        Rect rect;
        double zoom;
        int clipDepth;
    };
    std::vector<Call> calls;
    list.setPageOverlayPainter(
        [&calls](std::size_t page, const Rect& rect, double zoom, rivet::ui::PaintContext& context) {
            const auto* fake = dynamic_cast<rivet::ui::testing::FakePaintContext*>(&context);
            calls.push_back(Call{page, rect, zoom, fake != nullptr ? fake->clipDepth : -1});
        });

    rivet::ui::testing::FakePaintContext first;
    list.paint(first); // no cached thumbnail yet: a placeholder, no overlay
    CHECK(calls.empty());
    CHECK(!source.requests.empty());
    if (source.requests.empty()) return;
    source.insertTile(source.requests.back().request.key, 4, 4);

    rivet::ui::testing::FakePaintContext second;
    list.paint(second);
    CHECK(!second.bitmaps.empty());
    CHECK_EQ(calls.size(), std::size_t{1});
    if (calls.empty()) return;
    const Call& call = calls.back();
    CHECK_EQ(call.page, std::size_t{0});
    CHECK_NEAR(call.rect.minX(), 0.0, 1e-9);
    CHECK_NEAR(call.rect.minY(), 0.0, 1e-9);
    CHECK_NEAR(call.rect.size.width, PageThumbnailList::kThumbnailWidth, 1e-9);
    CHECK_NEAR(call.zoom, PageThumbnailList::kThumbnailWidth / 600.0, 1e-9);
    CHECK(call.clipDepth >= 2); // the list clip plus the thumbnail clip
}
