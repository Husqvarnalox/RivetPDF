// SPDX-License-Identifier: MPL-2.0
// ContentController over a real DocumentSession (fake page backend) and a FAKE
// ContentBackend: tool switching against the annotation tools, intents ->
// commands and undo, nudge coalescing, the inline editor, Add Text, restyle,
// Replace Image and the menu commands.
#include "RivetTest.h"

#include "ContentTestKit.hpp"

#include "ui/TextArea.hpp"

#include <string>

using namespace rivet;
using app::ContentCommand;
using app::ContentController;
using app::ContentInteraction;
using app::ContentTool;
using core::ObjectId;
using core::Point;
using core::Rect;
using editor::ContentCapability;
using test::content::ContentShell;
using IntentKind = ContentInteraction::Intent::Kind;

namespace {

bool contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

const Rect kImage{100.0, 100.0, 200.0, 100.0};
const Rect kBlock{50.0, 300.0, 200.0, 20.0};
const Point kInImage{150.0, 150.0};
const Point kInBlock{100.0, 310.0};

// A shell with a Ready tab, an image (id 1) and an editable block (id 2).
struct Fixture {
    ContentShell shell;
    ContentController& c;
    test::content::FakeState& fake;
    Fixture() : c(*shell.content), fake(*shell.fake) {
        CHECK(shell.open("ctl") != nullptr);
        shell.addImage(1, kImage);
        shell.addBlock(2, kBlock, "Hello");
        c.setTool(ContentTool::SelectObject);
    }
    test::content::FakeObject& image() { return shell.page().objects.front(); }
    test::content::FakeBlock& block() { return shell.page().blocks.front(); }
    ContentInteraction::Intent intent(IntentKind kind) {
        ContentInteraction::Intent i;
        i.kind = kind;
        i.page = 0;
        return i;
    }
};

} // namespace

RIVET_TEST(contentControllerToolSwitchingSharesOneStateWithAnnotations) {
    ContentShell shell;
    CHECK(shell.open("tools") != nullptr);
    shell.addImage(1, kImage);
    ContentController& c = *shell.content;
    CHECK(c.tool() == ContentTool::None);
    CHECK(!shell.annotations->suspended());

    c.setTool(ContentTool::SelectObject);
    CHECK(shell.annotations->suspended());
    CHECK(shell.annotations->tool() == app::AnnotationTool::Select);
    shell.click(kInImage);
    CHECK(c.selected().has_value());
    c.setTool(ContentTool::AddText); // resets the selection
    CHECK(!c.selected().has_value());
    CHECK(shell.annotations->suspended());

    // Choosing an annotation tool returns the content tool to None.
    shell.annotations->setTool(app::AnnotationTool::Rectangle);
    CHECK(c.tool() == ContentTool::None);
    CHECK(!shell.annotations->suspended());

    // Binding a tab resets the tool.
    c.setTool(ContentTool::SelectObject);
    c.bindTab(shell.workspace.activeTab());
    CHECK(c.tool() == ContentTool::None);
    CHECK(!shell.annotations->suspended());
}

RIVET_TEST(contentControllerSelectionAnnouncesCapabilityAndReason) {
    Fixture f;
    f.image().capability = ContentCapability::ReadOnly;
    f.image().reason = "scanned image mask";
    f.shell.click(kInImage);
    CHECK(f.c.selected().has_value());
    CHECK(contains(f.shell.lastStatus(), "read-only"));
    CHECK(contains(f.shell.lastStatus(), "scanned image mask"));
    CHECK_GT(f.fake.hitTestCalls, 0);
    CHECK_NEAR(f.fake.lastTolerance, app::ContentInteraction::kHitTolerance / f.shell.viewport->zoomFactor(), 1e-9);

    f.image().capability = ContentCapability::MoveOnly;
    f.image().reason.clear();
    f.shell.click(kInImage);
    CHECK(contains(f.shell.lastStatus(), "moved or deleted"));

    f.block().capability = ContentCapability::Replaceable;
    f.shell.click(kInBlock);
    CHECK(contains(f.shell.lastStatus(), "retyped"));
    CHECK(f.c.selectionIsText());
    CHECK(!f.c.selectionIsImage());
}

RIVET_TEST(contentControllerDragMoveIsOneCommandAndOneUndoStep) {
    Fixture f;
    f.shell.click(kInImage);
    f.shell.drag(kInImage, Point{170.0, 160.0});
    CHECK_EQ(f.fake.moveCalls, 1);
    CHECK_NEAR(f.fake.lastMoveDelta.x, 20.0, 1e-6);
    CHECK_NEAR(f.fake.lastMoveDelta.y, 10.0, 1e-6);
    CHECK_NEAR(f.image().bounds.minX(), 120.0, 1e-6);
    CHECK(f.shell.session().undo());
    CHECK_NEAR(f.image().bounds.minX(), 100.0, 1e-6);
    CHECK_NEAR(f.image().bounds.minY(), 100.0, 1e-6);
    CHECK(!f.shell.session().undo()); // exactly one step
}

RIVET_TEST(contentControllerResizeAndDeleteAreUndoable) {
    Fixture f;
    f.shell.click(kInImage);
    // Bottom-right handle, free resize with Shift (images keep the ratio otherwise).
    f.shell.mouse(ui::PointerEventType::Down, Point{300.0, 200.0});
    f.shell.mouse(ui::PointerEventType::Move, Point{400.0, 220.0}, true);
    f.shell.mouse(ui::PointerEventType::Up, Point{400.0, 220.0}, true);
    CHECK_EQ(f.fake.resizeCalls, 1);
    CHECK_NEAR(f.image().bounds.size.width, 300.0, 1e-6);
    CHECK_NEAR(f.image().bounds.size.height, 120.0, 1e-6);
    CHECK(f.shell.session().undo());
    CHECK_NEAR(f.image().bounds.size.width, 200.0, 1e-6);

    // Delete key through the viewport.
    CHECK(f.shell.key(ui::Key::Delete));
    CHECK_EQ(f.fake.deleteCalls, 1);
    CHECK(f.shell.page().objects.empty());
    CHECK(!f.c.selected().has_value());
    CHECK(f.shell.session().undo());
    CHECK_EQ(f.shell.page().objects.size(), std::size_t{1});
}

RIVET_TEST(contentControllerEscapeClearsSelectionThenReportsNothingToDo) {
    Fixture f;
    CHECK(!f.c.handleEscape());
    f.shell.click(kInImage);
    CHECK(f.c.handleEscape());
    CHECK(!f.c.selected().has_value());
    CHECK(!f.c.handleEscape());
    f.c.setTool(ContentTool::None);
    CHECK(!f.c.handleEscape());
}

RIVET_TEST(contentControllerEditingLockRefusesEditsWithAMessage) {
    Fixture f;
    f.shell.click(kInImage);
    const std::uint64_t before = f.shell.session().commands().stateId();
    f.shell.session().setEditingLocked(true, "saving");
    f.shell.drag(kInImage, Point{170.0, 160.0});
    CHECK_EQ(f.shell.session().commands().stateId(), before);
    CHECK_NEAR(f.image().bounds.minX(), 100.0, 1e-6);
    CHECK(contains(f.shell.lastStatus(), "save is in progress"));
    f.shell.statusLog.clear();
    f.shell.key(ui::Key::Right);
    CHECK(contains(f.shell.lastStatus(), "save is in progress"));
    f.c.deleteSelected();
    CHECK_EQ(f.shell.session().commands().stateId(), before);
    f.shell.session().setEditingLocked(false);
}

RIVET_TEST(contentControllerFailedFactoryReportsAnEditStatus) {
    Fixture f;
    f.shell.click(kInImage);
    f.fake.failNext = core::makeError(core::ErrorCode::InvalidArgument, "boom", "test");
    f.c.deleteSelected();
    CHECK(contains(f.shell.lastStatus(), "Edit:"));
    CHECK_EQ(f.shell.page().objects.size(), std::size_t{1});
}

RIVET_TEST(contentControllerNudgeCoalescesWithinHalfASecond) {
    Fixture f;
    f.shell.click(kInImage);
    f.shell.now = 10.0;
    CHECK(f.shell.key(ui::Key::Right));
    f.shell.now = 10.3;
    CHECK(f.shell.key(ui::Key::Right));
    CHECK_EQ(f.fake.moveCalls, 2);
    CHECK_NEAR(f.fake.lastMoveDelta.x, 2.0, 1e-9); // the combined move
    CHECK_NEAR(f.image().bounds.minX(), 102.0, 1e-9);
    CHECK(f.shell.session().undo()); // ONE step undoes both presses
    CHECK_NEAR(f.image().bounds.minX(), 100.0, 1e-9);
    CHECK(f.shell.session().redo());

    // After more than 500 ms: a second step.
    f.shell.now = 11.0;
    CHECK(f.shell.key(ui::Key::Right));
    CHECK_NEAR(f.image().bounds.minX(), 103.0, 1e-9);
    // A different key: a third step.
    f.shell.now = 11.1;
    CHECK(f.shell.key(ui::Key::Down));
    CHECK_NEAR(f.image().bounds.minY(), 101.0, 1e-9);
    CHECK(f.shell.session().undo());
    CHECK_NEAR(f.image().bounds.minY(), 100.0, 1e-9);
    CHECK_NEAR(f.image().bounds.minX(), 103.0, 1e-9);
    CHECK(f.shell.session().undo());
    CHECK_NEAR(f.image().bounds.minX(), 102.0, 1e-9);
    CHECK(f.shell.session().undo());
    CHECK_NEAR(f.image().bounds.minX(), 100.0, 1e-9);

    // Shift nudges 10.
    f.shell.now = 20.0;
    CHECK(f.shell.key(ui::Key::Left, true));
    CHECK_NEAR(f.image().bounds.minX(), 90.0, 1e-9);
}

RIVET_TEST(contentControllerInlineEditorCommitsOneEditAndCancelsWithoutOne) {
    Fixture f;
    f.shell.click(kInBlock);
    f.c.editSelectedText();
    CHECK(f.c.editorOpen());
    CHECK_EQ(f.c.editorArea().text(), std::string("Hello"));

    // Unchanged text: no command.
    CHECK(f.c.commitEditor());
    CHECK(!f.c.editorOpen());
    CHECK_EQ(f.fake.editCalls, 0);

    // Esc / cancel: no command.
    f.c.editSelectedText();
    f.c.editorArea().setText("Changed");
    CHECK(f.c.handleEscape());
    CHECK(!f.c.editorOpen());
    CHECK_EQ(f.fake.editCalls, 0);
    CHECK_EQ(f.block().text, std::string("Hello"));

    // Changed text: exactly one edit, one undo step.
    f.c.editSelectedText();
    f.c.editorArea().setText("Hello world");
    CHECK(f.c.commitEditor());
    CHECK_EQ(f.fake.editCalls, 1);
    CHECK(f.fake.lastPatch.text.has_value());
    CHECK_EQ(*f.fake.lastPatch.text, std::string("Hello world"));
    CHECK_EQ(f.block().text, std::string("Hello world"));
    CHECK(!f.c.editorOpen());
    CHECK(f.shell.session().undo());
    CHECK_EQ(f.block().text, std::string("Hello"));

    // The pending-edits hook commits an open editor.
    f.c.editSelectedText();
    f.c.editorArea().setText("Pending");
    CHECK(f.shell.context->pendingEdits.commitAll());
    CHECK_EQ(f.block().text, std::string("Pending"));
    CHECK(!f.c.editorOpen());
}

RIVET_TEST(contentControllerEditorRefusesCharactersTheBundledFontCannotWrite) {
    Fixture f;
    f.block().capability = ContentCapability::Replaceable; // retyped in a bundled font
    f.shell.click(kInBlock);
    f.c.editSelectedText();
    CHECK(f.c.editorOpen());
    f.c.editorArea().setText("Hello \xE4\xB8\xAD");
    f.fake.uncovered = std::u32string(1, static_cast<char32_t>(0x4E2D));
    CHECK(!f.c.commitEditor());
    CHECK(f.c.editorOpen());
    CHECK_EQ(f.fake.editCalls, 0);
    CHECK(contains(f.shell.lastStatus(), "U+4E2D"));
    f.fake.uncovered.clear();
    CHECK(f.c.commitEditor());
    CHECK_EQ(f.fake.editCalls, 1);
}

RIVET_TEST(contentControllerEditingLockKeepsTheEditorOpen) {
    Fixture f;
    f.shell.click(kInBlock);
    f.c.editSelectedText();
    f.c.editorArea().setText("Locked edit");
    f.shell.session().setEditingLocked(true, "saving");
    CHECK(!f.c.commitEditor());
    CHECK(f.c.editorOpen());
    CHECK(contains(f.shell.lastStatus(), "save is in progress"));
    f.shell.session().setEditingLocked(false);
    CHECK(f.c.commitEditor());
    CHECK_EQ(f.fake.editCalls, 1);
}

RIVET_TEST(contentControllerAddTextCreatesOneBlockWithTheDefaultStyle) {
    Fixture f;
    f.c.setTool(ContentTool::AddText);
    auto click = f.intent(IntentKind::AddTextClick);
    click.point = Point{100.0, 100.0};
    f.c.applyIntent(click, false);
    CHECK(f.c.editorOpen());
    f.c.editorArea().setText("Hi");
    CHECK(f.c.commitEditor());
    CHECK_EQ(f.fake.addCalls, 1);
    CHECK_EQ(f.fake.lastNew.text, std::string("Hi"));
    CHECK(f.fake.lastNew.font == pdf::PdfBundledFont::SansRegular);
    CHECK_NEAR(f.fake.lastNew.fontSize, 12.0, 1e-9);
    CHECK(f.fake.lastNew.color == (pdf::PdfColor{0.0F, 0.0F, 0.0F}));
    CHECK_NEAR(f.fake.lastNew.displayWrapWidth, 0.0, 1e-9);
    CHECK_EQ(f.shell.page().blocks.size(), std::size_t{2});
    CHECK(f.c.selected().has_value()); // the new block is selected
    CHECK(f.shell.session().undo());
    CHECK_EQ(f.shell.page().blocks.size(), std::size_t{1});

    // Empty text creates nothing.
    f.c.applyIntent(click, false);
    CHECK(f.c.editorOpen());
    CHECK(f.c.commitEditor());
    CHECK(!f.c.editorOpen());
    CHECK_EQ(f.fake.addCalls, 1);

    // A dragged box passes its width as the wrap width.
    auto box = f.intent(IntentKind::AddTextBox);
    box.rect = Rect{100.0, 200.0, 150.0, 40.0};
    f.c.applyIntent(box, false);
    f.c.editorArea().setText("Boxed");
    CHECK(f.c.commitEditor());
    CHECK_EQ(f.fake.addCalls, 2);
    CHECK_NEAR(f.fake.lastNew.displayWrapWidth, 150.0, 1e-9);
}

RIVET_TEST(contentControllerAddTextByClickingThePage) {
    Fixture f;
    f.c.setTool(ContentTool::AddText);
    f.shell.click(Point{300.0, 500.0});
    CHECK(f.c.editorOpen());
    f.c.cancelEditor();
    CHECK(!f.c.editorOpen());
    CHECK_EQ(f.fake.addCalls, 0);
}

RIVET_TEST(contentControllerRestyleSelectedBlockOrTheAddTextDefault) {
    Fixture f;
    f.shell.click(kInBlock);
    f.c.setFontSize(18.0);
    CHECK_EQ(f.fake.editCalls, 1);
    CHECK(f.fake.lastPatch.fontSize.has_value());
    CHECK_NEAR(*f.fake.lastPatch.fontSize, 18.0, 1e-9);
    CHECK(!f.fake.lastPatch.text.has_value());
    CHECK_NEAR(f.block().fontSize, 18.0, 1e-9);

    f.c.setTextColor(pdf::PdfColor{0.85F, 0.15F, 0.15F});
    CHECK_EQ(f.fake.editCalls, 2);
    CHECK(f.fake.lastPatch.color.has_value());

    // The font of a block Rivet did not write cannot change: the backend is not called.
    f.c.setFontFamily(ContentController::FontFamily::Serif);
    CHECK_EQ(f.fake.editCalls, 2);
    CHECK(contains(f.shell.lastStatus(), "font"));
    // A Rivet block can.
    f.block().tag = 99;
    f.c.setFontFamily(ContentController::FontFamily::Serif);
    CHECK_EQ(f.fake.editCalls, 3);
    CHECK(f.fake.lastPatch.font.has_value());
    CHECK(*f.fake.lastPatch.font == pdf::PdfBundledFont::SerifRegular);

    // Nothing selected under Add Text: the default changes, the backend is not touched.
    f.c.setTool(ContentTool::AddText);
    f.c.setFontSize(20.0);
    f.c.setFontFamily(ContentController::FontFamily::Mono);
    CHECK_EQ(f.fake.editCalls, 3);
    CHECK_NEAR(f.c.displayedStyle().size, 20.0, 1e-9);
    CHECK(f.c.displayedStyle().family == ContentController::FontFamily::Mono);
}

RIVET_TEST(contentControllerReplaceImageCommitsOnceAndFailuresAreReported) {
    Fixture f;
    f.shell.click(kInImage);
    f.shell.dialog.image = test::content::tempFile("replace", ".png", "not-an-image");

    f.c.replaceSelectedImage();
    CHECK(f.shell.dispatcher.waitUntil([&] { return f.fake.replaceCalls == 1; }));
    CHECK(f.fake.lastImage != nullptr);
    CHECK_EQ(f.fake.lastImage->width, 20u);
    CHECK_EQ(f.image().pixelWidth, 20u);
    CHECK_EQ(f.c.selectedImagePixels().first, 20u);
    CHECK(f.shell.session().undo());
    CHECK_EQ(f.image().pixelWidth, 640u);
}

RIVET_TEST(contentControllerReplaceImageOversizeAndCancelledDialog) {
    Fixture f;
    f.shell.click(kInImage);

    // Cancelled dialog: silent, nothing decoded.
    const std::size_t statuses = f.shell.statusLog.size();
    f.c.replaceSelectedImage();
    CHECK_EQ(f.shell.dialog.openImageCalls, 1);
    CHECK_EQ(f.shell.statusLog.size(), statuses);
    CHECK_EQ(f.shell.decoder.calls.load(), 0);

    // Decoder refusal (oversize): no command, an "Edit:" status.
    f.shell.dialog.image = test::content::tempFile("oversize", ".png", "x");
    f.shell.decoder.result = std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "too large", "test"));
    f.c.replaceSelectedImage();
    CHECK(f.shell.dispatcher.waitUntil([&] { return contains(f.shell.lastStatus(), "Edit:"); }));
    CHECK_EQ(f.fake.replaceCalls, 0);

    // A non-image selection refuses up front.
    f.shell.click(kInBlock);
    f.c.replaceSelectedImage();
    CHECK(contains(f.shell.lastStatus(), "select an image"));
}

RIVET_TEST(contentControllerReplaceImageDeliversInlineWithoutADispatcher) {
    Fixture f;
    f.shell.click(kInImage);
    f.shell.dialog.image = test::content::tempFile("inline", ".png", "x");
    f.shell.services.mainDispatcher = nullptr;
    f.c.replaceSelectedImage();
    // The command runs on the scheduler worker; poll the atomic counter only.
    CHECK(f.shell.dispatcher.waitUntil([&] { return f.fake.executed.load() >= 1; }));
    CHECK_EQ(f.fake.replaceCalls, 1);
    CHECK_EQ(f.image().pixelWidth, 20u);
    f.shell.content.reset(); // waits for the worker to finish its status updates
}

RIVET_TEST(contentControllerMenuCommandsFollowTheSelection) {
    Fixture f;
    CHECK(f.c.canPerform(ContentCommand::ToolEdit));
    CHECK(f.c.canPerform(ContentCommand::ToolAddText));
    CHECK(!f.c.canPerform(ContentCommand::DeleteObject)); // nothing selected

    f.shell.click(kInImage);
    CHECK(f.c.canPerform(ContentCommand::ReplaceImage));
    CHECK(f.c.canPerform(ContentCommand::DeleteObject));
    CHECK(!f.c.canPerform(ContentCommand::EditText));
    CHECK(!f.c.canPerform(ContentCommand::BringToFront));

    f.shell.click(kInBlock);
    CHECK(f.c.canPerform(ContentCommand::EditText));
    CHECK(!f.c.canPerform(ContentCommand::ReplaceImage));
    CHECK(!f.c.canPerform(ContentCommand::BringToFront)); // not a Rivet block (tag 0)
    f.block().tag = 5;
    CHECK(f.c.canPerform(ContentCommand::BringToFront));
    f.c.perform(ContentCommand::BringToFront);
    CHECK_EQ(f.fake.frontCalls, 1);

    f.block().capability = ContentCapability::MoveOnly;
    CHECK(!f.c.canPerform(ContentCommand::EditText));
    f.block().capability = ContentCapability::ReadOnly;
    CHECK(!f.c.canPerform(ContentCommand::DeleteObject));

    f.c.perform(ContentCommand::ToolEdit); // toggles the tool off
    CHECK(f.c.tool() == ContentTool::None);
    f.c.perform(ContentCommand::ToolAddText);
    CHECK(f.c.tool() == ContentTool::AddText);
}

RIVET_TEST(contentControllerSelectionIsPrunedWhenTheObjectDisappears) {
    Fixture f;
    f.shell.click(kInImage);
    CHECK(f.c.selected().has_value());
    f.shell.page().objects.clear();
    f.fake.fire(f.shell.pageId());
    CHECK(!f.c.selected().has_value());
}

RIVET_TEST(contentControllerReportsTextThatOverflowsTheOriginalBlock) {
    Fixture f;
    f.fake.editMarksEdited = false; // the edited block resolves later
    f.shell.click(kInBlock);
    f.c.editSelectedText();
    f.c.editorArea().setText("A much longer line that wraps");
    CHECK(f.c.commitEditor());
    CHECK(!contains(f.shell.lastStatus(), "extends below"));
    // The re-laid-out block arrives taller.
    f.block().bounds.size.height = 60.0;
    f.block().edited = true;
    f.fake.fire(f.shell.pageId());
    CHECK(contains(f.shell.lastStatus(), "Text extends below the original block"));
}
