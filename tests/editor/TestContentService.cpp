// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/ContentTestSupport.hpp"

#include <chrono>
#include <set>
#include <thread>

// ContentService over the fake backend: lazy extraction, bounded LRU with
// stable ids, hit testing in display space, registry re-keying, dropped
// stale results and destruction safety (ADR-0014).

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;
using core::ObjectId;
using core::PageId;
using core::Point;

RIVET_TEST(ContentService_loads_lazily_and_resolves_display_space) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK_EQ(f.document->contentLoads.load(), 0);
    const PageContentViewPtr first = f.content(0);
    CHECK(!first->loaded);
    CHECK(f.load(0));
    CHECK_EQ(f.document->contentLoads.load(), 1);
    CHECK_EQ(f.changes.size(), std::size_t{1});
    CHECK(f.changes[0] == f.id(0));

    const PageContentViewPtr view = f.content(0);
    CHECK(view->loaded);
    CHECK(!view->truncated);
    CHECK(view->regenerationSafe);
    CHECK_EQ(view->sourceObjectCount, std::size_t{5});
    CHECK_EQ(view->objects.size(), std::size_t{5});
    CHECK_EQ(view->blocks.size(), std::size_t{1});
    std::set<ObjectId> unique;
    for (std::size_t i = 0; i < view->objects.size(); ++i) {
        CHECK(view->objects[i].id.value() != 0);
        CHECK_EQ(view->objects[i].index, static_cast<std::uint32_t>(i));
        unique.insert(view->objects[i].id);
    }
    CHECK_EQ(unique.size(), std::size_t{5});

    // Display space: top-left origin, y down.
    const ContentObjectView& image = view->objects[0];
    CHECK_NEAR(image.bounds.minX(), 100.0, 1e-9);
    CHECK_NEAR(image.bounds.minY(), 792.0 - 600.0, 1e-9);
    CHECK_NEAR(image.bounds.size.width, 100.0, 1e-9);
    CHECK_NEAR(image.bounds.size.height, 100.0, 1e-9);
    CHECK(image.capability == ContentCapability::FullyEditable);
    CHECK_EQ(image.pixelWidth, std::uint32_t{16});
    CHECK(view->objects[4].capability == ContentCapability::ReadOnly);
    CHECK(!view->objects[4].capabilityReason.empty());

    // Text objects know their block, whose id is its first member's.
    const TextBlockView& block = view->blocks[0];
    CHECK(block.id == view->objects[2].id);
    CHECK(view->objects[2].block == block.id);
    CHECK(view->objects[3].block == block.id);
    CHECK_EQ(block.text, std::string("Hello world"));
    CHECK(!block.edited);

    // A second request neither reloads nor notifies.
    f.content(0);
    f.dispatcher.pump();
    CHECK_EQ(f.document->contentLoads.load(), 1);
    CHECK_EQ(f.changes.size(), std::size_t{1});
    CHECK(f.session->contentService().findObject(f.id(0), view->objects[1].id).has_value());
    CHECK(f.session->contentService().findBlock(f.id(0), block.id).has_value());
    CHECK(!f.session->contentService().findObject(f.id(0), ObjectId{99999}).has_value());
}

RIVET_TEST(ContentService_extracts_only_requested_pages) {
    ContentFixture f(4);
    f.setContent(2, samplePage());
    CHECK(f.load(2));
    CHECK_EQ(f.document->contentLoads.load(), 1);
    CHECK_EQ(f.session->contentService().cachedPages(), std::size_t{1});
    // Another page has an empty, loaded content.
    CHECK(f.load(1));
    CHECK(f.content(1)->objects.empty());
}

RIVET_TEST(ContentService_unknown_page_is_empty) {
    ContentFixture f;
    const PageContentViewPtr view = f.session->contentService().content(PageId{424242});
    CHECK(view != nullptr);
    CHECK(view->objects.empty());
    CHECK(!view->loaded);
    CHECK(!f.session->contentService().hitTest(PageId{424242}, Point{1, 1}, 2.0).has_value());
}

RIVET_TEST(ContentService_lru_eviction_keeps_ids) {
    constexpr std::size_t kPages = ContentService::kMaxCachedPages + 6;
    ContentFixture f(kPages);
    for (std::size_t i = 0; i < kPages; ++i) {
        pdf::PdfPageContent content;
        content.objects.push_back(makeImageObject(10, 10, 50, 50));
        content.objects.push_back(makeImageObject(60, 60, 90, 90));
        f.setContent(i, std::move(content));
    }
    CHECK(f.load(0));
    const ObjectId first0 = f.content(0)->objects[0].id;
    const ObjectId first1 = f.content(0)->objects[1].id;
    for (std::size_t i = 1; i < kPages; ++i) CHECK(f.load(i));
    CHECK_EQ(f.document->contentLoads.load(), static_cast<int>(kPages));
    CHECK(f.session->contentService().cachedPages() <= ContentService::kMaxCachedPages);

    // Page 0 was evicted: it is extracted again, with the same ids.
    f.content(0);
    CHECK(f.load(0));
    CHECK_EQ(f.document->contentLoads.load(), static_cast<int>(kPages) + 1);
    CHECK(f.content(0)->objects[0].id == first0);
    CHECK(f.content(0)->objects[1].id == first1);
    // Ids are unique across pages.
    CHECK(f.content(1)->objects[0].id != first0);
}

RIVET_TEST(ContentService_hit_test_picks_the_topmost_object) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const PageContentViewPtr view = f.content(0);
    auto& service = f.session->contentService();

    // The overlap of the two images: the later (higher z) one wins.
    auto hit = service.hitTest(f.id(0), disp(175, 575), 0.0);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->id == view->objects[1].id);
        CHECK(!hit->isBlock);
    }
    // Only the lower image.
    hit = service.hitTest(f.id(0), disp(110, 510), 0.0);
    CHECK(hit.has_value());
    if (hit) CHECK(hit->id == view->objects[0].id);
    // Nothing there.
    CHECK(!service.hitTest(f.id(0), disp(300, 300), 0.0).has_value());
}

RIVET_TEST(ContentService_hit_test_tolerance) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    auto& service = f.session->contentService();
    // 10 points left of the lower image (x = 100).
    CHECK(!service.hitTest(f.id(0), disp(90, 550), 5.0).has_value());
    CHECK(service.hitTest(f.id(0), disp(90, 550), 12.0).has_value());
    CHECK(!service.hitTest(f.id(0), disp(90, 550), 0.0).has_value());
}

RIVET_TEST(ContentService_hit_test_text_hits_its_block_and_editable_only_skips_read_only) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const PageContentViewPtr view = f.content(0);
    auto& service = f.session->contentService();

    auto hit = service.hitTest(f.id(0), disp(80, 703), 0.0);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->isBlock);
        CHECK(hit->id == view->blocks[0].id);
    }
    // The read-only object is hit unless only editable objects are asked for.
    hit = service.hitTest(f.id(0), disp(420, 120), 0.0);
    CHECK(hit.has_value());
    if (hit) CHECK(hit->id == view->objects[4].id);
    CHECK(!service.hitTest(f.id(0), disp(420, 120), 0.0, true).has_value());
}

RIVET_TEST(ContentService_page_wide_restrictions_make_everything_read_only) {
    ContentFixture f;
    pdf::PdfPageContent content = samplePage();
    content.regenerationSafe = false;
    content.regenerationIssue = "content would change";
    f.setContent(0, std::move(content));
    CHECK(f.load(0));
    const PageContentViewPtr view = f.content(0);
    CHECK(!view->regenerationSafe);
    CHECK_EQ(view->regenerationIssue, std::string("content would change"));
    for (const ContentObjectView& object : view->objects) CHECK(object.capability == ContentCapability::ReadOnly);
    CHECK(!f.session->contentService().hitTest(f.id(0), disp(110, 510), 0.0, true).has_value());
}

RIVET_TEST(ContentService_evict_pages_drops_the_resolved_view_but_keeps_ids) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const ObjectId id = f.content(0)->objects[1].id;
    const PageId page = f.id(0);
    f.session->contentService().evictPages(std::vector<PageId>{page});
    CHECK(f.load(0));
    CHECK(f.content(0)->objects[1].id == id);
}

RIVET_TEST(ContentService_rebased_rekeys_source_ids_to_the_new_indices) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const PageContentViewPtr before = f.content(0);
    const ObjectId a = before->objects[0].id;
    const ObjectId c = before->objects[2].id;

    // The saved page: old object 2 became 0, old object 0 became 1, the rest
    // were removed.
    ContentService::PageRekey rekey;
    rekey.page = f.id(0);
    rekey.origins = {pdf::PdfContentOrigin{pdf::PdfContentOrigin::Kind::Source, 2, 0},
                     pdf::PdfContentOrigin{pdf::PdfContentOrigin::Kind::Source, 0, 0}};
    rekey.blockTags = {0, 0};
    f.session->contentService().rebased(std::vector<ContentService::PageRekey>{rekey});
    CHECK_EQ(f.session->contentService().cachedPages(), std::size_t{0});

    // The (fake) document still reports five objects: the first two follow
    // the new indices, the others are fresh ids that were never issued.
    CHECK(f.load(0));
    const PageContentViewPtr after = f.content(0);
    CHECK_EQ(after->objects.size(), std::size_t{5});
    CHECK(after->objects[0].id == c);
    CHECK(after->objects[1].id == a);
    std::set<ObjectId> old;
    for (const ContentObjectView& object : before->objects) old.insert(object.id);
    for (std::size_t i = 2; i < after->objects.size(); ++i) {
        CHECK(old.count(after->objects[i].id) == 0);
    }
}

RIVET_TEST(ContentService_rebased_without_keep_mints_fresh_ids) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    std::set<ObjectId> old;
    for (const ContentObjectView& object : f.content(0)->objects) old.insert(object.id);

    ContentService::PageRekey rekey;
    rekey.page = f.id(0);
    rekey.keep = false;
    f.session->contentService().rebased(std::vector<ContentService::PageRekey>{rekey});
    CHECK(f.load(0));
    for (const ContentObjectView& object : f.content(0)->objects) CHECK(old.count(object.id) == 0);
}

RIVET_TEST(ContentService_rebased_leaves_other_pages_alone) {
    ContentFixture f;
    f.setContent(0, samplePage());
    f.setContent(1, samplePage());
    CHECK(f.load(0));
    CHECK(f.load(1));
    const ObjectId other = f.content(1)->objects[0].id;
    ContentService::PageRekey rekey;
    rekey.page = f.id(0);
    rekey.keep = false;
    f.session->contentService().rebased(std::vector<ContentService::PageRekey>{rekey});
    CHECK(f.load(0));
    CHECK(f.load(1));
    CHECK(f.content(1)->objects[0].id == other);
}

RIVET_TEST(ContentService_results_of_a_previous_base_are_dropped) {
    ContentFixture f;
    f.setContent(0, samplePage());
    std::unique_lock<std::mutex> gate(f.document->contentGate);
    f.content(0);
    // The worker is parked inside the backend.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.document->contentLoads.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_EQ(f.document->contentLoads.load(), 1);
    f.session->contentService().rebased({});
    gate.unlock();

    // The finished extraction belongs to the previous base: nothing is
    // cached and nobody is notified.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < until) {
        f.dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(f.changes.empty());
    CHECK_EQ(f.session->contentService().cachedPages(), std::size_t{0});
    // A fresh request loads again.
    CHECK(f.load(0));
    CHECK_EQ(f.document->contentLoads.load(), 2);
}

RIVET_TEST(ContentService_destruction_with_a_pending_extraction_is_safe) {
    ContentFixture f;
    f.setContent(0, samplePage());
    std::unique_lock<std::mutex> gate(f.document->contentGate);
    f.content(0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (f.document->contentLoads.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_EQ(f.document->contentLoads.load(), 1);
    std::thread releaser([&gate] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        gate.unlock();
    });
    f.session.reset(); // waits for the parked extraction, never notifies
    releaser.join();
    f.dispatcher.pump();
    CHECK(f.changes.empty());
}

RIVET_TEST(ContentService_registry_counts_issued_ids) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK_EQ(f.session->contentService().registrySize(), std::size_t{0});
    CHECK(f.load(0));
    CHECK_EQ(f.session->contentService().registrySize(), std::size_t{5});
}
