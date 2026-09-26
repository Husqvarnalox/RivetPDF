// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/FakePageDocument.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/geometry/Rotation.hpp"
#include "editor/CommandStack.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageModel.hpp"
#include "editor/PageSelection.hpp"
#include "editor/SelectionText.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Page model, page commands (execute/undo/redo round trips, invalid input,
// zero-page prevention), revisions, snapshots, destination remapping,
// assembly requests, page selection and the text-selection policy. Portable:
// fake backend only.

using rivet::core::ErrorCode;
using rivet::core::PageId;
using rivet::core::PageRotation;
using rivet::editor::CommandStack;
using rivet::editor::CropPagesCommand;
using rivet::editor::DeletePagesCommand;
using rivet::editor::DuplicatePagesCommand;
using rivet::editor::InsertPagesCommand;
using rivet::editor::MovePagesCommand;
using rivet::editor::PageEntry;
using rivet::editor::PageModel;
using rivet::editor::PageModelChange;
using rivet::editor::PageModelSnapshot;
using rivet::editor::PageSelection;
using rivet::editor::PageSnapshotPtr;
using rivet::editor::RotatePagesCommand;
using rivet::pdf::PdfBox;
using rivet::pdf::PdfDestination;
using rivet::test::FakePageDocument;

namespace {

struct Fixture {
    explicit Fixture(std::size_t pages = 5, std::vector<std::size_t> smallPages = {})
        : base(std::make_shared<FakePageDocument>(pages, "PAGE-", std::move(smallPages))) {
        auto created = PageModel::createIdentity(base);
        CHECK(created.has_value());
        model = std::move(*created);
        model->setOnChanged([this](const PageModelChange& change) { changes.push_back(change); });
        original = model->snapshot()->order();
    }

    PageId id(std::size_t index) const { return original.at(index); }
    std::vector<PageId> order() const { return model->snapshot()->order(); }
    const PageEntry& entry(PageId pageId) const { return *model->snapshot()->find(pageId); }

    // Source page numbers (1-based) in model order, for readable asserts.
    std::vector<std::size_t> sources() const {
        std::vector<std::size_t> result;
        for (const PageEntry& e : model->snapshot()->entries()) result.push_back(e.sourcePageIndex + 1);
        return result;
    }

    std::shared_ptr<FakePageDocument> base;
    std::unique_ptr<PageModel> model;
    std::vector<PageModelChange> changes;
    std::vector<PageId> original;
    CommandStack stack;
};

bool failedWith(CommandStack& stack, ErrorCode code) {
    return stack.lastError().has_value() && stack.lastError()->code == code;
}

std::vector<std::size_t> nums(std::initializer_list<std::size_t> items) {
    return std::vector<std::size_t>(items);
}

} // namespace

RIVET_TEST(pageModelIdentityOverBase) {
    Fixture f;
    const PageSnapshotPtr snapshot = f.model->snapshot();
    CHECK_EQ(snapshot->size(), std::size_t{5});
    CHECK(snapshot->isIdentityOrder());
    CHECK_EQ(f.model->orderRevision(), std::uint64_t{1});
    std::set<PageId> ids;
    for (std::size_t i = 0; i < snapshot->size(); ++i) {
        const PageEntry& e = snapshot->at(i);
        CHECK(e.id.value() != 0);
        ids.insert(e.id);
        CHECK_EQ(e.source.get(), f.base.get());
        CHECK_EQ(e.sourcePageIndex, i);
        CHECK_EQ(e.contentRevision, std::uint64_t{0});
        CHECK(e.view == e.nativeView);
        CHECK_EQ(snapshot->indexOf(e.id), i);
    }
    CHECK_EQ(ids.size(), std::size_t{5});
    CHECK_EQ(snapshot->indexOf(PageId{9999}), PageModelSnapshot::kInvalidIndex);

    // A document without pages cannot back a model.
    auto empty = PageModel::createIdentity(std::make_shared<FakePageDocument>(0));
    CHECK(!empty.has_value());
}

RIVET_TEST(pageIdsAreStableAndNeverReused) {
    Fixture f;
    std::set<PageId> everSeen(f.original.begin(), f.original.end());

    // Move keeps ids.
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(4)}, 0)));
    std::vector<PageId> moved = f.order();
    CHECK_EQ(std::set<PageId>(moved.begin(), moved.end()), std::set<PageId>(f.original.begin(), f.original.end()));

    // Delete + undo restores the very same ids.
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(1), f.id(2)})));
    CHECK(!f.model->snapshot()->contains(f.id(1)));
    CHECK(f.stack.undo());
    CHECK(f.order() == moved);

    // Duplicate mints new ids; undo + redo restores the SAME new ids.
    auto duplicate = std::make_unique<DuplicatePagesCommand>(*f.model, std::vector{f.id(0)});
    auto* dup = duplicate.get();
    CHECK(f.stack.execute(std::move(duplicate)));
    const std::vector<PageId> created = dup->createdIds();
    CHECK_EQ(created.size(), std::size_t{1});
    CHECK(everSeen.count(created[0]) == 0);
    everSeen.insert(created[0]);
    CHECK(f.stack.undo());
    CHECK(!f.model->snapshot()->contains(created[0]));
    CHECK(f.stack.redo());
    CHECK(f.model->snapshot()->contains(created[0]));

    // Delete the duplicate for good, duplicate again: a brand-new id.
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, created)));
    auto again = std::make_unique<DuplicatePagesCommand>(*f.model, std::vector{f.id(0)});
    auto* againPtr = again.get();
    CHECK(f.stack.execute(std::move(again)));
    CHECK(everSeen.count(againPtr->createdIds()[0]) == 0);
    CHECK(againPtr->createdIds()[0].value() > created[0].value());
}

RIVET_TEST(movePagesRoundTripPreservesRelativeOrder) {
    Fixture f;
    // Move pages 4 and 2 (given out of order) to the front: relative MODEL
    // order is kept -> 2, 4, 1, 3, 5.
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(3), f.id(1)}, 0)));
    CHECK(f.sources() == nums({2, 4, 1, 3, 5}));
    CHECK(!f.model->snapshot()->isIdentityOrder());
    CHECK(f.stack.undo());
    CHECK(f.order() == f.original);
    CHECK(f.model->snapshot()->isIdentityOrder());
    CHECK(f.stack.redo());
    CHECK(f.sources() == nums({2, 4, 1, 3, 5}));
    CHECK(f.stack.undo());

    // To the end.
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(0), f.id(2)}, 3)));
    CHECK(f.sources() == nums({2, 4, 5, 1, 3}));
    CHECK(f.stack.undo());
    CHECK(f.order() == f.original);

    // Drop gaps: dropping {1,2} into the gap before page 5 (gap 4) lands the
    // block at final index 2.
    const std::vector<PageId> block{f.id(0), f.id(1)};
    CHECK_EQ(MovePagesCommand::destinationForGap(*f.model->snapshot(), block, 4), std::size_t{2});
    CHECK_EQ(MovePagesCommand::destinationForGap(*f.model->snapshot(), block, 0), std::size_t{0});
    CHECK_EQ(MovePagesCommand::destinationForGap(*f.model->snapshot(), block, 5), std::size_t{3});
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(
        *f.model, block, MovePagesCommand::destinationForGap(*f.model->snapshot(), block, 4))));
    CHECK(f.sources() == nums({3, 4, 1, 2, 5}));
}

RIVET_TEST(movePagesRejectsInvalidInputWithoutMutation) {
    Fixture f;
    const std::uint64_t revision = f.model->documentRevision();
    CHECK(!f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(0)}, 5)));
    CHECK(failedWith(f.stack, ErrorCode::InvalidArgument));
    CHECK(!f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(0), PageId{777}}, 0)));
    CHECK(failedWith(f.stack, ErrorCode::NotFound));
    CHECK(!f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(0), f.id(0)}, 0)));
    CHECK(!f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector<PageId>{}, 0)));
    CHECK(f.order() == f.original);
    CHECK_EQ(f.model->documentRevision(), revision);
    CHECK(f.changes.empty());
    CHECK(!f.stack.canUndo());
}

RIVET_TEST(deletePagesRoundTripAndZeroPagePrevention) {
    Fixture f;
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(4), f.id(0), f.id(2)})));
    CHECK(f.sources() == nums({2, 4}));
    CHECK(f.stack.undo());
    CHECK(f.order() == f.original);
    CHECK(f.stack.redo());
    CHECK(f.sources() == nums({2, 4}));

    // Deleting every remaining page is refused, model untouched.
    const std::vector<PageId> before = f.order();
    CHECK(!f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, before)));
    CHECK(failedWith(f.stack, ErrorCode::InvalidArgument));
    CHECK(f.order() == before);
    // Unknown id in the set: nothing deleted (all-or-nothing).
    CHECK(!f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{before[0], PageId{4242}})));
    CHECK(f.order() == before);
    // Down to one page is fine; then the last page cannot go.
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{before[0]})));
    CHECK_EQ(f.model->size(), std::size_t{1});
    CHECK(!f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, f.order())));
    CHECK_EQ(f.model->size(), std::size_t{1});
    // Moving the only page anywhere but index 0 is out of range.
    CHECK(!f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, f.order(), 1)));
}

RIVET_TEST(rotatePagesRoundTripAndRevisions) {
    Fixture f;
    const std::uint64_t orderRevision = f.model->orderRevision();
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0), f.id(1)}, 90)));
    const PageEntry rotated0 = f.entry(f.id(0));
    const PageEntry rotated1 = f.entry(f.id(1));
    CHECK(rotated0.view.rotation == PageRotation::Clockwise90);
    CHECK(rotated0.contentRevision != 0);
    CHECK(rotated1.contentRevision != 0);
    CHECK(rotated0.contentRevision != rotated1.contentRevision);
    CHECK_EQ(f.entry(f.id(2)).contentRevision, std::uint64_t{0});
    CHECK_EQ(f.model->orderRevision(), orderRevision); // not structural
    CHECK_EQ(f.changes.back().contentChanged.size(), std::size_t{2});
    CHECK(!f.changes.back().orderChanged);

    // -90 on page 1 goes to 0 degrees with yet another fresh revision.
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0)}, -90)));
    CHECK(f.entry(f.id(0)).view.rotation == PageRotation::None);
    CHECK(f.entry(f.id(0)).contentRevision != rotated0.contentRevision);
    CHECK(f.entry(f.id(0)).contentRevision != 0);

    // Undo restores the EXACT previous (view, contentRevision) pairs.
    CHECK(f.stack.undo());
    CHECK(f.entry(f.id(0)).view == rotated0.view);
    CHECK_EQ(f.entry(f.id(0)).contentRevision, rotated0.contentRevision);
    CHECK(f.stack.undo());
    CHECK(f.entry(f.id(0)).view == f.entry(f.id(0)).nativeView);
    CHECK_EQ(f.entry(f.id(0)).contentRevision, std::uint64_t{0});
    CHECK(f.stack.redo());
    CHECK_EQ(f.entry(f.id(1)).contentRevision, rotated1.contentRevision);

    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(3)}, 270)));
    CHECK(f.entry(f.id(3)).view.rotation == PageRotation::Clockwise270);
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(3)}, -180)));
    CHECK(f.entry(f.id(3)).view.rotation == PageRotation::Clockwise90);

    // Invalid angles and ids: refused, nothing changes.
    const PageSnapshotPtr before = f.model->snapshot();
    for (const int degrees : {0, 45, 360, -720}) {
        CHECK(!f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0)}, degrees)));
        CHECK(failedWith(f.stack, ErrorCode::InvalidArgument));
    }
    CHECK(!f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0), PageId{999}}, 90)));
    CHECK_EQ(f.model->snapshot().get(), before.get());
}

RIVET_TEST(duplicatePagesRoundTrip) {
    Fixture f;
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(1)}, 180)));
    auto command = std::make_unique<DuplicatePagesCommand>(*f.model, std::vector{f.id(3), f.id(1)});
    auto* dup = command.get();
    CHECK(f.stack.execute(std::move(command)));
    // Each copy right after its original.
    CHECK(f.sources() == nums({1, 2, 2, 3, 4, 4, 5}));
    const auto& created = dup->createdIds();
    CHECK_EQ(created.size(), std::size_t{2});
    CHECK_EQ(f.model->snapshot()->indexOf(created[0]), std::size_t{2});
    CHECK_EQ(f.model->snapshot()->indexOf(created[1]), std::size_t{5});
    // The copy carries the original's CURRENT view; new id -> revision 0.
    CHECK(f.entry(created[0]).view.rotation == PageRotation::Clockwise180);
    CHECK_EQ(f.entry(created[0]).contentRevision, std::uint64_t{0});
    CHECK_EQ(f.changes.back().added.size(), std::size_t{2});

    CHECK(f.stack.undo());
    CHECK(f.sources() == nums({1, 2, 3, 4, 5}));
    CHECK(f.stack.redo());
    CHECK(f.sources() == nums({1, 2, 2, 3, 4, 4, 5}));
    CHECK_EQ(f.model->snapshot()->indexOf(created[1]), std::size_t{5});

    CHECK(!f.stack.execute(std::make_unique<DuplicatePagesCommand>(*f.model, std::vector{PageId{5555}})));
    CHECK_EQ(f.model->size(), std::size_t{7});
}

RIVET_TEST(insertPagesFromAnotherDocument) {
    Fixture f;
    auto other = std::make_shared<FakePageDocument>(3, "IMPORT-");
    auto pages = PageModel::describeAllPages(other);
    CHECK(pages.has_value());
    auto command = std::make_unique<InsertPagesCommand>(*f.model, *pages, 2);
    auto* insert = command.get();
    CHECK(f.stack.execute(std::move(command)));
    CHECK_EQ(f.model->size(), std::size_t{8});
    const auto& created = insert->createdIds();
    CHECK_EQ(created.size(), std::size_t{3});
    for (std::size_t i = 0; i < 3; ++i) {
        const PageEntry& e = f.model->snapshot()->at(2 + i);
        CHECK_EQ(e.id, created[i]);
        CHECK_EQ(e.source.get(), other.get());
        CHECK_EQ(e.sourcePageIndex, i);
    }
    // The model keeps the imported document alive.
    FakePageDocument* raw = other.get();
    other.reset();
    CHECK_EQ(f.model->snapshot()->at(2).source.get(), static_cast<rivet::pdf::PdfDocument*>(raw));

    CHECK(f.stack.undo());
    CHECK(f.order() == f.original);
    CHECK(f.stack.redo());
    CHECK_EQ(f.model->snapshot()->indexOf(created[2]), std::size_t{4});

    // Out-of-range position, empty list: refused.
    CHECK(!f.stack.execute(std::make_unique<InsertPagesCommand>(*f.model, *pages, 99)));
    CHECK(!f.stack.execute(std::make_unique<InsertPagesCommand>(*f.model, std::vector<rivet::editor::PageSource>{}, 0)));
    CHECK_EQ(f.model->size(), std::size_t{8});

    // At the very end.
    CHECK(f.stack.execute(std::make_unique<InsertPagesCommand>(*f.model, *pages, 8)));
    CHECK_EQ(f.model->snapshot()->at(10).sourcePageIndex, std::size_t{2});
}

RIVET_TEST(cropPagesValidatesAgainstMediaBoxAllOrNothing) {
    Fixture f(5, {4}); // page 5 has a smaller 300x400 media box
    const PdfBox crop{50.0, 80.0, 500.0, 700.0};
    CHECK(f.stack.execute(std::make_unique<CropPagesCommand>(*f.model, std::vector{f.id(0)}, crop)));
    CHECK(f.entry(f.id(0)).view.cropBox == crop);
    CHECK(f.entry(f.id(0)).contentRevision != 0);
    CHECK(f.entry(f.id(0)).view.rotation == PageRotation::None);

    // Page 5 cannot take that crop: the whole multi-page crop fails and page
    // 2 is NOT cropped either.
    const PageSnapshotPtr before = f.model->snapshot();
    CHECK(!f.stack.execute(std::make_unique<CropPagesCommand>(*f.model, std::vector{f.id(1), f.id(4)}, crop)));
    CHECK(failedWith(f.stack, ErrorCode::InvalidArgument));
    CHECK_EQ(f.model->snapshot().get(), before.get());
    // Degenerate / inverted boxes are rejected.
    CHECK(!f.stack.execute(std::make_unique<CropPagesCommand>(*f.model, std::vector{f.id(1)}, PdfBox{10, 10, 10, 50})));
    CHECK(!f.stack.execute(std::make_unique<CropPagesCommand>(*f.model, std::vector{f.id(1)}, PdfBox{100, 10, 50, 50})));

    // Rotation is kept by a crop; reset (nullopt) restores the native crop.
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0)}, 90)));
    CHECK(f.stack.execute(std::make_unique<CropPagesCommand>(*f.model, std::vector{f.id(0)}, std::nullopt)));
    CHECK(f.entry(f.id(0)).view.cropBox == f.entry(f.id(0)).nativeView.cropBox);
    CHECK(f.entry(f.id(0)).view.rotation == PageRotation::Clockwise90);

    CHECK(f.stack.undo());
    CHECK(f.entry(f.id(0)).view.cropBox == crop);
    CHECK(f.stack.undo());
    CHECK(f.stack.undo());
    CHECK(f.entry(f.id(0)).view == f.entry(f.id(0)).nativeView);
    CHECK_EQ(f.entry(f.id(0)).contentRevision, std::uint64_t{0});
}

RIVET_TEST(revisionsAreMonotonicAcrossUndoRedo) {
    Fixture f;
    std::uint64_t lastDocument = f.model->documentRevision();
    std::uint64_t lastOrder = f.model->orderRevision();
    const auto step = [&](bool structural) {
        CHECK(f.model->documentRevision() > lastDocument);
        if (structural) {
            CHECK(f.model->orderRevision() > lastOrder);
        } else {
            CHECK_EQ(f.model->orderRevision(), lastOrder);
        }
        lastDocument = f.model->documentRevision();
        lastOrder = f.model->orderRevision();
        CHECK_EQ(f.model->snapshot()->documentRevision(), lastDocument);
        CHECK_EQ(f.model->snapshot()->orderRevision(), lastOrder);
    };
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(0)}, 4)));
    step(true);
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0)}, 90)));
    step(false);
    CHECK(f.stack.undo());
    step(false);
    CHECK(f.stack.undo());
    step(true);
    CHECK(f.stack.redo());
    step(true);
    CHECK(f.stack.redo());
    step(false);
}

RIVET_TEST(snapshotsAreImmutable) {
    Fixture f;
    const PageSnapshotPtr before = f.model->snapshot();
    const std::vector<PageId> beforeOrder = before->order();
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(0)}, 4)));
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(1)}, 90)));
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(2)})));
    CHECK(before->order() == beforeOrder);
    CHECK(before->find(f.id(1))->view.rotation == PageRotation::None);
    CHECK(before->contains(f.id(2)));
    CHECK(before.get() != f.model->snapshot().get());
    CHECK(f.changes.size() == 3);
    CHECK_EQ(f.changes.back().removed.size(), std::size_t{1});
    CHECK_EQ(f.changes.back().removed[0], f.id(2));
    CHECK_EQ(f.changes.back().previous->size(), std::size_t{5});
}

RIVET_TEST(destinationsRemapThroughModel) {
    Fixture f;
    PdfDestination destination;
    destination.pageIndex = 2;
    destination.hasPoint = true;
    destination.point = rivet::core::Point{100.0, 692.0}; // native display point
    destination.hasUserPoint = true;
    destination.userX = 100.0;
    destination.userY = 100.0;

    auto resolved = f.model->snapshot()->resolveDestination(f.base.get(), destination);
    CHECK(resolved.has_value());
    CHECK_EQ(resolved->page, f.id(2));
    CHECK_EQ(resolved->index, std::size_t{2});
    CHECK(resolved->hasPoint);
    CHECK_NEAR(resolved->point.x, 100.0, 1e-9);
    CHECK_NEAR(resolved->point.y, 692.0, 1e-9);

    // Moved: same page, new index.
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(2)}, 0)));
    resolved = f.model->snapshot()->resolveDestination(f.base.get(), destination);
    CHECK(resolved.has_value() && resolved->page == f.id(2) && resolved->index == 0);

    // Rotated: the point is re-mapped through the new view (90 cw: x' = y0,
    // y' = x0 in user offsets).
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(2)}, 90)));
    resolved = f.model->snapshot()->resolveDestination(f.base.get(), destination);
    CHECK(resolved.has_value() && resolved->hasPoint);
    CHECK_NEAR(resolved->point.x, 100.0, 1e-9);
    CHECK_NEAR(resolved->point.y, 100.0, 1e-9);
    // Without a user point, a display point cannot survive a non-native view.
    PdfDestination displayOnly = destination;
    displayOnly.hasUserPoint = false;
    resolved = f.model->snapshot()->resolveDestination(f.base.get(), displayOnly);
    CHECK(resolved.has_value() && !resolved->hasPoint);

    // A duplicate never steals the destination (first entry wins) ...
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(2)}, 4)));
    auto duplicate = std::make_unique<DuplicatePagesCommand>(*f.model, std::vector{f.id(2)});
    CHECK(f.stack.execute(std::move(duplicate)));
    resolved = f.model->snapshot()->resolveDestination(f.base.get(), destination);
    CHECK(resolved.has_value() && resolved->page == f.id(2));
    // ... but when the original is deleted, the copy becomes the target.
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(2)})));
    resolved = f.model->snapshot()->resolveDestination(f.base.get(), destination);
    CHECK(resolved.has_value() && resolved->page != f.id(2));
    CHECK(f.stack.undo());
    CHECK(f.stack.undo());
    // Deleted target -> no destination; unknown source document -> none.
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(2)})));
    CHECK(!f.model->snapshot()->resolveDestination(f.base.get(), destination).has_value());
    FakePageDocument stranger(3);
    destination.pageIndex = 0;
    CHECK(!f.model->snapshot()->resolveDestination(&stranger, destination).has_value());
}

RIVET_TEST(assemblyRequestFromModel) {
    Fixture f;
    auto other = std::make_shared<FakePageDocument>(2, "IMPORT-");
    auto pages = PageModel::describeAllPages(other);
    CHECK(pages.has_value());
    CHECK(f.stack.execute(std::make_unique<InsertPagesCommand>(*f.model, *pages, 1)));
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0)}, 90)));
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(3)})));

    using Mode = PageModelSnapshot::AssemblyMode;
    auto save = f.model->toAssemblyRequest(Mode::Save);
    CHECK(save.has_value());
    CHECK(save->mode == rivet::pdf::PdfAssemblyRequest::Mode::PreserveBase);
    CHECK_EQ(save->base, static_cast<const rivet::pdf::PdfDocument*>(f.base.get()));
    CHECK_EQ(save->pages.size(), std::size_t{6});
    CHECK_EQ(save->pages[0].source, static_cast<const rivet::pdf::PdfDocument*>(f.base.get()));
    CHECK(save->pages[0].view.rotation == PageRotation::Clockwise90);
    CHECK_EQ(save->pages[1].source, static_cast<const rivet::pdf::PdfDocument*>(other.get()));
    CHECK_EQ(save->pages[3].sourcePageIndex, std::size_t{1});
    CHECK_EQ(save->pages[4].sourcePageIndex, std::size_t{2}); // page 4 deleted

    // Extract: subset in MODEL order regardless of the given order.
    const std::vector<PageId> subset{f.id(4), f.id(0)};
    auto extract = f.model->toAssemblyRequest(Mode::Extract, subset);
    CHECK(extract.has_value());
    CHECK(extract->mode == rivet::pdf::PdfAssemblyRequest::Mode::Fresh);
    CHECK_EQ(extract->pages.size(), std::size_t{2});
    CHECK_EQ(extract->pages[0].sourcePageIndex, std::size_t{0});
    CHECK_EQ(extract->pages[1].sourcePageIndex, std::size_t{4});
    CHECK(!f.model->toAssemblyRequest(Mode::Extract).has_value());
    const std::vector<PageId> deleted{f.id(3)};
    CHECK(!f.model->toAssemblyRequest(Mode::Extract, deleted).has_value());
}

RIVET_TEST(pageSelectionRules) {
    Fixture f;
    PageSelection selection;
    selection.select(f.id(1));
    CHECK_EQ(selection.count(), std::size_t{1});
    CHECK_EQ(selection.active(), f.id(1));
    selection.toggle(f.id(3));
    CHECK_EQ(selection.count(), std::size_t{2});
    CHECK_EQ(selection.active(), f.id(3));
    selection.toggle(f.id(3));
    CHECK(!selection.contains(f.id(3)));
    CHECK_EQ(selection.active(), f.id(3)); // focus stays on the toggled page

    // Range in CURRENT order: after moving page 5 to the front, a range from
    // (new) first to page 2 covers 5, 1, 2.
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(4)}, 0)));
    selection.select(f.id(4));
    selection.selectRange(f.id(1), *f.model->snapshot());
    CHECK((selection.inOrder(*f.model->snapshot()) == std::vector{f.id(4), f.id(0), f.id(1)}));
    CHECK_EQ(selection.anchor(), f.id(4));
    CHECK_EQ(selection.active(), f.id(1));
    // Shift-range backwards from the same anchor replaces the range.
    selection.selectRange(f.id(4), *f.model->snapshot());
    CHECK_EQ(selection.count(), std::size_t{1});

    selection.selectAll(*f.model->snapshot());
    CHECK_EQ(selection.count(), std::size_t{5});

    // Moves keep the selection.
    selection.select(f.id(2));
    selection.toggle(f.id(3));
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(2)}, 0)));
    selection.applyChange(f.changes.back());
    CHECK(selection.contains(f.id(2)) && selection.contains(f.id(3)));

    // Deleting the active page: dropped, active -> nearest surviving (the
    // next page in the previous order), anchor follows.
    // Current order: 3, 5, 1, 2, 4. Delete 4 (active, last) -> active = 2.
    CHECK((f.sources() == nums({3, 5, 1, 2, 4})));
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(3)})));
    selection.applyChange(f.changes.back());
    CHECK(!selection.contains(f.id(3)));
    CHECK_EQ(selection.active(), f.id(1));
    CHECK_EQ(selection.anchor(), f.id(1));
    CHECK_EQ(selection.count(), std::size_t{1}); // page 3 still selected
    CHECK(selection.contains(f.id(2)));

    // Delete a middle active page: the NEXT page takes focus; an emptied
    // selection selects it.
    selection.select(f.id(0));
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(0)})));
    selection.applyChange(f.changes.back());
    CHECK_EQ(selection.active(), f.id(1));
    CHECK(selection.contains(f.id(1)));
    CHECK_EQ(selection.count(), std::size_t{1});
}

RIVET_TEST(textSelectionPolicyAfterEdits) {
    Fixture f;
    rivet::editor::TextSelection selection;
    selection.anchor = {f.id(1), 2};
    selection.focus = {f.id(3), 1};

    // Moving untouched pages keeps it.
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, std::vector{f.id(4)}, 0)));
    CHECK(!rivet::editor::textSelectionInvalidatedBy(selection, f.changes.back()));
    // Rotating a page outside the range keeps it; inside the range clears.
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(0)}, 90)));
    CHECK(!rivet::editor::textSelectionInvalidatedBy(selection, f.changes.back()));
    CHECK(f.stack.execute(std::make_unique<RotatePagesCommand>(*f.model, std::vector{f.id(2)}, 90)));
    CHECK(rivet::editor::textSelectionInvalidatedBy(selection, f.changes.back()));
    // Deleting an endpoint clears.
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, std::vector{f.id(3)})));
    CHECK(rivet::editor::textSelectionInvalidatedBy(selection, f.changes.back()));
}

RIVET_TEST(movingHalfOf500PagesIsLinear) {
    // Complexity smoke test, no timing assertions: 500 pages, move every
    // other page (250) to the front and back, 40 execute/undo/redo cycles.
    // With an O(N^2) implementation (e.g. per-page erase/insert with index
    // scans) this would be ~10^8 operations; the linear one is ~10^5.
    Fixture f(500);
    std::vector<PageId> odd;
    for (std::size_t i = 1; i < 500; i += 2) odd.push_back(f.id(i));
    for (int round = 0; round < 40; ++round) {
        CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, odd, 0)));
        CHECK(f.stack.undo());
        CHECK(f.stack.redo());
        CHECK(f.stack.undo());
    }
    CHECK(f.order() == f.original);
    CHECK(f.stack.execute(std::make_unique<MovePagesCommand>(*f.model, odd, 250)));
    CHECK_EQ(f.model->snapshot()->indexOf(f.id(1)), std::size_t{250});
    CHECK_EQ(f.model->snapshot()->indexOf(f.id(0)), std::size_t{0});
    CHECK_EQ(f.model->snapshot()->indexOf(f.id(2)), std::size_t{1});
    CHECK(f.stack.execute(std::make_unique<DeletePagesCommand>(*f.model, odd)));
    CHECK_EQ(f.model->size(), std::size_t{250});
    CHECK(f.stack.undo());
    CHECK(f.stack.undo());
    CHECK(f.order() == f.original);
}
