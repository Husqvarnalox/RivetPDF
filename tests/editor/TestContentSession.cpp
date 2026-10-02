// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/ContentTestSupport.hpp"

#include "editor/DocumentSaver.hpp"
#include "editor/PageCommands.hpp"
#include "render/PhysicalRenderScaleKey.hpp"
#include "render/RenderPriority.hpp"
#include "render/RenderRequest.hpp"

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <set>

// Content edits through the session: the render / text / link pipelines see
// the edits and their revisions, the save pipeline passes them to the
// backend, and the rebase after a save re-keys object identities and clears
// the edits (ADR-0017 "Rebase").

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;
using core::ObjectId;
using core::PageId;
using core::Point;
using core::Rect;

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path() /
                ("rivet-content-" + std::to_string(::getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(counter++));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    fs::path operator/(const char* name) const { return path_ / name; }

private:
    fs::path path_;
};

bool renderPage(ContentFixture& f, PageId page, std::uint64_t revision) {
    const PageEntry* entry = f.session->pageSnapshot()->find(page);
    CHECK(entry != nullptr);
    if (entry == nullptr) return false;
    render::RenderRequest r;
    r.key.documentId = f.session->id();
    r.key.pageId = page;
    r.key.scale = render::PhysicalRenderScaleKey::fromDensities(0.25, 1.0);
    r.key.contentRevision = revision;
    const core::Size size = pdf::displaySize(entry->view);
    r.params.pageRectPoints = Rect{0.0, 0.0, size.width, size.height};
    r.params.devicePixelsPerPoint = r.key.scale.scale();
    auto done = std::make_shared<std::atomic<int>>(0); // 0 pending, 1 ok, 2 failed
    f.session->renderSource().requestRender(r, render::RenderPriority::Visible, [done](render::RenderResult result) {
        done->store(result.has_value() ? 1 : 2);
    });
    CHECK(settle(f.dispatcher, [&] { return done->load() != 0; }));
    return done->load() == 1;
}

struct SaveFixture {
    SaveFixture() : f(dir / "doc.pdf") {}
    TempDir dir;
    BasicContentFixture<FakeWritableEngine> f;

    // Saves the session's model and rebases onto the result.
    core::Status saveAndRebase() {
        const auto path = dir / "out.pdf";
        auto job = makeSaveJob(*f.session, path);
        CHECK(job.has_value());
        if (!job) return core::ok();
        auto result = runDocumentWrite(f.engine, *job);
        CHECK(result.written.has_value());
        CHECK(result.rebase.has_value());
        if (!result.rebase || !result.rebase->has_value()) return core::ok();
        job->snapshot.reset();
        auto status = f.session->rebaseOnto(std::move(**result.rebase));
        if (status) f.session->markSaved();
        return status;
    }

    // The report of a page-0 write: `origins` for page 0, empty for the rest.
    void reportPage0(std::vector<pdf::PdfContentOrigin> origins, std::vector<std::uint64_t> tags) {
        std::vector<pdf::PdfAssembledPageContent> report(f.session->pageCount());
        report[0].origins = std::move(origins);
        report[0].blockTags = std::move(tags);
        f.engine.contentReportOverride = std::move(report);
    }
};

pdf::PdfContentOrigin source(std::uint32_t index) {
    return pdf::PdfContentOrigin{pdf::PdfContentOrigin::Kind::Source, index, 0};
}

} // namespace

// --- Render / text / links ----------------------------------------------------------

RIVET_TEST(ContentSession_render_target_carries_the_edits_and_keys_the_raster_revision) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    CHECK(renderPage(f, f.id(0), f.entry(0).rasterRevision));
    CHECK(f.document->lastRenderContentEdits() == nullptr);

    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{4, 4})));
    const std::uint64_t raster = f.entry(0).rasterRevision;
    CHECK(f.entry(0).contentEdits != nullptr);
    CHECK(renderPage(f, f.id(0), raster));
    CHECK(f.document->lastRenderContentEdits() == f.entry(0).contentEdits);
    // The previous raster revision is stale.
    CHECK(!renderPage(f, f.id(0), raster - 1));

    // Undo renders the page as stored again, under a fresh revision.
    CHECK(f.session->undo());
    CHECK(f.entry(0).rasterRevision != raster);
    CHECK(renderPage(f, f.id(0), f.entry(0).rasterRevision));
    CHECK(f.document->lastRenderContentEdits() == nullptr);
}

RIVET_TEST(ContentSession_edit_invalidates_cached_text_and_links) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const PageId page = f.id(0);

    f.session->textService().ensureTextPage(page);
    CHECK(settle(f.dispatcher, [&] { return f.session->textService().cachedTextPage(page) != nullptr; }));
    CHECK(f.document->lastTextContentEdits() == nullptr);
    int linkDeliveries = 0;
    f.session->linkService().requestPageLinks(page, [&](std::vector<pdf::PdfPageLink>) { ++linkDeliveries; });
    CHECK(settle(f.dispatcher, [&] { return linkDeliveries == 1; }));
    CHECK_EQ(f.document->linkLoads.load(), 1);
    f.session->linkService().requestPageLinks(page, [&](std::vector<pdf::PdfPageLink>) { ++linkDeliveries; });
    CHECK(settle(f.dispatcher, [&] { return linkDeliveries == 2; }));
    CHECK_EQ(f.document->linkLoads.load(), 1);

    CHECK(f.run(moveContent(*f.session, page, {f.objectId(0, 0)}, Point{4, 4})));
    // Keyed by the new content revision: the old text and links are not served.
    CHECK(f.session->textService().cachedTextPage(page) == nullptr);
    CHECK(f.session->linkService().cachedLinks(page).empty());
    f.session->textService().ensureTextPage(page);
    CHECK(settle(f.dispatcher, [&] { return f.session->textService().cachedTextPage(page) != nullptr; }));
    CHECK(f.document->lastTextContentEdits() == f.entry(0).contentEdits);
    f.session->linkService().requestPageLinks(page, [&](std::vector<pdf::PdfPageLink>) { ++linkDeliveries; });
    CHECK(settle(f.dispatcher, [&] { return linkDeliveries == 3; }));
    CHECK_EQ(f.document->linkLoads.load(), 2);
}

RIVET_TEST(ContentSession_other_pages_keep_their_caches) {
    ContentFixture f;
    f.setContent(0, samplePage());
    f.setContent(1, samplePage());
    CHECK(f.load(0));
    f.session->textService().ensureTextPage(f.id(1));
    CHECK(settle(f.dispatcher, [&] { return f.session->textService().cachedTextPage(f.id(1)) != nullptr; }));
    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{4, 4})));
    CHECK(f.session->textService().cachedTextPage(f.id(1)) != nullptr);
}

// --- Page operations -------------------------------------------------------------------

RIVET_TEST(ContentSession_a_duplicated_page_shares_the_edits_but_has_its_own_ids) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 1)}, Point{8, 8})));
    auto duplicate = std::make_unique<DuplicatePagesCommand>(f.session->pageModel(), std::vector<PageId>{f.id(0)});
    const DuplicatePagesCommand* handle = duplicate.get();
    CHECK(f.session->execute(std::move(duplicate)).has_value());
    CHECK_EQ(handle->createdIds().size(), std::size_t{1});
    const PageId copy = handle->createdIds()[0];
    const PageEntry* copyEntry = f.session->pageSnapshot()->find(copy);
    CHECK(copyEntry != nullptr);
    if (copyEntry == nullptr) return;
    CHECK(copyEntry->contentEdits == f.entry(0).contentEdits);
    CHECK(copyEntry->contentEdits != nullptr);

    CHECK(settle(f.dispatcher, [&] { return f.session->contentService().content(copy)->loaded; }));
    CHECK(f.load(0));
    const auto original = f.content(0);
    const auto duplicated = f.session->contentService().content(copy);
    CHECK_EQ(duplicated->objects.size(), original->objects.size());
    for (std::size_t i = 0; i < original->objects.size(); ++i) {
        CHECK(original->objects[i].id != duplicated->objects[i].id);
    }
    CHECK(duplicated->objects[1].edited);

    // Editing the copy leaves the original page alone.
    const auto before = f.entry(0).contentEdits;
    auto edit = deleteContent(*f.session, copy, {duplicated->objects[0].id});
    CHECK(edit.has_value());
    if (!edit) return;
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    CHECK(f.entry(0).contentEdits == before);
    CHECK(f.session->pageSnapshot()->find(copy)->contentEdits != before);
}

RIVET_TEST(ContentSession_deleting_a_page_evicts_its_content) {
    ContentFixture f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const PageId page = f.id(0);
    CHECK(f.session->execute(std::make_unique<DeletePagesCommand>(f.session->pageModel(), std::vector<PageId>{page}))
              .has_value());
    CHECK(f.session->contentService().content(page)->objects.empty());
    CHECK(f.session->undo());
    CHECK(f.load(0));
    CHECK_EQ(f.content(0)->objects.size(), std::size_t{5});
}

// --- Save / rebase -----------------------------------------------------------------------

RIVET_TEST(ContentSession_save_sends_the_edits_to_the_engine) {
    SaveFixture s;
    auto& f = s.f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 1)}, Point{5, 5})));
    auto job = makeSaveJob(*f.session, s.dir / "out.pdf");
    CHECK(job.has_value());
    if (!job) return;
    auto result = runDocumentWrite(f.engine, *job);
    CHECK(result.written.has_value());
    CHECK_EQ(f.engine.lastContentEdits.size(), f.session->pageCount());
    CHECK_EQ(f.engine.lastContentEdits[0].objects.size(), std::size_t{1});
    CHECK_EQ(f.engine.lastContentEdits[0].objects[0].sourceIndex, std::uint32_t{1});
    for (std::size_t i = 1; i < f.engine.lastContentEdits.size(); ++i) CHECK(f.engine.lastContentEdits[i].empty());
}

RIVET_TEST(ContentSession_extract_and_print_requests_carry_the_edits) {
    SaveFixture s;
    auto& f = s.f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    CHECK(f.run(deleteContent(*f.session, f.id(0), {f.objectId(0, 0)})));

    const std::vector<PageId> pages{f.id(1), f.id(0)};
    auto job = makeExtractJob(*f.session, pages, s.dir / "part.pdf");
    CHECK(job.has_value());
    if (!job) return;
    auto result = runDocumentWrite(f.engine, *job);
    CHECK(result.written.has_value());
    // Pages in model order: page 0 (edited), page 1.
    CHECK_EQ(f.engine.lastContentEdits.size(), std::size_t{2});
    CHECK_EQ(f.engine.lastContentEdits[0].objects.size(), std::size_t{1});
    CHECK(f.engine.lastContentEdits[0].objects[0].remove);
    CHECK(f.engine.lastContentEdits[1].empty());

    // The print assembly asks the snapshot for an Extract request of every page.
    const PageSnapshotPtr snapshot = f.session->pageSnapshot();
    const std::vector<PageId> all = f.session->pageOrder();
    auto request = snapshot->toAssemblyRequest(PageModelSnapshot::AssemblyMode::Extract, all);
    CHECK(request.has_value());
    if (request) {
        CHECK_EQ(request->pages.size(), all.size());
        CHECK(request->pages[0].contentEdits == f.entry(0).contentEdits);
        CHECK(request->pages[1].contentEdits == nullptr);
    }
}

RIVET_TEST(ContentSession_save_and_rebase_rekeys_ids_and_clears_the_edits) {
    SaveFixture s;
    auto& f = s.f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const PageContentViewPtr before = f.content(0);
    std::vector<ObjectId> old;
    for (const ContentObjectView& object : before->objects) old.push_back(object.id);

    // Delete object 0: the saved page holds the old objects 1..4.
    CHECK(f.run(deleteContent(*f.session, f.id(0), {old[0]})));
    pdf::PdfPageContent saved = samplePage();
    saved.objects.erase(saved.objects.begin());
    f.engine.reopenedContents[0] = indexed(std::move(saved));
    s.reportPage0({source(1), source(2), source(3), source(4)}, {0, 0, 0, 0});

    const std::uint64_t content = f.entry(0).contentRevision;
    const std::uint64_t raster = f.entry(0).rasterRevision;
    CHECK(f.session->isDirty());
    CHECK(s.saveAndRebase());
    CHECK(!f.session->isDirty());
    CHECK(f.entry(0).contentEdits == nullptr);
    // The page looks the same: revisions are kept.
    CHECK_EQ(f.entry(0).contentRevision, content);
    CHECK_EQ(f.entry(0).rasterRevision, raster);
    // The history was cleared by the rebase.
    CHECK(!f.session->undo());

    CHECK(f.load(0));
    const PageContentViewPtr after = f.content(0);
    CHECK_EQ(after->objects.size(), std::size_t{4});
    for (std::size_t i = 0; i < 4; ++i) CHECK(after->objects[i].id == old[i + 1]);
    // The block keeps its id (its first member's).
    CHECK_EQ(after->blocks.size(), std::size_t{1});
    CHECK(after->blocks[0].id == old[2]);
    // The page can be edited again; a fresh edit starts from the saved file.
    CHECK(f.run(moveContent(*f.session, f.id(0), {old[1]}, Point{3, 3})));
    CHECK_EQ(f.entry(0).contentEdits->objects.size(), std::size_t{1});
    CHECK_EQ(f.entry(0).contentEdits->objects[0].sourceIndex, std::uint32_t{0});
}

RIVET_TEST(ContentSession_created_blocks_keep_their_id_across_a_save) {
    SaveFixture s;
    auto& f = s.f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    NewTextBlock text;
    text.text = "Added";
    text.displayOrigin = Point{300.0, 100.0};
    auto added = addTextBlock(*f.session, f.id(0), text);
    CHECK(added.has_value());
    if (!added) return;
    const ObjectId block = added->ids[0];
    const std::uint64_t tag = block.value();
    CHECK(f.session->execute(std::move(added->command)).has_value());
    CHECK(f.load(0));
    CHECK_EQ(f.content(0)->objects.size(), std::size_t{6});
    const ObjectId createdObject = f.content(0)->objects[5].id;
    CHECK(createdObject == block);

    // The saved page: the five originals, then the new text as plain content.
    pdf::PdfPageContent saved = samplePage();
    saved.objects.push_back(makeTextObject(300.0, 692.0, 12.0, "Added", 30.0));
    f.engine.reopenedContents[0] = indexed(std::move(saved));
    s.reportPage0({source(0), source(1), source(2), source(3), source(4),
                   pdf::PdfContentOrigin{pdf::PdfContentOrigin::Kind::Created, 0, tag}},
                  {0, 0, 0, 0, 0, tag});
    CHECK(s.saveAndRebase());
    CHECK(f.entry(0).contentEdits == nullptr);

    CHECK(f.load(0));
    const PageContentViewPtr after = f.content(0);
    CHECK_EQ(after->objects.size(), std::size_t{6});
    CHECK(after->objects[5].id == block);
    // It is an ordinary text block now, still addressed by the same id.
    const auto found = f.session->contentService().findBlock(f.id(0), block);
    CHECK(found.has_value());
    if (found) {
        CHECK_EQ(found->tag, std::uint64_t{0});
        CHECK_EQ(found->text, std::string("Added"));
    }
    // And it can be edited again.
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "Again"})));
    CHECK_EQ(f.entry(0).contentEdits->textBlocks[0].tag, tag);
}

RIVET_TEST(ContentSession_rebase_without_a_report_mints_fresh_ids) {
    SaveFixture s;
    auto& f = s.f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    std::set<ObjectId> old;
    for (const ContentObjectView& object : f.content(0)->objects) old.insert(object.id);
    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{2, 2})));
    f.engine.reopenedContents[0] = indexed(samplePage());
    // The default report has an empty entry per page.
    CHECK(s.saveAndRebase());
    CHECK(f.entry(0).contentEdits == nullptr);
    CHECK(f.load(0));
    CHECK_EQ(f.content(0)->objects.size(), std::size_t{5});
    for (const ContentObjectView& object : f.content(0)->objects) CHECK(old.count(object.id) == 0);
}

RIVET_TEST(ContentSession_untouched_pages_keep_their_ids_across_a_save) {
    SaveFixture s;
    auto& f = s.f;
    f.setContent(0, samplePage());
    f.setContent(1, samplePage());
    CHECK(f.load(0));
    CHECK(f.load(1));
    const ObjectId untouched = f.objectId(1, 0);
    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{2, 2})));
    f.engine.reopenedContents[0] = indexed(samplePage());
    f.engine.reopenedContents[1] = indexed(samplePage());
    s.reportPage0({source(0), source(1), source(2), source(3), source(4)}, {0, 0, 0, 0, 0});
    CHECK(s.saveAndRebase());
    CHECK(f.load(1));
    CHECK(f.content(1)->objects[0].id == untouched);
}

RIVET_TEST(ContentSession_failed_save_leaves_the_edits_in_place) {
    SaveFixture s;
    auto& f = s.f;
    f.setContent(0, samplePage());
    CHECK(f.load(0));
    const ObjectId image = f.objectId(0, 0);
    CHECK(f.run(moveContent(*f.session, f.id(0), {image}, Point{2, 2})));
    const auto edits = f.entry(0).contentEdits;
    const auto content = f.entry(0).contentRevision;

    f.engine.failAssembly = true;
    auto job = makeSaveJob(*f.session, s.dir / "out.pdf");
    CHECK(job.has_value());
    if (!job) return;
    auto result = runDocumentWrite(f.engine, *job);
    CHECK(!result.written.has_value());
    CHECK(f.session->isDirty());
    CHECK(f.entry(0).contentEdits == edits);
    CHECK_EQ(f.entry(0).contentRevision, content);
    CHECK(f.load(0));
    CHECK(f.content(0)->objects[0].id == image);
    // The edit can still be undone.
    CHECK(f.session->undo());
    CHECK(f.entry(0).contentEdits == nullptr);
}
