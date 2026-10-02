// SPDX-License-Identifier: MPL-2.0
// Phase 5 content editing, end to end: the REAL engine (PDFium when built in)
// behind the real app shell (ContentController over the editor backend,
// DocumentSession, FileController, PrintCoordinator). Fixtures are generated
// PDFs (tests/pdf/PdfFixtures.hpp). Bodies return early without a backend.
#include "ContentIntegrationKit.hpp"

using namespace rivet;
using namespace rivet::test::integ;
using core::ObjectId;
using core::Point;
using core::Rect;
using pdf::PdfContentObjectType;

namespace {

Point centerOf(const Rect& rect) { return rect.center(); }

} // namespace

// 1. Select + move a text block on a page that also carries a Square and a
// Link annotation over it.
RIVET_TEST(integSelectHitsTheTextBlockAndMoveIsOneUndoStep) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich();
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.selectTool();
    const auto view = rig.loaded(0);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    const auto* hello = rig.blockWith(*view, "Hello World");
    CHECK(hello != nullptr);
    if (hello == nullptr) return;
    const ObjectId blockId = hello->id;
    const Rect before = hello->bounds;
    const double userXBefore = [&] {
        const auto* object = rig.objectOfType(*view, PdfContentObjectType::Text, 0);
        return object != nullptr ? object->source.matrix.tx : -1.0;
    }();
    const double userYBefore = [&] {
        const auto* object = rig.objectOfType(*view, PdfContentObjectType::Text, 0);
        return object != nullptr ? object->source.matrix.ty : -1.0;
    }();

    // The Square annotation and the Link both cover this point; the content
    // tool still selects the text block.
    const Point at = centerOf(before);
    rig.click(at);
    const auto selected = rig.content->selected();
    CHECK(selected.has_value());
    if (!selected.has_value()) return;
    CHECK(selected->info.id == blockId);
    CHECK(selected->info.isBlock);
    CHECK(selected->info.kind == app::ContentInteraction::Kind::Text);
    CHECK(rig.annotations->suspended());
    CHECK(!rig.annotations->selectedId().has_value());
    CHECK_EQ(rig.depth(), std::size_t{0});

    const PageStamp page0 = rig.stamp(0);
    const PageStamp page1 = rig.stamp(1);
    const std::size_t depth0 = rig.depth();
    rig.drag(at, Point{at.x + 20.0, at.y + 10.0});
    CHECK_EQ(rig.depth(), depth0 + 1);
    CHECK(rig.session().isDirty());

    // The page re-extracts with the block moved by (20, 10) display points,
    // i.e. (+20, -10) in user space, under the same ObjectId.
    CHECK(rig.dispatcher.waitUntil([&] {
        const auto now = rig.contentNow(0);
        if (now == nullptr || !now->loaded) return false;
        const auto* moved = rig.blockWith(*now, "Hello World");
        return moved != nullptr && std::abs(moved->bounds.minX() - (before.minX() + 20.0)) < 0.5;
    }));
    {
        const auto now = rig.contentNow(0);
        const auto* moved = rig.blockWith(*now, "Hello World");
        CHECK(moved != nullptr);
        if (moved != nullptr) {
            CHECK(moved->id == blockId);
            CHECK_NEAR(moved->bounds.minY(), before.minY() + 10.0, 0.5);
            CHECK_NEAR(moved->bounds.size.width, before.size.width, 0.5);
        }
        const auto* object = rig.objectOfType(*now, PdfContentObjectType::Text, 0);
        CHECK(object != nullptr);
        if (object != nullptr) {
            CHECK_NEAR(object->source.matrix.tx, userXBefore + 20.0, 0.01);
            CHECK_NEAR(object->source.matrix.ty, userYBefore - 10.0, 0.01);
        }
    }
    // Only the edited page changed: page 1 untouched, annotation state kept.
    const PageStamp page0After = rig.stamp(0);
    CHECK(page0After.content != page0.content);
    CHECK(page0After.raster != page0.raster);
    CHECK(page0After.annotations == page0.annotations);
    CHECK(rig.stamp(1) == page1);

    // Undo restores the position and the identity; redo reapplies.
    CHECK(rig.session().undo());
    CHECK_EQ(rig.depth(), depth0);
    CHECK(rig.dispatcher.waitUntil([&] {
        const auto now = rig.contentNow(0);
        if (now == nullptr || !now->loaded) return false;
        const auto* back = rig.blockWith(*now, "Hello World");
        return back != nullptr && std::abs(back->bounds.minX() - before.minX()) < 0.5;
    }));
    {
        const auto now = rig.contentNow(0);
        const auto* back = rig.blockWith(*now, "Hello World");
        CHECK(back != nullptr);
        if (back != nullptr) {
            CHECK(back->id == blockId);
            CHECK_NEAR(back->bounds.minY(), before.minY(), 0.5);
        }
    }
    CHECK(!rig.session().isDirty());
    CHECK(rig.session().redo());
    CHECK(rig.dispatcher.waitUntil([&] {
        const auto now = rig.contentNow(0);
        if (now == nullptr || !now->loaded) return false;
        const auto* again = rig.blockWith(*now, "Hello World");
        return again != nullptr && std::abs(again->bounds.minX() - (before.minX() + 20.0)) < 0.5;
    }));
    CHECK(rig.session().isDirty());
}
