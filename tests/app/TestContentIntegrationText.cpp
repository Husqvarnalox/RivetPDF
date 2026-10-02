// SPDX-License-Identifier: MPL-2.0
// Phase 5 end to end (real engine): in-place text edits and Add Text through
// the content tools, saved and reopened. Bodies return early without a
// backend.
#include "ContentIntegrationKit.hpp"

using namespace rivet;
using namespace rivet::test::integ;
using core::ObjectId;
using core::Point;
using core::Rect;

// 2. Retype text in place (Latin keeps the font, Cyrillic substitutes a
// bundled one), save through Save As and Cmd+S, reopen and compare.
RIVET_TEST(integEditTextInPlaceThenSaveAndReopen) {
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
    const auto zBefore = firstMemberIndex(*view, blockId);
    CHECK(zBefore.has_value());
    CHECK(hello->capability == editor::ContentCapability::FullyEditable);
    CHECK(!hello->fontSubstituted);

    auto& original = *rig.session().documentPtr();
    const std::size_t annotationsBefore = annotationCount(original, 0);
    const std::size_t linksBefore = linkCount(original, 0);
    CHECK_EQ(annotationsBefore, std::size_t{2});
    CHECK_EQ(linksBefore, std::size_t{1});

    // Latin: one undo step, the text changed, the font kept, same z-index.
    const std::size_t depth0 = rig.depth();
    CHECK(retype(rig, "Hello World", "Howdy Planet"));
    CHECK_EQ(rig.depth(), depth0 + 1);
    const auto latin = rig.awaitBlock("Howdy Planet");
    CHECK(latin.has_value());
    if (!latin.has_value()) return;
    CHECK(latin->id == blockId);
    CHECK(!latin->fontSubstituted);
    CHECK(contains(latin->font.baseName, "Helvetica"));
    CHECK(latin->edited);
    {
        const auto now = rig.contentNow(0);
        const auto z = firstMemberIndex(*now, blockId);
        CHECK(z.has_value());
        if (z.has_value() && zBefore.has_value()) CHECK_EQ(*z, *zBefore);
    }

    // Cyrillic: the object's own Helvetica cannot encode it, a bundled font
    // is substituted for the whole block.
    CHECK(retype(rig, "Howdy Planet", "Привет мир"));
    CHECK_EQ(rig.depth(), depth0 + 2);
    const auto cyrillic = rig.awaitBlock("Привет мир");
    CHECK(cyrillic.has_value());
    if (!cyrillic.has_value()) return;
    CHECK(cyrillic->id == blockId);
    CHECK(cyrillic->fontSubstituted);
    CHECK(cyrillic->capability != editor::ContentCapability::ReadOnly);
    CHECK(rig.awaitNoBlock("Howdy Planet"));

    // What a save will write, rendered from the live document + edits.
    const editor::PageEntry* entry = rig.session().pageSnapshot()->find(rig.pageId(0));
    CHECK(entry != nullptr && entry->contentEdits != nullptr);
    if (entry == nullptr) return;
    const core::Bitmap expected = renderOf(original, 0, entry->contentEdits);
    CHECK(expected.isValid());

    // Save As -> reopen with the engine.
    const fs::path saved = rig.dir("edited.pdf");
    CHECK(rig.saveAsAndWait(saved));
    CHECK(!rig.session().isDirty());
    {
        auto reopened = rig.reopen(saved);
        CHECK(reopened != nullptr);
        if (reopened == nullptr) return;
        const std::string text = textOf(*reopened, 0);
        CHECK(contains(text, "Привет мир"));
        CHECK(!contains(text, "Hello World"));
        CHECK(!contains(text, "Howdy Planet"));
        CHECK(contains(text, "The quick brown fox"));
        CHECK(contains(text, "dog and keeps going"));
        CHECK_EQ(annotationCount(*reopened, 0), annotationsBefore);
        CHECK_EQ(linkCount(*reopened, 0), linksBefore);
        CHECK(contains(textOf(*reopened, 1), "Page Two"));

        const core::Bitmap actual = renderOf(*reopened, 0);
        CHECK(actual.isValid());
        CHECK_NEAR(differingFraction(expected, actual), 0.0, 0.01);
    }

    // The edited block keeps its identity across the rebase and can be edited
    // again; Cmd+S writes the new state to the (retitled) tab's file.
    const auto afterSave = rig.awaitBlock("Привет мир");
    CHECK(afterSave.has_value());
    if (afterSave.has_value()) CHECK(afterSave->id == blockId);
    CHECK(retype(rig, "Привет мир", "Final text"));
    CHECK(rig.session().isDirty());
    CHECK(rig.saveAndWait());
    CHECK(rig.session().path() == saved);
    {
        auto reopened = rig.reopen(saved);
        CHECK(reopened != nullptr);
        if (reopened == nullptr) return;
        const std::string text = textOf(*reopened, 0);
        CHECK(contains(text, "Final text"));
        CHECK(!contains(text, "Привет мир"));
        CHECK_EQ(annotationCount(*reopened, 0), annotationsBefore);
        CHECK_EQ(linkCount(*reopened, 0), linksBefore);
    }
}

// 3. Add Text (Latin + Cyrillic, 3 lines, Serif Bold 18 red): on top, saved
// with an embedded font; delete / undo / redo before and after the save.
RIVET_TEST(integAddTextIsOnTopEmbedsItsFontAndSurvivesDeleteUndoRedo) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich();
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.addTextTool();
    const auto view = rig.loaded(0);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    const std::size_t objectsBefore = view->objects.size();
    const std::size_t blocksBefore = view->blocks.size();
    const PageStamp page1 = rig.stamp(1);

    rig.content->setFontFamily(app::ContentController::FontFamily::Serif);
    rig.content->setBold(true);
    rig.content->setFontSize(18.0);
    rig.content->setTextColor(pdf::PdfColor{1.0F, 0.0F, 0.0F});
    const auto style = rig.content->displayedStyle();
    CHECK(style.family == app::ContentController::FontFamily::Serif);
    CHECK(style.bold);
    CHECK_NEAR(style.size, 18.0, 1e-9);

    const std::string typed = "Line one\nЛиния два\nLine three";
    const std::size_t depth0 = rig.depth();
    rig.click(Point{40.0, 330.0});
    CHECK(rig.content->editorOpen());
    rig.content->editorArea().setText(typed);
    CHECK(rig.content->commitEditor());
    CHECK_EQ(rig.depth(), depth0 + 1);
    // The new block is selected as soon as the page re-extracts (the
    // selection is kept while the content reloads).
    CHECK(rig.dispatcher.waitUntil([&] { return rig.content->selected().has_value(); }));
    const auto selected = rig.content->selected();
    CHECK(selected.has_value());
    if (!selected.has_value()) return;
    const ObjectId newId = selected->info.id;
    CHECK(selected->info.isBlock);
    CHECK(selected->info.rivetBlock);

    const auto added = rig.awaitBlock("Line one");
    CHECK(added.has_value());
    if (!added.has_value()) return;
    CHECK(added->id == newId);
    CHECK_EQ(added->text, typed);
    CHECK_EQ(added->lines.size(), std::size_t{3});
    CHECK(added->font.bold);
    CHECK_NEAR(added->fontSize, 18.0, 0.01);
    CHECK_NEAR(static_cast<double>(added->color.r), 1.0, 0.01);
    CHECK_NEAR(static_cast<double>(added->color.g), 0.0, 0.01);
    CHECK(added->tag != 0);
    CHECK(contains(added->font.baseName, "Tinos")); // the bundled serif
    // PDFium writes no serif flag for fonts loaded from bytes: describeFont
    // classifies the bundled serif (Tinos) by name, so the content bar shows
    // Serif for the block and the retype coverage check uses the serif face.
    CHECK(added->font.serif);
    CHECK(rig.content->displayedStyle().family == app::ContentController::FontFamily::Serif);
    {
        // Topmost: the new block's objects hold the highest z-indices.
        const auto now = rig.contentNow(0);
        CHECK(now != nullptr);
        if (now != nullptr) {
            CHECK(now->objects.size() > objectsBefore);
            CHECK_EQ(now->blocks.size(), blocksBefore + 1);
            std::uint32_t top = 0;
            std::uint32_t topOfNew = 0;
            for (const auto& object : now->objects) {
                top = std::max(top, object.index);
                if (object.block == newId) topOfNew = std::max(topOfNew, object.index);
            }
            CHECK_EQ(topOfNew, top);
            const auto lowestNew = firstMemberIndex(*now, newId);
            CHECK(lowestNew.has_value());
            if (lowestNew.has_value()) CHECK_EQ(*lowestNew, static_cast<std::uint32_t>(objectsBefore));
        }
    }
    CHECK(rig.stamp(1) == page1);

    // Retyping a Rivet block keeps its (bundled) font.
    rig.selectTool();
    CHECK(retype(rig, "Line one", "Zeta\nЛиния два\nLine three"));
    {
        const auto again = rig.awaitBlock("Zeta");
        CHECK(again.has_value());
        if (again.has_value()) {
            CHECK(again->id == newId);
            CHECK(contains(again->font.baseName, "Tinos"));
            CHECK(again->font.bold);
        }
    }
    CHECK(rig.session().undo());
    CHECK(rig.awaitBlock("Line one").has_value());

    // Delete (Delete key), undo, redo: the same id comes back.
    rig.click(centerOf(added->bounds));
    {
        CHECK(rig.dispatcher.waitUntil([&] { return rig.content->selected().has_value(); }));
        const auto now = rig.content->selected();
        CHECK(now.has_value() && now->info.id == newId);
    }
    const std::size_t depthBeforeDelete = rig.depth();
    CHECK(rig.key(ui::Key::Delete));
    CHECK_EQ(rig.depth(), depthBeforeDelete + 1);
    CHECK(rig.awaitNoBlock("Line one"));
    CHECK(rig.session().undo());
    {
        const auto back = rig.awaitBlock("Line one");
        CHECK(back.has_value());
        if (back.has_value()) CHECK(back->id == newId);
    }
    CHECK(rig.session().redo());
    CHECK(rig.awaitNoBlock("Line one"));
    CHECK(rig.session().undo());
    CHECK(rig.awaitBlock("Line one").has_value());

    // Save; the font of the new text is embedded in the written file.
    CHECK(rig.saveAndWait());
    const fs::path saved = rig.session().path();
    {
        auto reopened = rig.reopen(saved);
        CHECK(reopened != nullptr);
        if (reopened == nullptr) return;
        const std::string text = textOf(*reopened, 0);
        CHECK(contains(text, "Line one"));
        CHECK(contains(text, "Линия два"));
        CHECK(contains(text, "Line three"));
        CHECK(contains(text, "Hello World"));
        CHECK_EQ(annotationCount(*reopened, 0), std::size_t{2});
        const auto content = contentOf(*reopened, 0);
        CHECK(content != nullptr);
        if (content == nullptr) return;
        std::size_t created = 0;
        for (const auto& object : content->objects) {
            if (object.type != pdf::PdfContentObjectType::Text) continue;
            if (!contains(object.text, "Line") && !contains(object.text, "Линия")) continue;
            ++created;
            CHECK(object.font.embedded);
        }
        CHECK_EQ(created, std::size_t{3});
    }

    // After the rebase the block is a regular object of the file: delete,
    // undo and redo still work, and the delete is saved.
    const auto rebased = rig.awaitBlock("Line one");
    CHECK(rebased.has_value());
    if (!rebased.has_value()) return;
    CHECK(rebased->id == newId);
    rig.click(centerOf(rebased->bounds));
    CHECK(rig.dispatcher.waitUntil([&] { return rig.content->selected().has_value(); }));
    const std::size_t depthAfterSave = rig.depth();
    CHECK(rig.key(ui::Key::Delete));
    CHECK_EQ(rig.depth(), depthAfterSave + 1);
    CHECK(rig.awaitNoBlock("Line one"));
    CHECK(rig.session().undo());
    CHECK(rig.awaitBlock("Line one").has_value());
    CHECK(rig.session().redo());
    CHECK(rig.awaitNoBlock("Line one"));
    CHECK(rig.saveAndWait());
    {
        auto reopened = rig.reopen(saved);
        CHECK(reopened != nullptr);
        if (reopened == nullptr) return;
        const std::string text = textOf(*reopened, 0);
        CHECK(!contains(text, "Line one"));
        CHECK(!contains(text, "Линия два"));
        CHECK(contains(text, "Hello World"));
    }
}
