// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "fakes/ContentTestSupport.hpp"

#include "editor/PageCommands.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <cmath>
#include <set>

// Content edit commands over the fake backend: copy-on-write edits, identity
// rules, display/user space mapping, failure paths, undo/redo and revision
// minting, dirty state (ADR-0014, ADR-0015).

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;
using core::ErrorCode;
using core::ObjectId;
using core::PageId;
using core::Point;
using core::Rect;

namespace {

// Loads page 0 with the sample content.
void sample(ContentFixture& f, std::size_t page = 0) {
    f.setContent(page, samplePage());
    CHECK(f.load(page));
}

// The edits currently bound to a page.
const pdf::PdfPageContentEdits& editsOf(ContentFixture& f, std::size_t page = 0) {
    static const pdf::PdfPageContentEdits kNone;
    const auto& edits = f.entry(page).contentEdits;
    return edits != nullptr ? *edits : kNone;
}

NewTextBlock newText(const char* text) {
    NewTextBlock block;
    block.text = text;
    return block;
}

bool hasError(const core::Result<ContentEdit>& result, ErrorCode code) {
    return !result.has_value() && result.error().code == code;
}

const pdf::PdfObjectEdit* objectEditAt(const pdf::PdfPageContentEdits& edits, std::uint32_t index) {
    for (const pdf::PdfObjectEdit& edit : edits.objects) {
        if (edit.sourceIndex == index) return &edit;
    }
    return nullptr;
}

// Content with a three-line paragraph (objects 0..5) and one image (6).
pdf::PdfPageContent paragraphPage() {
    pdf::PdfPageContent content;
    addTextLine(content, 700.0);
    addTextLine(content, 685.6);
    addTextLine(content, 671.2);
    content.objects.push_back(makeImageObject(300, 300, 400, 400));
    return content;
}

} // namespace

// --- Move ---------------------------------------------------------------------------

RIVET_TEST(ContentCommands_move_image_maps_the_display_delta_to_user_space) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 1);
    const Rect before = f.content(0)->objects[1].bounds;

    auto edit = moveContent(*f.session, f.id(0), {image}, Point{10.0, 20.0});
    CHECK(edit.has_value());
    if (!edit) return;
    CHECK_EQ(edit->ids.size(), std::size_t{1});
    CHECK(edit->ids[0] == image);
    CHECK_EQ(std::string(edit->command->name()), std::string("Move"));
    CHECK(f.session->execute(std::move(edit->command)).has_value());

    const auto& edits = editsOf(f);
    CHECK_EQ(edits.objects.size(), std::size_t{1});
    CHECK_EQ(edits.objects[0].sourceIndex, std::uint32_t{1});
    CHECK(!edits.objects[0].remove);
    CHECK(edits.objects[0].transform.has_value());
    if (edits.objects[0].transform) {
        // Display y is down, user y is up.
        CHECK_NEAR(edits.objects[0].transform->tx, 10.0, 1e-9);
        CHECK_NEAR(edits.objects[0].transform->ty, -20.0, 1e-9);
        CHECK_NEAR(edits.objects[0].transform->a, 1.0, 1e-12);
    }
    CHECK(edits.textBlocks.empty());

    // The re-extracted object has the same id at the moved position.
    CHECK(f.load(0));
    const ContentObjectView moved = f.content(0)->objects[1];
    CHECK(moved.id == image);
    CHECK(moved.edited);
    CHECK_NEAR(moved.bounds.minX(), before.minX() + 10.0, 1e-9);
    CHECK_NEAR(moved.bounds.minY(), before.minY() + 20.0, 1e-9);
}

RIVET_TEST(ContentCommands_move_undo_redo_restore_identity_and_mint_revisions) {
    ContentFixture f;
    sample(f);
    f.setContent(1, samplePage());
    const auto c0 = f.entry(0).contentRevision;
    const auto r0 = f.entry(0).rasterRevision;
    const auto c1 = f.entry(1).contentRevision;
    const auto r1 = f.entry(1).rasterRevision;

    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{3, 4})));
    const pdf::PdfPageContentEditsPtr afterExecute = f.entry(0).contentEdits;
    CHECK(afterExecute != nullptr);
    const auto c1st = f.entry(0).contentRevision;
    const auto r1st = f.entry(0).rasterRevision;
    CHECK(c1st != c0);
    CHECK(r1st != r0);
    // Other pages are untouched.
    CHECK_EQ(f.entry(1).contentRevision, c1);
    CHECK_EQ(f.entry(1).rasterRevision, r1);
    CHECK(f.entry(1).contentEdits == nullptr);
    CHECK(f.session->isDirty());

    CHECK(f.session->undo());
    CHECK(f.entry(0).contentEdits == nullptr);
    const auto c2nd = f.entry(0).contentRevision;
    const auto r2nd = f.entry(0).rasterRevision;
    CHECK(c2nd != c0);
    CHECK(c2nd != c1st);
    CHECK(r2nd != r0);
    CHECK(r2nd != r1st);
    CHECK(!f.session->isDirty());

    CHECK(f.session->redo());
    // The very same immutable edits object comes back.
    CHECK(f.entry(0).contentEdits == afterExecute);
    CHECK(f.entry(0).contentRevision != c2nd);
    CHECK(f.entry(0).contentRevision != c1st);
    CHECK(f.entry(0).rasterRevision != r2nd);
    CHECK(f.session->isDirty());
}

RIVET_TEST(ContentCommands_moves_compose) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 0);
    CHECK(f.run(moveContent(*f.session, f.id(0), {image}, Point{10, 0})));
    CHECK(f.load(0));
    CHECK(f.run(moveContent(*f.session, f.id(0), {image}, Point{5, 7})));
    const auto& edits = editsOf(f);
    CHECK_EQ(edits.objects.size(), std::size_t{1});
    CHECK(edits.objects[0].transform.has_value());
    if (edits.objects[0].transform) {
        CHECK_NEAR(edits.objects[0].transform->tx, 15.0, 1e-9);
        CHECK_NEAR(edits.objects[0].transform->ty, -7.0, 1e-9);
    }
    // Undoing both brings back the pristine page.
    CHECK(f.session->undo());
    CHECK(f.session->undo());
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(ContentCommands_move_unedited_block_moves_every_member) {
    ContentFixture f;
    sample(f);
    const ObjectId block = f.content(0)->blocks[0].id;
    CHECK(f.run(moveContent(*f.session, f.id(0), {block}, Point{-4, 6})));
    const auto& edits = editsOf(f);
    CHECK_EQ(edits.objects.size(), std::size_t{2});
    CHECK_EQ(edits.objects[0].sourceIndex, std::uint32_t{2});
    CHECK_EQ(edits.objects[1].sourceIndex, std::uint32_t{3});
    CHECK(edits.textBlocks.empty());
    CHECK(f.load(0));
    // Same block id afterwards (its first member keeps its object id).
    CHECK(f.content(0)->blocks[0].id == block);
}

RIVET_TEST(ContentCommands_move_mixed_selection_and_duplicates) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 0);
    const ObjectId block = f.content(0)->blocks[0].id;
    const ObjectId word2 = f.objectId(0, 3);
    CHECK(f.run(moveContent(*f.session, f.id(0), {image, block, image, word2}, Point{1, 1})));
    // Image + two words; the duplicate id and the member listed on top of its
    // block add nothing.
    CHECK_EQ(editsOf(f).objects.size(), std::size_t{3});
}

RIVET_TEST(ContentCommands_move_failure_paths) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 0);
    CHECK(hasError(moveContent(*f.session, f.id(0), {}, Point{1, 1}), ErrorCode::InvalidArgument));
    CHECK(hasError(moveContent(*f.session, f.id(0), {ObjectId{999999}}, Point{1, 1}), ErrorCode::NotFound));
    CHECK(hasError(moveContent(*f.session, PageId{424242}, {image}, Point{1, 1}), ErrorCode::NotFound));
    CHECK(hasError(moveContent(*f.session, f.id(0), {image}, Point{std::nan(""), 1}), ErrorCode::InvalidArgument));
    // The unknown-type object is read-only.
    auto readOnly = moveContent(*f.session, f.id(0), {f.objectId(0, 4)}, Point{1, 1});
    CHECK(hasError(readOnly, ErrorCode::Unsupported));
    if (!readOnly) CHECK(!readOnly.error().message.empty());
    // One bad id fails the whole command; nothing was applied.
    CHECK(hasError(moveContent(*f.session, f.id(0), {image, ObjectId{999999}}, Point{1, 1}), ErrorCode::NotFound));
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(ContentCommands_factories_refuse_until_the_page_content_is_loaded) {
    ContentFixture f;
    f.setContent(0, samplePage());
    auto early = moveContent(*f.session, f.id(0), {ObjectId{1}}, Point{1, 1});
    CHECK(hasError(early, ErrorCode::NotAvailable));
    CHECK(hasError(addTextBlock(*f.session, f.id(1), newText("x")), ErrorCode::NotAvailable));
    CHECK(f.load(0));
    CHECK(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{1, 1}).has_value());
}

RIVET_TEST(ContentCommands_identity_factories_accept_a_stale_view_after_an_edit) {
    // After an edit the resolved view is stale (`loaded` = false) until the
    // backend re-extracts. Key-repeat nudges, back-to-back drags, deletes,
    // retypes and Add Text must still go through (identities and the edits
    // are current); resizing reads geometry and waits for the fresh view.
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 1);
    const ObjectId block = f.content(0)->blocks[0].id;
    CHECK(f.run(moveContent(*f.session, f.id(0), {image}, Point{10, 0})));
    CHECK(!f.content(0)->loaded);
    CHECK(!f.content(0)->objects.empty());

    // Moves compose on the stale view.
    CHECK(f.run(moveContent(*f.session, f.id(0), {image}, Point{5, 7})));
    const pdf::PdfObjectEdit* edit = objectEditAt(editsOf(f), 1);
    CHECK(edit != nullptr && edit->transform.has_value());
    if (edit != nullptr && edit->transform) {
        CHECK_NEAR(edit->transform->tx, 15.0, 1e-9);
        CHECK_NEAR(edit->transform->ty, -7.0, 1e-9);
    }
    // Resize needs fresh geometry.
    CHECK(hasError(resizeContent(*f.session, f.id(0), image, Rect{Point{0, 0}, core::Size{10, 10}}),
                   ErrorCode::NotAvailable));
    CHECK(f.load(0));
    CHECK(resizeContent(*f.session, f.id(0), image, Rect{Point{0, 0}, core::Size{10, 10}}).has_value());

    // A retype followed by a second retype and a move on the stale view
    // address the SAME edit (the replaced block's tag is its id), never a
    // duplicate block nor transforms on the replaced members.
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "first"})));
    CHECK(!f.content(0)->loaded);
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "second"})));
    CHECK(f.run(moveContent(*f.session, f.id(0), {block}, Point{3, 0})));
    CHECK_EQ(editsOf(f).textBlocks.size(), std::size_t{1});
    if (!editsOf(f).textBlocks.empty()) {
        CHECK_EQ(editsOf(f).textBlocks[0].text, std::string("second"));
        CHECK_EQ(editsOf(f).textBlocks[0].tag, block.value());
        CHECK_NEAR(editsOf(f).textBlocks[0].placement.tx, 75.0, 1e-9); // 72 + 3 (see the placement test)
    }
    for (const std::uint32_t member : editsOf(f).textBlocks[0].members) {
        const pdf::PdfObjectEdit* memberEdit = objectEditAt(editsOf(f), member);
        CHECK(memberEdit == nullptr || !memberEdit->transform.has_value());
    }
    CHECK(f.run(addTextBlock(*f.session, f.id(0), newText("x"))));
    CHECK_EQ(editsOf(f).textBlocks.size(), std::size_t{2});

    // A deleted object still shown by the stale view cannot be edited again.
    CHECK(f.run(deleteContent(*f.session, f.id(0), {image})));
    CHECK(hasError(moveContent(*f.session, f.id(0), {image}, Point{1, 1}), ErrorCode::NotFound));
    CHECK(hasError(replaceImage(*f.session, f.id(0), image, makeBgraImage()), ErrorCode::NotFound));
    CHECK(f.run(deleteContent(*f.session, f.id(0), {block})));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "z"}), ErrorCode::NotFound));
    CHECK(hasError(moveContent(*f.session, f.id(0), {block}, Point{1, 1}), ErrorCode::NotFound));
    CHECK(f.load(0));
    CHECK(f.content(0)->loaded);
}

RIVET_TEST(ContentCommands_locked_session_refuses_every_factory) {
    ContentFixture f;
    sample(f);
    f.session->setEditingLocked(true, "saving the document");
    const ObjectId image = f.objectId(0, 0);
    const auto check = [](const core::Result<ContentEdit>& result) {
        CHECK(hasError(result, ErrorCode::Unsupported));
        if (!result) CHECK_EQ(result.error().message, std::string("saving the document"));
    };
    check(moveContent(*f.session, f.id(0), {image}, Point{1, 1}));
    check(deleteContent(*f.session, f.id(0), {image}));
    check(resizeContent(*f.session, f.id(0), image, Rect{Point{0, 0}, core::Size{10, 10}}));
    check(replaceImage(*f.session, f.id(0), image, makeBgraImage()));
    check(editTextBlock(*f.session, f.id(0), f.content(0)->blocks[0].id, TextBlockPatch{.text = "x"}));
    check(addTextBlock(*f.session, f.id(0), newText("x")));
    check(bringToFront(*f.session, f.id(0), image));
    f.session->setEditingLocked(false);
    CHECK(moveContent(*f.session, f.id(0), {image}, Point{1, 1}).has_value());
}

RIVET_TEST(ContentCommands_a_stale_command_is_refused_and_changes_nothing) {
    ContentFixture f;
    sample(f);
    auto first = moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{1, 1});
    auto second = moveContent(*f.session, f.id(0), {f.objectId(0, 1)}, Point{2, 2});
    CHECK(first.has_value());
    CHECK(second.has_value());
    if (!first || !second) return;
    CHECK(f.session->execute(std::move(second->command)).has_value());
    const auto edits = f.entry(0).contentEdits;
    const auto revision = f.entry(0).contentRevision;
    const auto status = f.session->execute(std::move(first->command));
    CHECK(!status.has_value());
    CHECK(f.entry(0).contentEdits == edits);
    CHECK_EQ(f.entry(0).contentRevision, revision);
}

// --- Delete -------------------------------------------------------------------------

RIVET_TEST(ContentCommands_delete_marks_objects_removed_and_undo_restores) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 1);
    CHECK(f.run(deleteContent(*f.session, f.id(0), {image})));
    CHECK_EQ(editsOf(f).objects.size(), std::size_t{1});
    CHECK(editsOf(f).objects[0].remove);
    CHECK(!editsOf(f).objects[0].transform.has_value());
    CHECK(f.load(0));
    CHECK_EQ(f.content(0)->objects.size(), std::size_t{4});
    CHECK(!f.session->contentService().findObject(f.id(0), image).has_value());
    // The remaining objects keep their ids.
    CHECK(f.session->undo());
    CHECK(f.load(0));
    CHECK_EQ(f.content(0)->objects.size(), std::size_t{5});
    CHECK(f.content(0)->objects[1].id == image);
}

RIVET_TEST(ContentCommands_delete_after_move_drops_the_transform) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 0);
    CHECK(f.run(moveContent(*f.session, f.id(0), {image}, Point{9, 9})));
    CHECK(f.load(0));
    CHECK(f.run(deleteContent(*f.session, f.id(0), {image})));
    const pdf::PdfObjectEdit* edit = objectEditAt(editsOf(f), 0);
    CHECK(edit != nullptr);
    if (edit != nullptr) {
        CHECK(edit->remove);
        CHECK(!edit->transform.has_value());
    }
}

RIVET_TEST(ContentCommands_delete_text_block_removes_all_members) {
    ContentFixture f;
    sample(f);
    CHECK(f.run(deleteContent(*f.session, f.id(0), {f.content(0)->blocks[0].id})));
    CHECK_EQ(editsOf(f).objects.size(), std::size_t{2});
    CHECK(editsOf(f).objects[0].remove);
    CHECK(editsOf(f).objects[1].remove);
    CHECK(f.load(0));
    CHECK(f.content(0)->blocks.empty());
}

RIVET_TEST(ContentCommands_delete_read_only_is_refused) {
    ContentFixture f;
    sample(f);
    CHECK(hasError(deleteContent(*f.session, f.id(0), {f.objectId(0, 4)}), ErrorCode::Unsupported));
    CHECK(hasError(deleteContent(*f.session, f.id(0), {}), ErrorCode::InvalidArgument));
}

// --- Resize / replace image ---------------------------------------------------------

RIVET_TEST(ContentCommands_resize_image_scales_the_display_bounds) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 0);
    const Rect target{Point{120.0, 160.0}, core::Size{50.0, 25.0}};
    CHECK(f.run(resizeContent(*f.session, f.id(0), image, target)));
    CHECK(f.load(0));
    const ContentObjectView resized = f.content(0)->objects[0];
    CHECK(resized.id == image);
    CHECK_NEAR(resized.bounds.minX(), 120.0, 1e-6);
    CHECK_NEAR(resized.bounds.minY(), 160.0, 1e-6);
    CHECK_NEAR(resized.bounds.size.width, 50.0, 1e-6);
    CHECK_NEAR(resized.bounds.size.height, 25.0, 1e-6);
    // A second resize composes with the first.
    const Rect again{Point{10.0, 10.0}, core::Size{100.0, 100.0}};
    CHECK(f.run(resizeContent(*f.session, f.id(0), image, again)));
    CHECK(f.load(0));
    CHECK_NEAR(f.content(0)->objects[0].bounds.size.width, 100.0, 1e-6);
    CHECK_EQ(editsOf(f).objects.size(), std::size_t{1});
}

RIVET_TEST(ContentCommands_resize_accepts_paths_and_refuses_text_and_garbage) {
    ContentFixture f;
    pdf::PdfPageContent content = samplePage();
    content.objects.push_back(makePathObject(300, 300, 340, 320));
    f.setContent(0, std::move(content));
    CHECK(f.load(0));
    const Rect target{Point{5.0, 5.0}, core::Size{30.0, 30.0}};
    CHECK(f.run(resizeContent(*f.session, f.id(0), f.objectId(0, 5), target)));
    CHECK(f.load(0));
    CHECK(hasError(resizeContent(*f.session, f.id(0), f.objectId(0, 2), target), ErrorCode::InvalidArgument));
    CHECK(hasError(resizeContent(*f.session, f.id(0), ObjectId{999999}, target), ErrorCode::NotFound));
    CHECK(hasError(resizeContent(*f.session, f.id(0), f.objectId(0, 4), target), ErrorCode::InvalidArgument));
    const ObjectId image = f.objectId(0, 0);
    CHECK(hasError(resizeContent(*f.session, f.id(0), image, Rect{Point{0, 0}, core::Size{0, 10}}),
                   ErrorCode::InvalidArgument));
    CHECK(hasError(resizeContent(*f.session, f.id(0), image, Rect{Point{0, 0}, core::Size{-5, 10}}),
                   ErrorCode::InvalidArgument));
    CHECK(hasError(resizeContent(*f.session, f.id(0), image, Rect{Point{std::nan(""), 0}, core::Size{5, 10}}),
                   ErrorCode::InvalidArgument));
}

RIVET_TEST(ContentCommands_replace_image_sets_the_replacement_on_the_object_edit) {
    ContentFixture f;
    sample(f);
    const auto image = makeBgraImage(8, 4);
    CHECK(f.run(replaceImage(*f.session, f.id(0), f.objectId(0, 1), image)));
    const pdf::PdfObjectEdit* edit = objectEditAt(editsOf(f), 1);
    CHECK(edit != nullptr);
    if (edit != nullptr) {
        CHECK(edit->replaceImage == image);
        CHECK(!edit->remove);
    }
    CHECK(f.session->undo());
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(ContentCommands_replace_image_failure_paths) {
    ContentFixture f;
    sample(f);
    const ObjectId image = f.objectId(0, 0);
    CHECK(hasError(replaceImage(*f.session, f.id(0), image, nullptr), ErrorCode::InvalidArgument));
    CHECK(hasError(replaceImage(*f.session, f.id(0), f.objectId(0, 2), makeBgraImage()), ErrorCode::InvalidArgument));
    CHECK(hasError(replaceImage(*f.session, f.id(0), ObjectId{999999}, makeBgraImage()), ErrorCode::NotFound));
    // The pixel data must match its dimensions (pdf::validate).
    auto bad = std::make_shared<pdf::PdfImageData>(*makeBgraImage());
    bad->bytes.resize(3);
    CHECK(hasError(replaceImage(*f.session, f.id(0), image, bad), ErrorCode::InvalidArgument));
    auto empty = std::make_shared<pdf::PdfImageData>(*makeBgraImage());
    empty->width = 0;
    CHECK(hasError(replaceImage(*f.session, f.id(0), image, empty), ErrorCode::InvalidArgument));
    CHECK(f.entry(0).contentEdits == nullptr);
}

// --- Text editing -------------------------------------------------------------------

RIVET_TEST(ContentCommands_edit_text_block_replaces_in_place_and_keeps_the_block_id) {
    ContentFixture f;
    sample(f);
    const TextBlockView before = f.content(0)->blocks[0];
    auto edit = editTextBlock(*f.session, f.id(0), before.id, TextBlockPatch{.text = "Hello brave world"});
    CHECK(edit.has_value());
    if (!edit) return;
    CHECK_EQ(edit->ids.size(), std::size_t{1});
    CHECK(edit->ids[0] == before.id);
    CHECK_EQ(std::string(edit->command->name()), std::string("Edit Text"));
    CHECK(f.session->execute(std::move(edit->command)).has_value());

    const auto& edits = editsOf(f);
    CHECK(edits.objects.empty());
    CHECK_EQ(edits.textBlocks.size(), std::size_t{1});
    const pdf::PdfTextBlockEdit& tb = edits.textBlocks[0];
    CHECK_EQ(tb.tag, before.id.value());
    CHECK_EQ(tb.members.size(), std::size_t{2});
    CHECK_EQ(tb.members[0], std::uint32_t{2});
    CHECK_EQ(tb.members[1], std::uint32_t{3});
    CHECK_EQ(tb.text, std::string("Hello brave world"));
    CHECK(tb.font.kind == pdf::PdfFontRef::Kind::FromObject);
    CHECK_EQ(tb.font.sourceIndex, std::uint32_t{2});
    CHECK_NEAR(tb.fontSize, 12.0, 1e-9);
    CHECK_NEAR(tb.wrapWidth, 0.0, 1e-12);
    CHECK_NEAR(tb.lineAdvance, 14.4, 1e-9);
    CHECK_NEAR(tb.placement.tx, 72.0, 1e-9);
    CHECK_NEAR(tb.placement.ty, 700.0, 1e-9);
    CHECK_NEAR(tb.placement.a, 1.0, 1e-12);
    CHECK(tb.color == pdf::PdfColor(0.1F, 0.2F, 0.3F));

    // Identity: the edited block is addressed by the same id as before.
    CHECK(f.load(0));
    const PageContentViewPtr view = f.content(0);
    CHECK_EQ(view->blocks.size(), std::size_t{1});
    CHECK(view->blocks[0].id == before.id);
    CHECK_EQ(view->blocks[0].tag, before.id.value());
    CHECK(view->blocks[0].edited);
    CHECK_EQ(view->blocks[0].text, std::string("Hello brave world"));
    // Objects that were not touched keep their ids.
    CHECK(view->objects[0].id == f.session->contentService().findObject(f.id(0), view->objects[0].id)->id);
}

RIVET_TEST(ContentCommands_editing_an_edited_block_updates_the_same_edit) {
    ContentFixture f;
    sample(f);
    const ObjectId block = f.content(0)->blocks[0].id;
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "One"})));
    CHECK(f.load(0));
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block,
                              TextBlockPatch{.text = "Two", .fontSize = 24.0, .color = pdf::PdfColor{1, 0, 0}})));
    const auto& edits = editsOf(f);
    CHECK_EQ(edits.textBlocks.size(), std::size_t{1});
    const pdf::PdfTextBlockEdit& tb = edits.textBlocks[0];
    CHECK_EQ(tb.tag, block.value());
    CHECK_EQ(tb.members.size(), std::size_t{2});
    CHECK_EQ(tb.text, std::string("Two"));
    CHECK_NEAR(tb.fontSize, 24.0, 1e-12);
    // The line advance follows the size.
    CHECK_NEAR(tb.lineAdvance, 28.8, 1e-9);
    CHECK(tb.color == pdf::PdfColor(1, 0, 0));
    CHECK(tb.font.kind == pdf::PdfFontRef::Kind::FromObject);
    // Undo returns to the first edit, then to the pristine page.
    CHECK(f.session->undo());
    CHECK_EQ(editsOf(f).textBlocks[0].text, std::string("One"));
    CHECK(f.session->undo());
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(ContentCommands_edit_multi_line_block_keeps_wrap_width_and_advance) {
    ContentFixture f;
    f.setContent(0, paragraphPage());
    CHECK(f.load(0));
    const TextBlockView block = f.content(0)->blocks[0];
    CHECK_EQ(block.lines.size(), std::size_t{3});
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block.id, TextBlockPatch{.text = "A\nB\nC\nD"})));
    const pdf::PdfTextBlockEdit& tb = editsOf(f).textBlocks[0];
    CHECK_EQ(tb.members.size(), std::size_t{6});
    CHECK_GT(tb.wrapWidth, 60.0);
    CHECK_NEAR(tb.wrapWidth, block.wrapWidth, 1e-9);
    CHECK_NEAR(tb.lineAdvance, 14.4, 1e-9);
    CHECK_EQ(tb.font.sourceIndex, std::uint32_t{0});
    // The image is not part of the block.
    CHECK(f.load(0));
    CHECK_EQ(f.content(0)->blocks.size(), std::size_t{1});
    CHECK_EQ(f.content(0)->blocks[0].lines.size(), std::size_t{4});
}

RIVET_TEST(ContentCommands_edit_after_move_bakes_the_move_into_the_placement) {
    ContentFixture f;
    sample(f);
    const ObjectId block = f.content(0)->blocks[0].id;
    CHECK(f.run(moveContent(*f.session, f.id(0), {block}, Point{10.0, 30.0})));
    CHECK(f.load(0));
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "Moved"})));
    const auto& edits = editsOf(f);
    // The members are replaced: their transforms are gone.
    CHECK(edits.objects.empty());
    CHECK_EQ(edits.textBlocks.size(), std::size_t{1});
    CHECK_NEAR(edits.textBlocks[0].placement.tx, 82.0, 1e-9);
    CHECK_NEAR(edits.textBlocks[0].placement.ty, 670.0, 1e-9);
}

RIVET_TEST(ContentCommands_moving_an_edited_block_changes_its_placement) {
    ContentFixture f;
    sample(f);
    const ObjectId block = f.content(0)->blocks[0].id;
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "Edited"})));
    CHECK(f.load(0));
    CHECK(f.run(moveContent(*f.session, f.id(0), {block}, Point{5.0, -10.0})));
    const auto& edits = editsOf(f);
    CHECK(edits.objects.empty());
    CHECK_EQ(edits.textBlocks.size(), std::size_t{1});
    CHECK_NEAR(edits.textBlocks[0].placement.tx, 77.0, 1e-9);
    CHECK_NEAR(edits.textBlocks[0].placement.ty, 710.0, 1e-9);
    // Block identity holds through the move.
    CHECK(f.load(0));
    CHECK(f.content(0)->blocks[0].id == block);
}

RIVET_TEST(ContentCommands_delete_edited_block_removes_the_edit_and_the_replaced_members) {
    ContentFixture f;
    sample(f);
    const ObjectId block = f.content(0)->blocks[0].id;
    CHECK(f.run(editTextBlock(*f.session, f.id(0), block, TextBlockPatch{.text = "Edited"})));
    CHECK(f.load(0));
    CHECK(f.run(deleteContent(*f.session, f.id(0), {block})));
    const auto& edits = editsOf(f);
    CHECK(edits.textBlocks.empty());
    CHECK_EQ(edits.objects.size(), std::size_t{2});
    CHECK(edits.objects[0].remove);
    CHECK(edits.objects[1].remove);
    CHECK(f.load(0));
    CHECK(f.content(0)->blocks.empty());
}

RIVET_TEST(ContentCommands_emptying_a_block_deletes_it) {
    ContentFixture f;
    sample(f);
    auto edit = editTextBlock(*f.session, f.id(0), f.content(0)->blocks[0].id, TextBlockPatch{.text = ""});
    CHECK(edit.has_value());
    if (!edit) return;
    CHECK_EQ(std::string(edit->command->name()), std::string("Delete"));
}

RIVET_TEST(ContentCommands_edit_text_failure_paths) {
    ContentFixture f;
    pdf::PdfPageContent content = samplePage();
    // A subset-font line below: a bundled substitute is needed for new glyphs.
    addTextLine(content, 400.0);
    for (std::size_t i = 5; i < 7; ++i) content.objects[i].font.subset = true;
    // Invisible text (OCR layer): read-only.
    pdf::PdfContentObject ocr = makeTextObject(72, 200, 12, "hidden", 40);
    ocr.renderMode = 3;
    content.objects.push_back(ocr);
    // Type3 text: move only.
    pdf::PdfContentObject type3 = makeTextObject(72, 100, 12, "glyphs", 40);
    type3.font.type3 = true;
    content.objects.push_back(type3);
    f.setContent(0, std::move(content));
    CHECK(f.load(0));
    const PageContentViewPtr view = f.content(0);
    CHECK_EQ(view->blocks.size(), std::size_t{4});
    const ObjectId plain = view->blocks[0].id;
    const ObjectId subset = view->blocks[1].id;
    const ObjectId hidden = view->blocks[2].id;
    const ObjectId glyphs = view->blocks[3].id;

    CHECK(hasError(editTextBlock(*f.session, f.id(0), plain, TextBlockPatch{}), ErrorCode::InvalidArgument));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), ObjectId{999999}, TextBlockPatch{.text = "x"}),
                   ErrorCode::NotFound));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), hidden, TextBlockPatch{.text = "x"}), ErrorCode::Unsupported));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), glyphs, TextBlockPatch{.text = "x"}), ErrorCode::Unsupported));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), plain, TextBlockPatch{.fontSize = 0.5}),
                   ErrorCode::InvalidArgument));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), plain, TextBlockPatch{.fontSize = 900.0}),
                   ErrorCode::InvalidArgument));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), plain, TextBlockPatch{.wrapWidth = -1.0}),
                   ErrorCode::InvalidArgument));
    CHECK(hasError(editTextBlock(*f.session, f.id(0), plain,
                                 TextBlockPatch{.text = std::string(pdf::kMaxTextBlockBytes + 1, 'a')}),
                   ErrorCode::InvalidArgument));
    // Existing text keeps its font family.
    CHECK(hasError(editTextBlock(*f.session, f.id(0), plain, TextBlockPatch{.font = pdf::PdfBundledFont::MonoBold}),
                   ErrorCode::Unsupported));
    // Glyphs no bundled face has, written through a subset font: refused.
    CHECK(hasError(editTextBlock(*f.session, f.id(0), subset, TextBlockPatch{.text = "\xE6\x97\xA5\xE6\x9C\xAC"}),
                   ErrorCode::InvalidArgument));
    // The same text in a full embedded font is left to the backend.
    CHECK(editTextBlock(*f.session, f.id(0), plain, TextBlockPatch{.text = "\xE6\x97\xA5\xE6\x9C\xAC"}).has_value());
    // Ordinary Latin / Cyrillic text through the subset font is fine.
    CHECK(editTextBlock(*f.session, f.id(0), subset, TextBlockPatch{.text = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"})
              .has_value());
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(ContentCommands_edit_rotated_text_keeps_the_rotation_in_the_placement) {
    ContentFixture f;
    pdf::PdfPageContent content;
    const double angle = 0.5;
    content.objects.push_back(makeTextObject(200, 300, 12, "Rotated", 50, angle));
    f.setContent(0, std::move(content));
    CHECK(f.load(0));
    CHECK(f.run(editTextBlock(*f.session, f.id(0), f.objectId(0, 0), TextBlockPatch{.text = "Turned"})));
    const pdf::PdfTextBlockEdit& tb = editsOf(f).textBlocks.at(0);
    CHECK_NEAR(tb.placement.a, std::cos(angle), 1e-9);
    CHECK_NEAR(tb.placement.b, std::sin(angle), 1e-9);
    CHECK_NEAR(tb.placement.tx, 200.0, 1e-9);
    CHECK_NEAR(tb.placement.ty, 300.0, 1e-9);
}

// --- Add text -----------------------------------------------------------------------

RIVET_TEST(ContentCommands_add_text_block_creates_a_bundled_block_with_a_minted_tag) {
    ContentFixture f;
    sample(f);
    NewTextBlock block;
    block.text = "New text";
    block.font = pdf::PdfBundledFont::SerifBold;
    block.fontSize = 18.0;
    block.color = pdf::PdfColor{0, 0.5F, 0};
    block.displayOrigin = Point{100.0, 200.0};
    block.displayWrapWidth = 120.0;
    auto edit = addTextBlock(*f.session, f.id(0), block);
    CHECK(edit.has_value());
    if (!edit) return;
    CHECK_EQ(edit->ids.size(), std::size_t{1});
    const ObjectId id = edit->ids[0];
    CHECK(id.value() != 0);
    CHECK_EQ(std::string(edit->command->name()), std::string("Add Text"));
    CHECK(f.session->execute(std::move(edit->command)).has_value());

    const auto& edits = editsOf(f);
    CHECK_EQ(edits.textBlocks.size(), std::size_t{1});
    const pdf::PdfTextBlockEdit& tb = edits.textBlocks[0];
    CHECK_EQ(tb.tag, id.value());
    CHECK(tb.members.empty());
    CHECK(tb.font.kind == pdf::PdfFontRef::Kind::Bundled);
    CHECK(tb.font.fallback == pdf::PdfBundledFont::SerifBold);
    CHECK_NEAR(tb.fontSize, 18.0, 1e-12);
    CHECK_NEAR(tb.wrapWidth, 120.0, 1e-12);
    CHECK_NEAR(tb.lineAdvance, 21.6, 1e-9);
    // Upright page: user y = 792 - display y.
    CHECK_NEAR(tb.placement.tx, 100.0, 1e-9);
    CHECK_NEAR(tb.placement.ty, 592.0, 1e-9);

    // The created block is addressed by the returned id and stays so across
    // undo and redo.
    CHECK(f.load(0));
    CHECK(f.content(0)->blocks.size() == 2);
    const auto found = f.session->contentService().findBlock(f.id(0), id);
    CHECK(found.has_value());
    if (found) {
        CHECK_EQ(found->text, std::string("New text"));
        CHECK(found->edited);
        CHECK_EQ(found->tag, id.value());
        CHECK_NEAR(found->bounds.minX(), 100.0, 1e-6);
    }
    CHECK(f.session->undo());
    CHECK(f.session->redo());
    CHECK(f.load(0));
    CHECK(f.session->contentService().findBlock(f.id(0), id).has_value());
}

RIVET_TEST(ContentCommands_add_text_reads_upright_on_a_rotated_page) {
    ContentFixture f;
    sample(f);
    CHECK(f.session
              ->execute(std::make_unique<RotatePagesCommand>(f.session->pageModel(), std::vector<PageId>{f.id(0)}, 90))
              .has_value());
    CHECK(f.load(0));
    NewTextBlock block;
    block.text = "Sideways";
    block.displayOrigin = Point{150.0, 250.0};
    auto edit = addTextBlock(*f.session, f.id(0), block);
    CHECK(edit.has_value());
    if (!edit) return;
    CHECK(f.session->execute(std::move(edit->command)).has_value());
    const pdf::PdfPageContentEdits& edits = editsOf(f);
    CHECK_EQ(edits.textBlocks.size(), std::size_t{1});

    // The frame (x right, y up) displays as x right, y up on screen.
    const core::Matrix v = pdf::userToDisplayMatrix(f.entry(0).view);
    const core::Matrix frameToDisplay = v * edits.textBlocks[0].placement;
    const Point origin = frameToDisplay.map(Point{0, 0});
    const Point along = frameToDisplay.map(Point{1, 0});
    const Point up = frameToDisplay.map(Point{0, 1});
    CHECK_NEAR(origin.x, 150.0, 1e-9);
    CHECK_NEAR(origin.y, 250.0, 1e-9);
    CHECK_NEAR(along.x - origin.x, 1.0, 1e-9);
    CHECK_NEAR(along.y - origin.y, 0.0, 1e-9);
    CHECK_NEAR(up.x - origin.x, 0.0, 1e-9);
    CHECK_NEAR(up.y - origin.y, -1.0, 1e-9);
    // The placement itself is a rotation of the page (not upright in user space).
    CHECK_NEAR(std::abs(edits.textBlocks[0].placement.b), 1.0, 1e-9);

    // The resolved block reads horizontally in display space.
    CHECK(f.load(0));
    const auto found = f.session->contentService().findBlock(f.id(0), edit->ids[0]);
    CHECK(found.has_value());
    if (found) CHECK_NEAR(found->rotationDegrees, 0.0, 1e-6);
}

RIVET_TEST(ContentCommands_add_text_failure_paths) {
    ContentFixture f;
    sample(f);
    const auto build = [&](auto mutate) {
        NewTextBlock block;
        block.text = "ok";
        mutate(block);
        return addTextBlock(*f.session, f.id(0), block);
    };
    CHECK(build([](NewTextBlock&) {}).has_value());
    CHECK(hasError(build([](NewTextBlock& b) { b.text.clear(); }), ErrorCode::InvalidArgument));
    CHECK(hasError(build([](NewTextBlock& b) { b.fontSize = 0.0; }), ErrorCode::InvalidArgument));
    CHECK(hasError(build([](NewTextBlock& b) { b.fontSize = 1000.0; }), ErrorCode::InvalidArgument));
    CHECK(hasError(build([](NewTextBlock& b) { b.fontSize = std::nan(""); }), ErrorCode::InvalidArgument));
    CHECK(hasError(build([](NewTextBlock& b) { b.displayOrigin = Point{std::nan(""), 0}; }), ErrorCode::InvalidArgument));
    CHECK(hasError(build([](NewTextBlock& b) { b.displayWrapWidth = -1.0; }), ErrorCode::InvalidArgument));
    CHECK(hasError(build([](NewTextBlock& b) { b.text = "\xE6\x97\xA5"; }), ErrorCode::InvalidArgument));
    CHECK(hasError(build([](NewTextBlock& b) { b.text.assign(pdf::kMaxTextBlockBytes + 1, 'a'); }),
                   ErrorCode::InvalidArgument));
    CHECK(hasError(addTextBlock(*f.session, PageId{424242}, newText("x")), ErrorCode::NotFound));
    CHECK(f.entry(0).contentEdits == nullptr);
}

RIVET_TEST(ContentCommands_add_text_is_refused_on_pages_that_cannot_be_regenerated) {
    ContentFixture f;
    pdf::PdfPageContent content = samplePage();
    content.regenerationSafe = false;
    content.regenerationIssue = "shading would be lost";
    f.setContent(0, std::move(content));
    CHECK(f.load(0));
    auto refused = addTextBlock(*f.session, f.id(0), newText("x"));
    CHECK(hasError(refused, ErrorCode::Unsupported));
    if (!refused) CHECK_EQ(refused.error().message, std::string("shading would be lost"));
    CHECK(hasError(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{1, 1}), ErrorCode::Unsupported));

    pdf::PdfPageContent truncated;
    truncated.truncated = true;
    f.setContent(1, std::move(truncated));
    CHECK(f.load(1));
    CHECK(hasError(addTextBlock(*f.session, f.id(1), newText("x")), ErrorCode::Unsupported));
}

RIVET_TEST(ContentCommands_each_added_block_gets_its_own_id) {
    ContentFixture f;
    sample(f);
    std::set<ObjectId> seen;
    for (int i = 0; i < 3; ++i) {
        auto edit = addTextBlock(*f.session, f.id(0), newText("text"));
        CHECK(edit.has_value());
        if (!edit) return;
        CHECK(seen.insert(edit->ids[0]).second);
        CHECK(f.session->execute(std::move(edit->command)).has_value());
        CHECK(f.load(0));
    }
    CHECK_EQ(editsOf(f).textBlocks.size(), std::size_t{3});
    // Ids of the page's source objects never collide with the tags.
    for (const ContentObjectView& object : f.content(0)->objects) {
        if (object.source.origin.kind == pdf::PdfContentOrigin::Kind::Source) CHECK(seen.count(object.id) == 0);
    }
}

// --- Z-order --------------------------------------------------------------------------

RIVET_TEST(ContentCommands_bring_to_front_reorders_created_blocks_only) {
    ContentFixture f;
    sample(f);
    std::vector<ObjectId> ids;
    for (const char* text : {"first", "second", "third"}) {
        auto edit = addTextBlock(*f.session, f.id(0), newText(text));
        CHECK(edit.has_value());
        if (!edit) return;
        ids.push_back(edit->ids[0]);
        CHECK(f.session->execute(std::move(edit->command)).has_value());
        CHECK(f.load(0));
    }
    CHECK(f.run(bringToFront(*f.session, f.id(0), ids[0])));
    const auto& blocks = editsOf(f).textBlocks;
    CHECK_EQ(blocks.size(), std::size_t{3});
    CHECK_EQ(blocks[0].tag, ids[1].value());
    CHECK_EQ(blocks[1].tag, ids[2].value());
    CHECK_EQ(blocks[2].tag, ids[0].value());
    CHECK(f.session->undo());
    CHECK_EQ(editsOf(f).textBlocks[0].tag, ids[0].value());

    CHECK(f.load(0));
    // Already in front, a source object and an unknown id are refused.
    CHECK(hasError(bringToFront(*f.session, f.id(0), ids[2]), ErrorCode::NotAvailable));
    CHECK(hasError(bringToFront(*f.session, f.id(0), f.objectId(0, 0)), ErrorCode::NotAvailable));
    CHECK(hasError(bringToFront(*f.session, f.id(0), ObjectId{999999}), ErrorCode::NotFound));
}

// --- uncoveredCodepoints ------------------------------------------------------------

RIVET_TEST(ContentCommands_uncovered_codepoints_lists_each_missing_character_once) {
    CHECK(uncoveredCodepoints(pdf::PdfBundledFont::SansRegular, "Hello, world\n\tcaf\xC3\xA9").empty());
    CHECK(uncoveredCodepoints(pdf::PdfBundledFont::SerifBold, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82").empty());
    const std::u32string missing =
        uncoveredCodepoints(pdf::PdfBundledFont::MonoRegular, "a\xE6\x97\xA5" "b\xE6\x9C\xAC\xE6\x97\xA5");
    CHECK_EQ(missing.size(), std::size_t{2});
    CHECK(missing[0] == U'日');
    CHECK(missing[1] == U'本');
    CHECK(uncoveredCodepoints(pdf::PdfBundledFont::SansRegular, "").empty());
}

// --- Dirty state / saving ------------------------------------------------------------

RIVET_TEST(ContentCommands_dirty_state_follows_the_command_stack) {
    ContentFixture f;
    sample(f);
    CHECK(!f.session->isDirty());
    CHECK(f.run(moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{1, 1})));
    CHECK(f.session->isDirty());
    f.session->markSaved();
    CHECK(!f.session->isDirty());
    CHECK(f.session->undo());
    CHECK(f.session->isDirty());
    CHECK(f.session->redo());
    CHECK(!f.session->isDirty());
}

RIVET_TEST(ContentCommands_a_content_command_for_a_removed_page_fails_cleanly) {
    ContentFixture f;
    sample(f);
    auto edit = moveContent(*f.session, f.id(0), {f.objectId(0, 0)}, Point{1, 1});
    CHECK(edit.has_value());
    if (!edit) return;
    CHECK(f.session->execute(std::make_unique<DeletePagesCommand>(f.session->pageModel(), std::vector<PageId>{f.id(0)}))
              .has_value());
    CHECK(!f.session->execute(std::move(edit->command)).has_value());
}
