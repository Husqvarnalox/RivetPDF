// SPDX-License-Identifier: MPL-2.0
// Phase 5 end to end (real engine): revisions, object identity and the text
// search restart policy across content commands, undo/redo and a save with
// its rebase. Bodies return early without a backend.
#include "ContentIntegrationKit.hpp"

using namespace rivet;
using namespace rivet::test::integ;
using core::ObjectId;
using core::Point;
using core::Rect;
using pdf::PdfContentObjectType;

namespace {

std::vector<ObjectId> idsOf(const editor::PageContentView& view) {
    std::vector<ObjectId> ids;
    ids.reserve(view.objects.size());
    for (const auto& object : view.objects) ids.push_back(object.id);
    return ids;
}

bool hasId(const editor::PageContentView& view, ObjectId id) {
    return std::any_of(view.objects.begin(), view.objects.end(), [&](const auto& object) { return object.id == id; });
}

// Waits until the search walk has settled.
bool searchIdle(Rig& rig) {
    return rig.dispatcher.waitUntil([&] { return !rig.tab()->search()->searching(); });
}

bool runCommand(Rig& rig, core::Result<editor::ContentEdit> edit) {
    if (!edit.has_value()) {
        std::fprintf(stderr, "command refused: %s\n", edit.error().message.c_str());
        CHECK(edit.has_value());
        return false;
    }
    const core::Status status = rig.session().execute(std::move(edit->command));
    CHECK(static_cast<bool>(status));
    return static_cast<bool>(status);
}

} // namespace

// 5a. After every command / undo / redo the contentRevision and rasterRevision
// of the edited page change (and only theirs), page 2 keeps its stamp, the
// annotation state pointer of the edited page stays, and ids are stable.
RIVET_TEST(integRevisionsChangeOnlyForTheEditedPage) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich();
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    const auto view = rig.loaded(0);
    CHECK(view != nullptr);
    const auto view2 = rig.loaded(1);
    CHECK(view2 != nullptr);
    if (view == nullptr || view2 == nullptr) return;
    const std::vector<ObjectId> page2Ids = idsOf(*view2);
    const auto* hello = rig.blockWith(*view, "Hello World");
    const auto* paragraph = rig.blockWith(*view, "quick brown");
    const auto* image = rig.objectOfType(*view, PdfContentObjectType::Image, 0);
    const auto* path = rig.objectOfType(*view, PdfContentObjectType::Path, 0);
    CHECK(hello != nullptr && paragraph != nullptr && image != nullptr && path != nullptr);
    if (hello == nullptr || paragraph == nullptr || image == nullptr || path == nullptr) return;
    const ObjectId helloId = hello->id;
    const ObjectId paragraphId = paragraph->id;
    const ObjectId imageId = image->id;
    const ObjectId pathId = path->id;
    const Rect imageBounds = image->bounds;
    const core::PageId page0 = rig.pageId(0);

    const PageStamp initial0 = rig.stamp(0);
    const PageStamp initial1 = rig.stamp(1);

    std::vector<PageStamp> history{initial0};
    const auto step = [&](core::Result<editor::ContentEdit> edit, const char* what) {
        const PageStamp before = rig.stamp(0);
        const bool ran = runCommand(rig, std::move(edit));
        CHECK(ran);
        if (!ran) return false;
        const PageStamp after = rig.stamp(0);
        if (!(after.content > before.content)) std::fprintf(stderr, "content revision not increased after %s\n", what);
        CHECK(after.content > before.content);
        CHECK(after.raster > before.raster);
        CHECK(after.edits != before.edits);
        CHECK(after.annotations == before.annotations);
        CHECK(rig.stamp(1) == initial1);
        history.push_back(after);
        // The page re-extracts; the untouched objects keep their ids.
        const auto now = rig.loaded(0);
        CHECK(now != nullptr);
        if (now == nullptr) return false;
        CHECK(hasId(*now, paragraphId) || std::string(what) == "delete paragraph");
        CHECK(hasId(*now, helloId) || std::string(what) == "delete hello");
        return true;
    };

    auto& session = rig.session();
    CHECK(step(editor::moveContent(session, page0, {helloId}, Point{6.0, 4.0}), "move"));
    CHECK(step(editor::editTextBlock(session, page0, helloId, editor::TextBlockPatch{.text = std::string("Howdy")}),
               "edit text"));
    CHECK(step(editor::resizeContent(session, page0, imageId,
                                     Rect{imageBounds.minX(), imageBounds.minY(), imageBounds.size.width * 2.0,
                                          imageBounds.size.height * 2.0}),
               "resize"));
    CHECK(step(editor::deleteContent(session, page0, {pathId}), "delete path"));
    CHECK_EQ(rig.depth(), std::size_t{4});
    {
        const auto now = rig.contentNow(0);
        CHECK(!hasId(*now, pathId));
        CHECK(hasId(*now, imageId));
        CHECK(hasId(*now, paragraphId));
        const auto* edited = rig.blockWith(*now, "Howdy");
        CHECK(edited != nullptr && edited->id == helloId);
    }

    // Undo walks the revisions back (each step changes them), redo forward.
    for (int i = 0; i < 4; ++i) {
        const PageStamp before = rig.stamp(0);
        CHECK(session.undo());
        const PageStamp after = rig.stamp(0);
        CHECK(after.content != before.content);
        CHECK(after.raster != before.raster);
        CHECK(after.annotations == before.annotations);
        CHECK(rig.stamp(1) == initial1);
    }
    CHECK(!session.isDirty());
    {
        const auto now = rig.loaded(0);
        CHECK(now != nullptr);
        if (now != nullptr) {
            CHECK(idsOf(*now) == idsOf(*view));
            CHECK(rig.blockWith(*now, "Hello World") != nullptr);
        }
    }
    for (int i = 0; i < 4; ++i) {
        const PageStamp before = rig.stamp(0);
        CHECK(session.redo());
        const PageStamp after = rig.stamp(0);
        CHECK(after.content != before.content);
        CHECK(after.annotations == before.annotations);
        CHECK(rig.stamp(1) == initial1);
    }
    {
        const auto now = rig.loaded(0);
        CHECK(now != nullptr);
        if (now != nullptr) {
            CHECK(!hasId(*now, pathId));
            CHECK(hasId(*now, imageId) && hasId(*now, paragraphId));
        }
        // Page 2 never re-extracted differently: same objects, same ids.
        const auto after2 = rig.loaded(1);
        CHECK(after2 != nullptr);
        if (after2 != nullptr) CHECK(idsOf(*after2) == page2Ids);
    }
    // The revisions of every execute step were strictly increasing.
    for (std::size_t i = 1; i < history.size(); ++i) CHECK(history[i].content > history[i - 1].content);
}

// 5b. A content edit restarts the running search over the new text (the
// token is monotonic, matches are re-found in the edited text and no stale
// character index is reused); undo restores the original indices.
RIVET_TEST(integSearchRestartsAfterEveryContentEdit) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich();
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    CHECK(tab->search() != nullptr);
    if (tab->search() == nullptr) return;
    rig.selectTool();
    CHECK(rig.loaded(0) != nullptr);
    auto& search = *tab->search();

    search.start("Hello");
    CHECK(searchIdle(rig));
    CHECK_EQ(search.matchCount(), std::size_t{1});
    const auto before = search.matches();
    CHECK_EQ(before.size(), std::size_t{1});
    if (before.size() != 1) return;
    CHECK(before[0].page == rig.pageId(0));
    CHECK_EQ(before[0].count, std::uint32_t{5});
    const std::uint32_t originalIndex = before[0].startIndex;
    std::uint64_t token = search.activeRequestForTesting();

    // The text shifts the match by 4 characters ("Say " prepended).
    CHECK(retype(rig, "Hello World", "Say Hello World"));
    CHECK(search.activeRequestForTesting() > token);
    token = search.activeRequestForTesting();
    CHECK(searchIdle(rig));
    {
        const auto matches = search.matches();
        CHECK_EQ(matches.size(), std::size_t{1});
        if (matches.size() == 1) {
            CHECK(matches[0].page == rig.pageId(0));
            CHECK(matches[0].startIndex != originalIndex);
            CHECK_EQ(matches[0].startIndex, originalIndex + 4);
            // The index addresses the EDITED text page.
            const editor::PageEntry* entry = rig.session().pageSnapshot()->find(rig.pageId(0));
            CHECK(entry != nullptr);
            if (entry != nullptr) {
                auto text = entry->source->textPage(entry->sourcePageIndex, entry->view, entry->contentEdits);
                CHECK(text.has_value());
                if (text.has_value()) CHECK_EQ((*text)->text().find("Hello"), std::size_t{matches[0].startIndex});
            }
        }
    }

    // Replacing the word removes the match altogether.
    CHECK(rig.awaitBlock("Say Hello World").has_value());
    CHECK(retype(rig, "Say Hello World", "Greetings"));
    CHECK(search.activeRequestForTesting() > token);
    token = search.activeRequestForTesting();
    CHECK(searchIdle(rig));
    CHECK_EQ(search.matchCount(), std::size_t{0});
    CHECK(!search.currentIndex().has_value());

    // Undo twice: the original match (and index) is back.
    CHECK(rig.session().undo());
    CHECK(search.activeRequestForTesting() > token);
    token = search.activeRequestForTesting();
    CHECK(rig.session().undo());
    CHECK(search.activeRequestForTesting() > token);
    token = search.activeRequestForTesting();
    CHECK(searchIdle(rig));
    {
        const auto matches = search.matches();
        CHECK_EQ(matches.size(), std::size_t{1});
        if (matches.size() == 1) CHECK_EQ(matches[0].startIndex, originalIndex);
    }

    // A move changes the page's content revision too: restart, same result.
    const auto view = rig.loaded(0);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    const auto* image = rig.objectOfType(*view, PdfContentObjectType::Image, 0);
    CHECK(image != nullptr);
    if (image == nullptr) return;
    CHECK(runCommand(rig, editor::moveContent(rig.session(), rig.pageId(0), {image->id}, Point{5.0, 5.0})));
    CHECK(search.activeRequestForTesting() > token);
    CHECK(searchIdle(rig));
    {
        const auto matches = search.matches();
        CHECK_EQ(matches.size(), std::size_t{1});
        if (matches.size() == 1) CHECK_EQ(matches[0].startIndex, originalIndex);
    }

    // A search that matches on the untouched page 2 keeps pointing there.
    search.start("Page Two");
    CHECK(searchIdle(rig));
    CHECK_EQ(search.matchCount(), std::size_t{1});
    CHECK(rig.awaitBlock("Hello World").has_value());
    CHECK(retype(rig, "Hello World", "Another"));
    CHECK(searchIdle(rig));
    {
        const auto matches = search.matches();
        CHECK_EQ(matches.size(), std::size_t{1});
        if (matches.size() == 1) CHECK(matches[0].page == rig.pageId(1));
    }
}

// 5c. A save rebases the session onto the written file: the objects the user
// did not touch keep their ids, the edited block keeps its id, and a second
// edit + save cycle keeps them too.
RIVET_TEST(integObjectIdsSurviveSaveAndRebase) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich();
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.selectTool();
    const auto view = rig.loaded(0);
    const auto view2 = rig.loaded(1);
    CHECK(view != nullptr && view2 != nullptr);
    if (view == nullptr || view2 == nullptr) return;
    const auto* hello = rig.blockWith(*view, "Hello World");
    const auto* paragraph = rig.blockWith(*view, "quick brown");
    const auto* image = rig.objectOfType(*view, PdfContentObjectType::Image, 0);
    const auto* path = rig.objectOfType(*view, PdfContentObjectType::Path, 0);
    CHECK(hello != nullptr && paragraph != nullptr && image != nullptr && path != nullptr);
    if (hello == nullptr || paragraph == nullptr || image == nullptr || path == nullptr) return;
    const ObjectId helloId = hello->id;
    const ObjectId paragraphId = paragraph->id;
    const ObjectId imageId = image->id;
    const ObjectId pathId = path->id;
    const Rect imageBounds = image->bounds;
    const std::vector<ObjectId> page2Ids = idsOf(*view2);
    const std::string paragraphText = paragraph->text;

    // Edit the greeting and move the image, save.
    CHECK(retype(rig, "Hello World", "Howdy Planet"));
    CHECK(rig.awaitBlock("Howdy Planet").has_value());
    CHECK(runCommand(rig, editor::moveContent(rig.session(), rig.pageId(0), {imageId}, Point{10.0, 0.0})));
    CHECK(rig.saveAndWait());
    CHECK(!rig.session().isDirty());

    for (int cycle = 0; cycle < 2; ++cycle) {
        const auto edited = rig.awaitBlock(cycle == 0 ? "Howdy Planet" : "Second pass");
        CHECK(edited.has_value());
        if (!edited.has_value()) return;
        CHECK(edited->id == helloId);
        const auto now = rig.loaded(0);
        CHECK(now != nullptr);
        if (now == nullptr) return;
        CHECK(hasId(*now, imageId));
        CHECK(hasId(*now, pathId) == (cycle == 0)); // deleted by the second cycle
        const auto* para = rig.blockWith(*now, "quick brown");
        CHECK(para != nullptr);
        if (para != nullptr) {
            CHECK(para->id == paragraphId);
            CHECK_EQ(para->text, paragraphText);
        }
        const auto* moved = rig.objectById(*now, imageId);
        CHECK(moved != nullptr);
        if (moved != nullptr) CHECK_NEAR(moved->bounds.minX(), imageBounds.minX() + 10.0, 0.5);
        const auto after2 = rig.loaded(1);
        CHECK(after2 != nullptr);
        if (after2 != nullptr) CHECK(idsOf(*after2) == page2Ids);

        if (cycle == 0) {
            // Edit the (already edited) block again, plus the path, and save
            // once more.
            CHECK(retype(rig, "Howdy Planet", "Second pass"));
            CHECK(rig.awaitBlock("Second pass").has_value());
            CHECK(runCommand(rig, editor::deleteContent(rig.session(), rig.pageId(0), {pathId})));
            CHECK(rig.saveAndWait());
            CHECK(!rig.session().isDirty());
        }
    }
    // The deleted path is gone for good; the rest is as before.
    {
        const auto edited = rig.awaitBlock("Second pass");
        CHECK(edited.has_value() && edited->id == helloId);
        CHECK(rig.awaitNoBlock("Howdy Planet"));
        const auto now = rig.contentNow(0);
        CHECK(now != nullptr);
        if (now != nullptr) {
            CHECK(!hasId(*now, pathId));
            CHECK(hasId(*now, imageId));
        }
    }
    auto reopened = rig.reopen(rig.session().path());
    CHECK(reopened != nullptr);
    if (reopened == nullptr) return;
    CHECK(contains(textOf(*reopened, 0), "Second pass"));
    CHECK(contains(textOf(*reopened, 0), "quick brown"));
    CHECK_EQ(annotationCount(*reopened, 0), std::size_t{2});
}
