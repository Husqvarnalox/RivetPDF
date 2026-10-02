// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "editor/AnnotationGeometry.hpp"
#include "editor/PageCommands.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

// AnnotationService: lazy originals (async, coalesced, bounded LRU, empty on
// error), resolution with the page model's state, identity registry, hit
// testing, and dropping of stale completions.

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;
using core::PageRotation;
using core::Point;

namespace {

std::vector<pdf::PdfPageAnnotation> sampleOriginals() {
    // /Annots: 0 highlight (popup at 1), 1 popup, 2 square, 3 opaque (e.g. FreeText), 4 note.
    return {makeOriginal(0, highlightAt(100, 500, 200, 515), 1), makePopupEntry(1),
            makeOriginal(2, squareAt(300, 300, 380, 360)), makeOpaque(3), makeOriginal(4, noteAt(50, 600))};
}

} // namespace

RIVET_TEST(annotationServiceLoadsOriginalsLazilyAndResolves) {
    AnnotationFixture f;
    f.document->setAnnotations(0, sampleOriginals());

    // First look: overlay-only (empty) while the load runs on the worker.
    CHECK(f.list(0)->empty());
    CHECK(f.load(0));
    CHECK_EQ(f.changes.size(), std::size_t{1});
    CHECK(f.changes[0] == f.id(0));
    CHECK_EQ(f.document->annotationLoads.load(), 1);

    const auto resolved = f.list(0);
    // Editable, non-popup originals only, in /Annots order.
    CHECK_EQ(resolved->size(), std::size_t{3});
    CHECK((*resolved)[0].kind == pdf::PdfAnnotationKind::Highlight);
    CHECK((*resolved)[1].kind == pdf::PdfAnnotationKind::Square);
    CHECK((*resolved)[2].kind == pdf::PdfAnnotationKind::Note);
    for (const AnnotationView& v : *resolved) {
        CHECK(!v.drawnByOverlay);
        CHECK(!v.appearance.has_value());
        CHECK(v.id);
        CHECK_EQ(v.caps, annotationCaps(v.kind));
    }
    CHECK(((*resolved)[0].id != (*resolved)[1].id) && ((*resolved)[1].id != (*resolved)[2].id));
    CHECK_EQ((*resolved)[1].contents, std::string("sq"));
    CHECK_EQ(f.list(0)->size(), std::size_t{3});
    // Cached resolution is the same immutable object; no second load.
    CHECK(f.list(0) == resolved);
    CHECK_EQ(f.document->annotationLoads.load(), 1);
    // Another page: independent.
    CHECK(f.list(1)->empty());
}

RIVET_TEST(annotationServiceCoalescesConcurrentRequests) {
    AnnotationFixture f;
    f.document->setAnnotations(0, sampleOriginals());
    f.document->closeAnnotationGate();
    for (int i = 0; i < 5; ++i) f.session->annotations().annotations(f.id(0));
    CHECK(f.document->waitAnnotationParked(1));
    for (int i = 0; i < 5; ++i) f.session->annotations().annotations(f.id(0));
    f.document->releaseAnnotationGate();
    CHECK(settle(f.dispatcher, [&] { return !f.changes.empty(); }));
    f.dispatcher.pump();
    CHECK_EQ(f.changes.size(), std::size_t{1}); // exactly once
    CHECK_EQ(f.document->annotationLoads.load(), 1);
    CHECK_EQ(f.list(0)->size(), std::size_t{3});
}

RIVET_TEST(annotationServiceUnavailableBackendCachesEmpty) {
    AnnotationFixture f;
    f.document->setAnnotationsUnavailable(true);
    CHECK(f.load(0)); // completes (as an empty list), onChanged fires
    CHECK(f.list(0)->empty());
    CHECK(f.list(0)->empty());
    CHECK_EQ(f.document->annotationLoads.load(), 1); // not retried
    CHECK_EQ(f.session->annotations().cachedOriginalPages(), std::size_t{1});
}

RIVET_TEST(annotationServiceDropsCompletionWhenServiceDies) {
    AnnotationFixture f;
    f.document->setAnnotations(0, sampleOriginals());
    f.document->closeAnnotationGate();
    f.session->annotations().annotations(f.id(0));
    CHECK(f.document->waitAnnotationParked(1));
    std::atomic<int> fired{0};
    f.session->setOnAnnotationsChanged([&](core::PageId) { ++fired; });
    std::thread release([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        f.document->releaseAnnotationGate();
    });
    f.session.reset(); // destructor waits for the parked job, cancels the rest
    release.join();
    f.dispatcher.pump(); // the posted delivery must be inert now
    CHECK_EQ(fired.load(), 0);
}

RIVET_TEST(annotationServiceRebaseDropsLoadsInFlight) {
    AnnotationFixture f;
    f.document->setAnnotations(0, sampleOriginals());
    f.document->closeAnnotationGate();
    f.session->annotations().annotations(f.id(0));
    CHECK(f.document->waitAnnotationParked(1));
    f.session->annotations().rebased({}); // a new base: results of the old load are stale
    f.document->releaseAnnotationGate();
    CHECK(settle(f.dispatcher, [&] { return f.document->annotationLoads.load() == 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f.dispatcher.pump();
    CHECK(f.changes.empty());
    CHECK_EQ(f.session->annotations().cachedOriginalPages(), std::size_t{0});
    // A fresh request loads again.
    CHECK(f.load(0));
    CHECK_EQ(f.list(0)->size(), std::size_t{3});
}

RIVET_TEST(annotationServiceLruEvictsAndReloadKeepsIds) {
    constexpr std::size_t kPages = AnnotationService::kMaxCachedPages + 4;
    AnnotationFixture f(kPages);
    f.document->setAnnotations(0, sampleOriginals());
    CHECK(f.load(0));
    const core::AnnotationId first = (*f.list(0))[0].id;
    CHECK_EQ(f.document->annotationLoads.load(), 1);
    for (std::size_t i = 1; i < kPages; ++i) CHECK(f.load(i));
    CHECK_LE(f.session->annotations().cachedOriginalPages(), AnnotationService::kMaxCachedPages);
    CHECK_EQ(f.document->annotationLoads.load(), static_cast<int>(kPages));
    // Page 0 was evicted: the next look reloads, with the very same ids.
    f.loaded.clear();
    CHECK(f.list(0)->empty() || f.list(0)->size() == 3);
    CHECK(f.load(0));
    CHECK_EQ(f.document->annotationLoads.load(), static_cast<int>(kPages) + 1);
    CHECK_EQ(f.list(0)->size(), std::size_t{3});
    CHECK((*f.list(0))[0].id == first);
}

RIVET_TEST(annotationServiceIdsSurviveReorderDeleteAndUndo) {
    AnnotationFixture f;
    f.document->setAnnotations(0, sampleOriginals());
    CHECK(f.load(0));
    const auto before = f.list(0);
    std::vector<core::AnnotationId> ids;
    for (const AnnotationView& v : *before) ids.push_back(v.id);

    // Reorder: the page keeps its identity and its annotations.
    CHECK(f.session->execute(std::make_unique<MovePagesCommand>(f.session->pageModel(),
                                                                std::vector<core::PageId>{f.id(0)}, 2))
              .has_value());
    const auto moved = f.list(0);
    CHECK_EQ(moved->size(), std::size_t{3});
    for (std::size_t i = 0; i < 3; ++i) CHECK((*moved)[i].id == ids[i]);

    // Delete the page and undo: same ids come back (registry never evicts).
    CHECK(f.session->execute(std::make_unique<DeletePagesCommand>(f.session->pageModel(),
                                                                  std::vector<core::PageId>{f.id(0)}))
              .has_value());
    CHECK(f.list(0)->empty()); // the page is gone
    CHECK(f.session->undo());
    const auto restored = f.list(0);
    CHECK_EQ(restored->size(), std::size_t{3});
    for (std::size_t i = 0; i < 3; ++i) CHECK((*restored)[i].id == ids[i]);
}

RIVET_TEST(annotationServiceHitTestTopmostFirstAndKinds) {
    AnnotationFixture f;
    f.document->setAnnotations(0, {makeOriginal(0, squareAt(100, 100, 300, 300, 4.0F)),
                                   makeOriginal(1, squareAt(150, 150, 350, 350, 4.0F)),
                                   makeOriginal(2, highlightAt(100, 500, 200, 515))});
    CHECK(f.load(0));
    const auto view = f.entry(0).view;
    const auto list = f.list(0);
    // A point on both outlines' crossing region: the topmost (later) one wins.
    const Point onFirstOnly = geometry::toDisplay(view, pdf::PdfPoint{100, 200});
    CHECK(f.session->annotations().hitTest(f.id(0), onFirstOnly, 1.0) == (*list)[0].id);
    const Point onSecondOnly = geometry::toDisplay(view, pdf::PdfPoint{350, 250});
    CHECK(f.session->annotations().hitTest(f.id(0), onSecondOnly, 1.0) == (*list)[1].id);
    // Both outlines meet within tolerance at the crossing: the later wins.
    const Point crossing = geometry::toDisplay(view, pdf::PdfPoint{300, 175});
    CHECK(f.session->annotations().hitTest(f.id(0), crossing, 1.0) == (*list)[0].id);
    const Point crossing2 = geometry::toDisplay(view, pdf::PdfPoint{150, 300});
    CHECK(f.session->annotations().hitTest(f.id(0), crossing2, 1.0) == (*list)[1].id);
    CHECK(f.session->annotations().hitTest(f.id(0), geometry::toDisplay(view, pdf::PdfPoint{150, 507}), 0.0) ==
          (*list)[2].id);
    CHECK(!f.session->annotations().hitTest(f.id(0), geometry::toDisplay(view, pdf::PdfPoint{500, 700}), 3.0));
    CHECK(!f.session->annotations().hitTest(f.id(1), Point{10, 10}, 3.0)); // another page
}

RIVET_TEST(annotationServiceResolvedFollowsRotatedView) {
    AnnotationFixture f;
    f.document->setAnnotations(0, {makeOriginal(0, squareAt(100, 100, 300, 300, 4.0F))});
    CHECK(f.load(0));
    const Point before = (*f.list(0))[0].bounds.center();
    CHECK(f.session->execute(std::make_unique<RotatePagesCommand>(f.session->pageModel(),
                                                                  std::vector<core::PageId>{f.id(0)}, 90))
              .has_value());
    const auto after = f.list(0);
    CHECK_EQ(after->size(), std::size_t{1});
    const Point rotated = (*after)[0].bounds.center();
    CHECK(std::abs(rotated.x - before.x) > 1.0 || std::abs(rotated.y - before.y) > 1.0);
    const Point expect = geometry::toDisplay(f.entry(0).view, pdf::PdfPoint{200, 200});
    CHECK_NEAR(rotated.x, expect.x, 1e-6);
    CHECK_NEAR(rotated.y, expect.y, 1e-6);
}

RIVET_TEST(annotationServiceLocateKnowsOriginalsOverlayAndDeleted) {
    AnnotationFixture f;
    f.document->setAnnotations(0, sampleOriginals());
    // Unknown before the originals are loaded.
    CHECK(!f.session->annotations().locate(core::AnnotationId{999}));
    CHECK(f.load(0));
    const auto list = f.list(0);
    const auto square = f.session->annotations().locate((*list)[1].id);
    CHECK(square.has_value());
    CHECK(!square->overlay);
    CHECK(square->index == std::optional<std::uint32_t>{2});
    const auto highlight = f.session->annotations().locate((*list)[0].id);
    CHECK(highlight.has_value());
    CHECK(highlight->popupIndex == std::optional<std::uint32_t>{1});
    CHECK(f.session->annotations().find(f.id(0), (*list)[2].id).has_value());
    CHECK(!f.session->annotations().find(f.id(1), (*list)[2].id).has_value());
}
