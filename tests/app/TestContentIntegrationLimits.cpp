// SPDX-License-Identifier: MPL-2.0
// Phase 5 end to end (real engine): limits and failure paths - oversize image
// replacements, a read-only (CMYK) page, editing while a save holds the lock,
// closing a tab while its content loads, and quitting mid-save. Bodies return
// early without a backend.
#include "ContentIntegrationKit.hpp"

using namespace rivet;
using namespace rivet::test::integ;
using core::ObjectId;
using core::Point;
using core::Rect;
using pdf::PdfContentObjectType;

namespace {

constexpr const char* kLocked = "A save is in progress";

std::shared_ptr<const pdf::PdfImageData> bgra(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                                              std::size_t byteCount) {
    auto image = std::make_shared<pdf::PdfImageData>();
    image->format = pdf::PdfImageData::Format::Bgra;
    image->width = width;
    image->height = height;
    image->stride = stride;
    image->bytes.assign(byteCount, 0x80);
    return image;
}

std::shared_ptr<const pdf::PdfImageData> jpeg(std::vector<std::uint8_t> bytes, std::uint32_t side = 8) {
    auto image = std::make_shared<pdf::PdfImageData>();
    image->format = pdf::PdfImageData::Format::Jpeg;
    image->width = side;
    image->height = side;
    image->bytes = std::move(bytes);
    return image;
}

} // namespace

// 6a. replaceImage enforces the documented limits without touching the
// session, both through the command factory and through the controller.
RIVET_TEST(integReplaceImageRejectsOversizeAndMalformedData) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich();
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.selectTool();
    const auto view = rig.loaded(0);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    const auto* image = rig.objectOfType(*view, PdfContentObjectType::Image, 0);
    CHECK(image != nullptr);
    if (image == nullptr) return;
    const ObjectId imageId = image->id;
    const core::PageId page = rig.pageId(0);
    auto& session = rig.session();

    const auto refused = [&](std::shared_ptr<const pdf::PdfImageData> data, const char* what) {
        auto edit = editor::replaceImage(session, page, imageId, std::move(data));
        if (edit.has_value()) std::fprintf(stderr, "replaceImage unexpectedly accepted: %s\n", what);
        CHECK(!edit.has_value());
        if (!edit.has_value()) CHECK(!edit.error().message.empty());
        CHECK_EQ(rig.depth(), std::size_t{0});
        CHECK(!session.isDirty());
    };

    const std::uint32_t over = pdf::kMaxImageSide + 1;
    refused(nullptr, "null");
    refused(bgra(over, 1, over * 4, static_cast<std::size_t>(over) * 4), "width over the side limit");
    refused(bgra(1, over, 4, static_cast<std::size_t>(over) * 4), "height over the side limit");
    refused(bgra(0, 4, 0, 0), "zero width");
    refused(bgra(4, 0, 16, 0), "zero height");
    refused(bgra(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 16), "32-bit overflow");
    // Both sides legal, 50,010,000 pixels: over kMaxImagePixels (no data is
    // allocated: the count is checked before the buffer).
    CHECK(static_cast<std::uint64_t>(pdf::kMaxImageSide) * 5001u > pdf::kMaxImagePixels);
    refused(bgra(pdf::kMaxImageSide, 5001, pdf::kMaxImageSide * 4, 16), "pixel count over the limit");
    refused(bgra(4, 4, 8, 64), "stride below the row size");
    refused(bgra(4, 4, 16, 63), "data shorter than the bitmap");
    refused(jpeg({}), "empty JPEG");
    refused(jpeg({0x00, 0x01, 0x02, 0x03, 0x04}), "JPEG without SOI");
    {
        auto big = jpeg(std::vector<std::uint8_t>(pdf::kMaxImageEncodedBytes + 1, 0xFF));
        refused(std::move(big), "JPEG over the encoded size limit");
    }

    // The boundary itself is legal: kMaxImageSide x 1 builds a command (it is
    // not executed, 40 KB of pixels are enough).
    {
        auto edit = editor::replaceImage(session, page, imageId, bgra(pdf::kMaxImageSide, 1, pdf::kMaxImageSide * 4,
                                                                    static_cast<std::size_t>(pdf::kMaxImageSide) * 4));
        CHECK(edit.has_value());
    }
    CHECK_EQ(rig.depth(), std::size_t{0});

    // Through the controller: an oversize decode result is reported, nothing
    // is changed; a decoder failure and a cancelled dialog too.
    rig.click(centerOf(image->bounds));
    CHECK(rig.content->selectionIsImage());
    CHECK(writeFile(rig.dir("pick.img"), "placeholder"));
    rig.dialog.image = rig.dir("pick.img");
    {
        pdf::PdfImageData oversize;
        oversize.format = pdf::PdfImageData::Format::Bgra;
        oversize.width = over;
        oversize.height = 1;
        oversize.stride = over * 4;
        oversize.bytes.assign(static_cast<std::size_t>(over) * 4, 0x40);
        rig.decoder.result = std::move(oversize);
        rig.statusLog.clear();
        rig.content->replaceSelectedImage();
        CHECK(rig.dispatcher.waitUntil([&] { return rig.anyStatus("Edit:"); }));
        CHECK_EQ(rig.depth(), std::size_t{0});
        CHECK(!session.isDirty());
    }
    {
        rig.decoder.result = std::unexpected(core::makeError(core::ErrorCode::Unsupported, "cannot decode"));
        rig.statusLog.clear();
        rig.content->replaceSelectedImage();
        CHECK(rig.dispatcher.waitUntil([&] { return rig.anyStatus("Edit:"); }));
        CHECK_EQ(rig.depth(), std::size_t{0});
    }
    {
        const int calls = rig.decoder.calls.load();
        rig.dialog.image = std::unexpected(core::makeError(core::ErrorCode::Cancelled, "cancel"));
        rig.content->replaceSelectedImage();
        rig.dispatcher.pump();
        CHECK_EQ(rig.decoder.calls.load(), calls);
        CHECK_EQ(rig.depth(), std::size_t{0});
    }
    // A valid replacement still works afterwards.
    rig.dialog.image = rig.dir("pick.img");
    {
        pdf::PdfImageData ok;
        ok.format = pdf::PdfImageData::Format::Jpeg;
        ok.width = 8;
        ok.height = 8;
        ok.bytes = pdffix::blueJpeg();
        rig.decoder.result = std::move(ok);
    }
    rig.content->replaceSelectedImage();
    CHECK(rig.dispatcher.waitUntil([&] { return rig.depth() == 1; }));
    CHECK(session.isDirty());
}

// 6b. A page the regeneration probe refuses (CMYK colours) is read-only: every
// object says why, every factory refuses with a reason, no tool edits it.
RIVET_TEST(integCmykPageIsReadOnlyAndEveryFactoryRefuses) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openBytes("cmyk.pdf", pdffix::cmykPdf());
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.selectTool();
    const auto view = rig.loaded(0);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    CHECK(!view->objects.empty());
    CHECK(!view->regenerationSafe);
    CHECK(!view->regenerationIssue.empty());
    for (const auto& object : view->objects) {
        CHECK(object.capability == editor::ContentCapability::ReadOnly);
        CHECK(!object.capabilityReason.empty());
    }
    for (const auto& block : view->blocks) CHECK(block.capability == editor::ContentCapability::ReadOnly);

    auto& session = rig.session();
    const core::PageId page = rig.pageId(0);
    const auto mustRefuse = [&](core::Result<editor::ContentEdit> edit, const char* what) {
        if (edit.has_value()) std::fprintf(stderr, "factory unexpectedly accepted on a read-only page: %s\n", what);
        CHECK(!edit.has_value());
        if (!edit.has_value()) CHECK(!edit.error().message.empty());
    };
    const ObjectId anyObject = view->objects.front().id;
    mustRefuse(editor::moveContent(session, page, {anyObject}, Point{5.0, 5.0}), "move");
    mustRefuse(editor::deleteContent(session, page, {anyObject}), "delete");
    mustRefuse(editor::resizeContent(session, page, anyObject, Rect{10.0, 10.0, 50.0, 50.0}), "resize");
    mustRefuse(editor::replaceImage(session, page, anyObject, jpeg(pdffix::blueJpeg())), "replace image");
    mustRefuse(editor::bringToFront(session, page, anyObject), "bring to front");
    editor::NewTextBlock added;
    added.text = "Added";
    added.fontSize = 12.0;
    added.displayOrigin = Point{20.0, 100.0};
    mustRefuse(editor::addTextBlock(session, page, added), "add text");
    if (!view->blocks.empty()) {
        editor::TextBlockPatch edited;
        edited.text = std::string("Edited");
        mustRefuse(editor::editTextBlock(session, page, view->blocks.front().id, edited), "edit text");
        mustRefuse(editor::moveContent(session, page, {view->blocks.front().id}, Point{5.0, 5.0}), "move block");
    }
    CHECK_EQ(rig.depth(), std::size_t{0});

    // The tools cannot start an edit either.
    const Point at = centerOf(view->objects.front().bounds);
    rig.drag(at, Point{at.x + 15.0, at.y + 15.0});
    CHECK_EQ(rig.depth(), std::size_t{0});
    (void)rig.key(ui::Key::Delete);
    CHECK_EQ(rig.depth(), std::size_t{0});
    rig.addTextTool();
    rig.click(Point{100.0, 100.0});
    if (rig.content->editorOpen()) {
        rig.content->editorArea().setText("Added");
        CHECK(!rig.content->commitEditor());
        rig.content->cancelEditor();
    }
    CHECK_EQ(rig.depth(), std::size_t{0});
    CHECK(!session.isDirty());
}

// 6c. While a save holds the editing lock every way of editing is refused and
// says so; afterwards editing works again.
RIVET_TEST(integEditWhileASaveHoldsTheLockIsRefusedWithAStatus) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich();
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.selectTool();
    CHECK(rig.loaded(0) != nullptr);
    CHECK(retype(rig, "Hello World", "Before save"));
    const auto edited = rig.awaitBlock("Before save");
    CHECK(edited.has_value());
    if (!edited.has_value()) return;
    const ObjectId helloId = edited->id;
    const Rect helloBounds = edited->bounds;
    const std::size_t depth0 = rig.depth();

    GateGuard gate{rig.engine};
    rig.engine.closeGate();
    rig.statusLog.clear();
    rig.files->perform(app::FileCommand::Save);
    CHECK(rig.engine.waitParked(1));
    CHECK(rig.session().isEditingLocked());

    // Command factories.
    {
        auto move = editor::moveContent(rig.session(), rig.pageId(0), {helloId}, Point{3.0, 3.0});
        CHECK(!move.has_value());
    }
    CHECK(!rig.session().undo());
    CHECK(!rig.session().redo());
    // The tools: select + drag, Delete, inline edit.
    rig.statusLog.clear();
    rig.click(centerOf(helloBounds));
    rig.drag(centerOf(helloBounds), Point{centerOf(helloBounds).x + 20.0, centerOf(helloBounds).y + 10.0});
    CHECK_EQ(rig.depth(), depth0);
    CHECK(rig.anyStatus(kLocked));
    rig.statusLog.clear();
    rig.key(ui::Key::Delete);
    CHECK_EQ(rig.depth(), depth0);
    if (rig.content->selected().has_value()) {
        rig.content->editSelectedText();
        if (rig.content->editorOpen()) {
            rig.content->editorArea().setText("Typed during the save");
            rig.statusLog.clear();
            CHECK(!rig.content->commitEditor());
            CHECK(rig.anyStatus(kLocked));
            rig.content->cancelEditor();
        }
    }
    CHECK_EQ(rig.depth(), depth0);
    rig.statusLog.clear();
    rig.content->deleteSelected();
    CHECK_EQ(rig.depth(), depth0);

    // Release; the save completes with what was there when it started.
    rig.engine.release();
    CHECK(rig.dispatcher.waitUntil([&] { return rig.hasStatus("Saved") || rig.hasStatus("Save failed"); }));
    CHECK(rig.anyStatus("Saved"));
    CHECK(!rig.session().isEditingLocked());
    CHECK(!rig.session().isDirty());
    {
        auto reopened = rig.reopen(rig.session().path());
        CHECK(reopened != nullptr);
        if (reopened != nullptr) {
            CHECK(contains(textOf(*reopened, 0), "Before save"));
            CHECK(!contains(textOf(*reopened, 0), "Typed during the save"));
        }
    }

    // Editing is back.
    const auto after = rig.awaitBlock("Before save");
    CHECK(after.has_value());
    const std::size_t depthAfterSave = rig.depth();
    CHECK(retype(rig, "Before save", "After"));
    CHECK(rig.awaitBlock("After").has_value());
    CHECK_EQ(rig.depth(), depthAfterSave + 1);
    CHECK(rig.session().isDirty());
}

// 6d. Closing a tab right after its content extraction was requested (and
// while it may still be running) leaves nothing dangling: run under ASan.
RIVET_TEST(integCloseTabWhileContentIsLoadingLeavesNothingDangling) {
    Rig rig;
    if (!rig.ok()) return;
    for (int i = 0; i < 12; ++i) {
        DocumentTab* tab = rig.openBytes("closing" + std::to_string(i) + ".pdf",
                                         i % 3 == 2 ? pdffix::denseContentPdf(400) : pdffix::richPdf());
        CHECK(tab != nullptr);
        if (tab == nullptr) return;
        rig.selectTool();
        // Kick the lazy extraction of every page, then close at once.
        for (std::size_t p = 0; p < tab->session()->pageCount(); ++p) {
            (void)tab->session()->contentService().content(tab->session()->pageId(p));
        }
        if (i % 2 == 1) rig.click(Point{60.0, 60.0}); // a hit test on a (maybe) unloaded page
        rig.workspace.closeActiveTab();
        if (i % 4 == 0) rig.dispatcher.pump();
        CHECK_EQ(rig.workspace.tabCount(), std::size_t{0});
    }
    rig.dispatcher.pump();
    // The rig is still healthy: a new tab loads and edits.
    DocumentTab* tab = rig.openRich("final.pdf");
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    CHECK(rig.loaded(0) != nullptr);
    rig.selectTool();
    CHECK(retype(rig, "Hello World", "Still works"));
    CHECK(rig.awaitBlock("Still works").has_value());
}

// 6e. Quitting while an edit's save is in flight: the controllers' destructors
// fence their workers (the save is parked on the gate and released from
// another thread while quit waits), nothing is touched afterwards.
RIVET_TEST(integQuitWhileAnEditedDocumentIsBeingSavedLeavesNothingDangling) {
    Rig rig;
    if (!rig.ok()) return;
    DocumentTab* tab = rig.openRich("quit.pdf");
    CHECK(tab != nullptr);
    if (tab == nullptr) return;
    rig.selectTool();
    CHECK(rig.loaded(0) != nullptr);
    CHECK(retype(rig, "Hello World", "Quit edit"));
    CHECK(rig.awaitBlock("Quit edit").has_value());
    // An inline editor with uncommitted text is open at quit time too.
    rig.addTextTool();
    rig.click(Point{200.0, 350.0});
    CHECK(rig.content->editorOpen());
    rig.content->editorArea().setText("uncommitted at quit");

    GateGuard gate{rig.engine};
    rig.engine.closeGate();
    rig.files->perform(app::FileCommand::Save);
    CHECK(rig.engine.waitParked(1));
    CHECK(rig.session().isEditingLocked());
    rig.statusLog.clear();

    const int posted = rig.dispatcher.posted.load();
    std::thread releaser([&rig] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        rig.engine.release();
    });
    rig.quit();
    releaser.join();
    CHECK(rig.dispatcher.waitPosted(posted + 1));
    rig.dispatcher.pump();
    CHECK(rig.statusLog.empty()); // the dead controllers ignored the completion
    CHECK_EQ(rig.engine.assemblies.load(), 1);
}
