// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/FakePageDocument.hpp"

#include "core/Error.hpp"
#include "core/StrongId.hpp"
#include "core/async/TaskScheduler.hpp"
#include "core/geometry/Rotation.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/TextSearchController.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"
#include "render/RenderSource.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// DocumentSession over the page model: delegation, dirty tracking, cache
// keys/invalidation per (PageId, contentRevision), worker jobs over
// snapshots (TSan target), destination resolution and search restart.
// Portable: fake backend, no dispatcher (deliveries inline on workers).

using rivet::core::ErrorCode;
using rivet::core::PageId;
using rivet::core::PageRotation;
using rivet::core::Rect;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DeletePagesCommand;
using rivet::editor::DocumentSession;
using rivet::editor::DuplicatePagesCommand;
using rivet::editor::MovePagesCommand;
using rivet::editor::PageModelChange;
using rivet::editor::RotatePagesCommand;
using rivet::editor::TextSearchController;
using rivet::render::PhysicalRenderScaleKey;
using rivet::render::RenderPriority;
using rivet::render::RenderRequest;
using rivet::render::RenderResult;
using rivet::test::FakePageDocument;
using rivet::test::FakePageEngine;

namespace {

struct Fixture {
    explicit Fixture(std::size_t pages = 5) : engine(pages) {
        auto created = DocumentSession::create(engine, scheduler, nullptr, "fake.pdf");
        CHECK(created.has_value());
        session = std::move(*created);
        document = engine.lastDocument;
        for (std::size_t i = 0; i < session->pageCount(); ++i) ids.push_back(session->pageId(i));
    }

    PageId id(std::size_t index) const { return ids.at(index); }

    template <typename C, typename... Args>
    bool run(Args&&... args) {
        return session->execute(std::make_unique<C>(session->pageModel(), std::forward<Args>(args)...)).has_value();
    }

    RenderRequest request(PageId page) const {
        const auto* entry = session->pageSnapshot()->find(page);
        RenderRequest r;
        r.key.documentId = session->id();
        r.key.pageId = page;
        r.key.scale = PhysicalRenderScaleKey::fromDensities(0.25, 1.0);
        r.key.contentRevision = entry != nullptr ? entry->contentRevision : 0;
        const Size size = entry != nullptr ? rivet::pdf::displaySize(entry->view) : Size{10, 10};
        r.params.pageRectPoints = Rect{0.0, 0.0, size.width, size.height};
        r.params.devicePixelsPerPoint = r.key.scale.scale();
        return r;
    }

    RenderResult render(const RenderRequest& r) {
        auto promise = std::make_shared<std::promise<RenderResult>>();
        auto future = promise->get_future();
        session->renderSource().requestRender(r, RenderPriority::Visible,
                                              [promise](RenderResult result) { promise->set_value(std::move(result)); });
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
            return std::unexpected(rivet::core::makeError(ErrorCode::Internal, "timeout", "test"));
        }
        return future.get();
    }

    std::shared_ptr<const rivet::pdf::PdfTextPage> text(PageId page) {
        auto promise = std::make_shared<std::promise<std::shared_ptr<const rivet::pdf::PdfTextPage>>>();
        auto future = promise->get_future();
        session->textService().requestTextPage(page, [promise](auto result) { promise->set_value(result); });
        if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) return nullptr;
        return future.get();
    }

    FakePageEngine engine;
    TaskScheduler scheduler{3};
    std::unique_ptr<DocumentSession> session;
    FakePageDocument* document = nullptr;
    std::vector<PageId> ids;
};

template <typename Predicate>
bool waitFor(Predicate predicate, int attempts = 1000) {
    for (int i = 0; i < attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

} // namespace

RIVET_TEST(sessionDelegatesToPageModel) {
    Fixture f;
    CHECK_EQ(f.session->pageCount(), std::size_t{5});
    CHECK_EQ(f.session->pageLabel(0), std::string("L1"));

    CHECK(f.run<MovePagesCommand>(std::vector{f.id(4)}, std::size_t{0}));
    CHECK_EQ(f.session->pageIndexFor(f.id(4)), std::size_t{0});
    CHECK_EQ(f.session->pageId(1), f.id(0));
    CHECK((f.session->pageOrder() == std::vector{f.id(4), f.id(0), f.id(1), f.id(2), f.id(3)}));
    CHECK_EQ(f.session->layout().pages()[0].id, f.id(4));
    // Labels only while the order is the identity: positional fallback now.
    CHECK_EQ(f.session->pageLabel(0), std::string());
    CHECK_EQ(f.session->pageLabels().size(), std::size_t{5});
    CHECK_EQ(f.session->pageLabels()[0], std::string());

    CHECK(f.run<RotatePagesCommand>(std::vector{f.id(0)}, 90));
    CHECK(Size::nearlyEqual(f.session->pageSizePoints(1), Size{792.0, 612.0}));
    CHECK(Size::nearlyEqual(f.session->layout().pages()[1].sizePoints, Size{792.0, 612.0}));
    CHECK(f.session->layout().pages()[1].rotation == PageRotation::Clockwise90);

    CHECK(f.session->undo());
    CHECK(f.session->undo());
    CHECK_EQ(f.session->pageLabel(0), std::string("L1"));
    CHECK_EQ(f.session->pageLabels()[4], std::string("L5"));

    // Delete: count shrinks, id unknown afterwards.
    CHECK(f.run<DeletePagesCommand>(std::vector{f.id(2)}));
    CHECK_EQ(f.session->pageCount(), std::size_t{4});
    CHECK_EQ(f.session->pageIndexFor(f.id(2)), DocumentSession::kInvalidPage);
    CHECK_EQ(f.session->layout().pageCount(), std::size_t{4});

    // Failed command: error surfaces, nothing changes, not recorded.
    const auto status = f.session->execute(
        std::make_unique<DeletePagesCommand>(f.session->pageModel(), f.session->pageOrder()));
    CHECK(!status.has_value());
    CHECK_EQ(status.error().code, ErrorCode::InvalidArgument);
    CHECK_EQ(f.session->pageCount(), std::size_t{4});
}

RIVET_TEST(sessionDirtyTracking) {
    Fixture f;
    std::vector<bool> notifications;
    f.session->setOnDirtyChanged([&](bool dirty) { notifications.push_back(dirty); });
    CHECK(!f.session->isDirty());

    CHECK(f.run<RotatePagesCommand>(std::vector{f.id(0)}, 90)); // A
    CHECK(f.session->isDirty());
    CHECK(f.session->undo());
    CHECK(!f.session->isDirty()); // back to the opened state
    CHECK(f.session->redo());
    CHECK(f.session->isDirty());
    CHECK((notifications == std::vector<bool>{true, false, true}));

    f.session->markSaved(); // saved at A
    CHECK(!f.session->isDirty());
    CHECK(f.session->undo());
    CHECK(f.session->isDirty());
    CHECK(f.session->redo());
    CHECK(!f.session->isDirty()); // redo back to the saved state

    // New command after an undo never falsely matches the saved state.
    CHECK(f.session->undo());          // state 0, dirty
    CHECK(f.run<RotatePagesCommand>(std::vector{f.id(0)}, 90)); // B: same effect as A
    CHECK(f.session->isDirty());
    CHECK(!f.session->commands().canRedo()); // A is gone for good
    CHECK(f.session->undo());
    CHECK(f.session->isDirty()); // state 0 != saved (A)

    // Failed commands never change dirtiness.
    f.session->markSaved();
    const std::size_t before = notifications.size();
    CHECK(!f.run<RotatePagesCommand>(std::vector{f.id(0)}, 45));
    CHECK(!f.session->isDirty());
    CHECK_EQ(notifications.size(), before);

    // The document revision never goes backwards.
    const std::uint64_t revision = f.session->documentRevision();
    CHECK(f.run<RotatePagesCommand>(std::vector{f.id(1)}, 90));
    CHECK(f.session->undo());
    CHECK(f.session->documentRevision() > revision + 1);
}

RIVET_TEST(renderTilesKeyedByContentRevision) {
    Fixture f;
    const RenderRequest a = f.request(f.id(0));
    const RenderRequest b = f.request(f.id(1));
    CHECK(f.render(a).has_value());
    CHECK(f.render(b).has_value());
    const int renders = f.document->renders.load();
    auto& source = f.session->renderSource();
    const std::uint64_t epoch = f.session->revision();

    // Reorder / duplicate / delete another page: both tiles stay valid.
    CHECK(f.run<MovePagesCommand>(std::vector{f.id(1)}, std::size_t{4}));
    CHECK(f.run<DuplicatePagesCommand>(std::vector{f.id(0)}));
    CHECK(f.run<DeletePagesCommand>(std::vector{f.id(3)}));
    CHECK_EQ(f.session->revision(), epoch);
    CHECK(source.cachedTile(a.key, epoch) != nullptr);
    CHECK(source.cachedTile(b.key, epoch) != nullptr);
    CHECK(f.render(a).has_value());
    CHECK_EQ(f.document->renders.load(), renders); // cache hits

    // Rotate page 1: its old key is stale (rejected inline, NotFound), the
    // new key renders through the rotated view; page 2 is untouched.
    CHECK(f.run<RotatePagesCommand>(std::vector{f.id(0)}, 90));
    const RenderResult stale = f.render(a);
    CHECK(!stale.has_value() && stale.error().code == ErrorCode::NotFound);
    const RenderRequest rotated = f.request(f.id(0));
    CHECK(rotated.key.contentRevision != a.key.contentRevision);
    const RenderResult fresh = f.render(rotated);
    CHECK(fresh.has_value());
    CHECK_EQ((*fresh)->width(), 198u); // 792 * 0.25
    CHECK(f.document->lastRenderView().rotation == PageRotation::Clockwise90);
    CHECK(source.cachedTile(b.key, epoch) != nullptr);

    // Undo restores the old revision -> the old tile is valid again.
    CHECK(f.session->undo());
    CHECK_EQ(f.request(f.id(0)).key.contentRevision, a.key.contentRevision);
    const int beforeUndoHit = f.document->renders.load();
    CHECK(f.render(a).has_value());
    CHECK_EQ(f.document->renders.load(), beforeUndoHit);

    // Deleted page: NotFound, never reaches the backend.
    CHECK(f.run<DeletePagesCommand>(std::vector{f.id(0)}));
    const RenderResult gone = f.render(a);
    CHECK(!gone.has_value() && gone.error().code == ErrorCode::NotFound);
}

RIVET_TEST(textCacheInvalidatesOnlyAffectedPages) {
    Fixture f;
    auto& text = f.session->textService();
    for (std::size_t i = 0; i < 3; ++i) CHECK(f.text(f.id(i)) != nullptr);
    CHECK_EQ(f.document->extractions.load(), 3);
    CHECK_EQ(f.text(f.id(1))->text(), std::string("PAGE-2@0"));

    CHECK(f.run<MovePagesCommand>(std::vector{f.id(2)}, std::size_t{0}));
    CHECK(f.run<DuplicatePagesCommand>(std::vector{f.id(4)}));
    for (std::size_t i = 0; i < 3; ++i) CHECK(text.cachedTextPage(f.id(i)) != nullptr);

    // Rotation: only that page misses and is re-extracted through the view.
    CHECK(f.run<RotatePagesCommand>(std::vector{f.id(1)}, 180));
    CHECK(text.cachedTextPage(f.id(1)) == nullptr);
    CHECK(text.cachedTextPage(f.id(0)) != nullptr);
    CHECK_EQ(f.text(f.id(1))->text(), std::string("PAGE-2@180"));
    CHECK_EQ(f.document->extractions.load(), 4);

    // Delete: its text is evicted; requests for it deliver null.
    const std::size_t cached = text.cache().count();
    CHECK(f.run<DeletePagesCommand>(std::vector{f.id(0)}));
    CHECK_EQ(text.cache().count(), cached - 1);
    CHECK(text.cachedTextPage(f.id(0)) == nullptr);
    CHECK(f.text(f.id(0)) == nullptr);

    // Copy over the snapshot: a deleted page in the ranges fails the copy.
    std::promise<rivet::core::Result<std::string>> copied;
    auto copiedFuture = copied.get_future();
    text.requestRangesText({rivet::editor::TextRange{f.id(2), 0, rivet::editor::TextRange::kToPageEnd},
                            rivet::editor::TextRange{f.id(1), 0, rivet::editor::TextRange::kToPageEnd}},
                           [&copied](rivet::core::Result<std::string> result) { copied.set_value(std::move(result)); });
    CHECK(copiedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto copy = copiedFuture.get();
    CHECK(copy.has_value());
    if (copy.has_value()) CHECK_EQ(*copy, std::string("PAGE-3@0\nPAGE-2@180"));
}

RIVET_TEST(searchWalksSnapshotOrderAndRestartsAfterEdits) {
    Fixture f;
    TextSearchController search(*f.session, f.session->textService());
    search.start("PAGE-");
    CHECK(waitFor([&] { return !search.searching(); }));
    CHECK_EQ(search.matchCount(), std::size_t{5});

    CHECK(f.run<MovePagesCommand>(std::vector{f.id(4)}, std::size_t{0}));
    std::vector<PageModelChange> changes;
    f.session->setOnPageModelChanged([&](const PageModelChange& change) { changes.push_back(change); });
    CHECK(f.run<DeletePagesCommand>(std::vector{f.id(1)}));
    CHECK_EQ(changes.size(), std::size_t{1});
    search.handlePageModelChanged(changes.back());
    CHECK(waitFor([&] { return !search.searching(); }));
    const auto matches = search.matches();
    CHECK_EQ(matches.size(), std::size_t{4});
    // Walked in the CURRENT order: 5, 1, 3, 4.
    CHECK(matches.size() == 4 && matches[0].page == f.id(4) && matches[1].page == f.id(0) &&
          matches[2].page == f.id(2) && matches[3].page == f.id(3));

    // Empty query: no restart.
    search.reset();
    search.handlePageModelChanged(changes.back());
    CHECK(!search.searching());
    CHECK_EQ(search.matchCount(), std::size_t{0});
}

RIVET_TEST(linkDestinationsResolveThroughSession) {
    Fixture f;
    rivet::pdf::PdfPageLink link;
    link.kind = rivet::pdf::PdfPageLink::Kind::Internal;
    link.rects.push_back(Rect{0, 0, 10, 10});
    link.destination.pageIndex = 3;
    link.destination.hasUserPoint = true;
    link.destination.userX = 10.0;
    link.destination.userY = 20.0;
    f.document->setLinks(0, {link});

    std::promise<std::vector<rivet::pdf::PdfPageLink>> loaded;
    auto future = loaded.get_future();
    f.session->linkService().requestPageLinks(f.id(0), [&loaded](auto links) { loaded.set_value(std::move(links)); });
    CHECK(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    const auto links = future.get();
    CHECK_EQ(links.size(), std::size_t{1});
    CHECK_EQ(f.session->linkService().cachedLinks(f.id(0)).size(), std::size_t{1});

    CHECK(f.run<MovePagesCommand>(std::vector{f.id(3)}, std::size_t{0}));
    auto target = f.session->resolveLinkDestination(f.id(0), links[0].destination);
    CHECK(target.has_value() && target->page == f.id(3) && target->index == 0);
    CHECK(target.has_value() && target->hasPoint);
    if (target) CHECK_NEAR(target->point.y, 792.0 - 20.0, 1e-9);
    // Outline destinations resolve against the base document the same way.
    CHECK(f.session->resolveOutlineDestination(links[0].destination).has_value());
    // Links survive the reorder (same PageId, same contentRevision).
    const int loads = f.document->linkLoads.load();
    CHECK_EQ(f.session->linkService().cachedLinks(f.id(0)).size(), std::size_t{1});
    CHECK_EQ(f.document->linkLoads.load(), loads);

    CHECK(f.run<DeletePagesCommand>(std::vector{f.id(3)}));
    CHECK(!f.session->resolveLinkDestination(f.id(0), links[0].destination).has_value());
    // Rotating the link's page invalidates its cached links only.
    CHECK(f.run<RotatePagesCommand>(std::vector{f.id(0)}, 90));
    CHECK(f.session->linkService().cachedLinks(f.id(0)).empty());
}

RIVET_TEST(workerJobsUseSnapshotsWhileMainThreadEdits) {
    // TSan target: search walks, text extraction, link loading and renders
    // run on workers while this (main) thread keeps mutating the model.
    // Workers only see captured snapshots/entries; the test asserts that
    // everything completes and the model ends consistent.
    Fixture f(40);
    TextSearchController search(*f.session, f.session->textService());
    std::atomic<int> textDeliveries{0};
    std::atomic<int> renderDeliveries{0};
    int textRequests = 0;
    int renderRequests = 0;
    for (int round = 0; round < 60; ++round) {
        const PageId page = f.id(static_cast<std::size_t>(round) % 40);
        if (round % 5 == 0) search.start("PAGE");
        f.session->textService().requestTextPage(page, [&textDeliveries](auto) { ++textDeliveries; });
        ++textRequests;
        f.session->linkService().ensurePageLinks(page);
        f.session->renderSource().requestRender(f.request(page), RenderPriority::Prefetch,
                                                [&renderDeliveries](RenderResult) { ++renderDeliveries; });
        ++renderRequests;
        switch (round % 4) {
        case 0: CHECK(f.run<MovePagesCommand>(std::vector{page}, std::size_t{0})); break;
        case 1: CHECK(f.run<RotatePagesCommand>(std::vector{page}, 90)); break;
        case 2: CHECK(f.run<DuplicatePagesCommand>(std::vector{page})); break;
        default: CHECK(f.session->undo()); break;
        }
        if (round % 7 == 0) search.handlePageModelChanged(PageModelChange{});
    }
    CHECK(waitFor([&] { return textDeliveries.load() == textRequests; }));
    CHECK(waitFor([&] { return renderDeliveries.load() == renderRequests; }));
    search.start("PAGE"); // over the final snapshot
    CHECK(waitFor([&] { return !search.searching(); }));
    CHECK_EQ(search.matchCount(), f.session->pageCount());
    while (f.session->undo()) {
    }
    CHECK((f.session->pageOrder() == f.ids));
    CHECK(!f.session->isDirty());
}
