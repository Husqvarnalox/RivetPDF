// SPDX-License-Identifier: MPL-2.0
// Phase 5 end to end (real engine): images and paths through the content
// tools - corner-handle resize keeping the aspect, replacement with a decoded
// BGRA bitmap and a JPEG, delete / undo / redo, move, save and reopen. Bodies
// return early without a backend.
#include "ContentIntegrationKit.hpp"

#include "app/AnnotationInteraction.hpp"

using namespace rivet;
using namespace rivet::test::integ;
using core::ObjectId;
using core::Point;
using core::Rect;
using pdf::PdfContentObjectType;

namespace {

// The first object of `type` on page 0 of a freshly reopened document.
std::optional<pdf::PdfContentObject> firstOf(pdf::PdfDocument& document, PdfContentObjectType type) {
    const auto content = contentOf(document, 0);
    if (content == nullptr) return std::nullopt;
    for (const auto& object : content->objects) {
        if (object.type == type) return object;
    }
    return std::nullopt;
}

std::size_t countOf(pdf::PdfDocument& document, PdfContentObjectType type) {
    const auto content = contentOf(document, 0);
    if (content == nullptr) return 0;
    return static_cast<std::size_t>(std::count_if(content->objects.begin(), content->objects.end(),
                                                  [&](const auto& object) { return object.type == type; }));
}

pdf::PdfImageData solidBgra(std::uint32_t side, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
    pdf::PdfImageData image;
    image.format = pdf::PdfImageData::Format::Bgra;
    image.width = side;
    image.height = side;
    image.stride = side * 4;
    image.bytes.reserve(static_cast<std::size_t>(image.stride) * side);
    for (std::uint32_t i = 0; i < side * side; ++i) {
        image.bytes.push_back(b);
        image.bytes.push_back(g);
        image.bytes.push_back(r);
        image.bytes.push_back(255);
    }
    return image;
}

pdf::PdfImageData jpegImage() {
    pdf::PdfImageData image;
    image.format = pdf::PdfImageData::Format::Jpeg;
    image.width = 8;
    image.height = 8;
    image.bytes = rivet::test::pdffix::blueJpeg();
    return image;
}

// First image of page 0 once the content is loaded and its pixel width is
// `pixelWidth`.
bool awaitImagePixels(Rig& rig, std::uint32_t pixelWidth) {
    return rig.dispatcher.waitUntil([&] {
        const auto view = rig.contentNow(0);
        if (view == nullptr || !view->loaded) return false;
        const auto* image = rig.objectOfType(*view, PdfContentObjectType::Image, 0);
        return image != nullptr && image->pixelWidth == pixelWidth;
    });
}

bool awaitNoObject(Rig& rig, PdfContentObjectType type) {
    return rig.dispatcher.waitUntil([&] {
        const auto view = rig.contentNow(0);
        return view != nullptr && view->loaded && rig.objectOfType(*view, type, 0) == nullptr;
    });
}

bool awaitObject(Rig& rig, PdfContentObjectType type) {
    return rig.dispatcher.waitUntil([&] {
        const auto view = rig.contentNow(0);
        return view != nullptr && view->loaded && rig.objectOfType(*view, type, 0) != nullptr;
    });
}

// Replaces the selected image through the file dialog + decoder fakes (the
// real async path: scheduler worker, then the main-thread dispatcher).
bool replaceWith(Rig& rig, pdf::PdfImageData data) {
    CHECK(writeFile(rig.dir("pick.img"), "placeholder"));
    rig.dialog.image = rig.dir("pick.img");
    rig.decoder.result = std::move(data);
    const std::size_t depth = rig.depth();
    rig.content->replaceSelectedImage();
    return rig.dispatcher.waitUntil([&] { return rig.depth() == depth + 1; });
}

Rgb centerPixel(const core::Bitmap& bitmap, const Rect& display) {
    const Point c = centerOf(display);
    return pixelAt(bitmap, static_cast<int>(c.x), static_cast<int>(c.y));
}

} // namespace

// 4. The image: select, resize with a corner handle (aspect kept), replace
// with a BGRA bitmap then a JPEG, save and reopen, delete / undo / redo; then
// move and delete the path.
RIVET_TEST(integImageResizeReplaceDeleteAndPathMoveDelete) {
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
    const Rect imageBefore = image->bounds;
    CHECK_EQ(image->pixelWidth, std::uint32_t{4});
    CHECK_EQ(image->pixelHeight, std::uint32_t{4});
    const auto* path = rig.objectOfType(*view, PdfContentObjectType::Path, 0);
    CHECK(path != nullptr);
    if (path == nullptr) return;
    const ObjectId pathId = path->id;
    const Rect pathBefore = path->bounds;
    const std::size_t objectsBefore = view->objects.size();
    const PageStamp page1 = rig.stamp(1);

    // Select (the Link annotation is elsewhere on the page).
    rig.click(centerOf(imageBefore));
    {
        const auto selected = rig.content->selected();
        CHECK(selected.has_value());
        if (!selected.has_value()) return;
        CHECK(selected->info.id == imageId);
        CHECK(selected->info.kind == app::ContentInteraction::Kind::Image);
        CHECK(selected->info.canResize);
    }
    CHECK(rig.content->selectionIsImage());
    CHECK_EQ(rig.content->selectedImagePixels().first, std::uint32_t{4});

    // Resize through the bottom-right handle; images keep their aspect ratio
    // by default (the pointer's larger relative extent decides the scale).
    const std::size_t depth0 = rig.depth();
    const Point handle = app::AnnotationInteraction::handlePoint(imageBefore, app::AnnotationInteraction::Handle::BottomRight);
    rig.drag(handle, Point{handle.x + 40.0, handle.y + 10.0});
    CHECK_EQ(rig.depth(), depth0 + 1);
    Rect resized;
    CHECK(rig.dispatcher.waitUntil([&] {
        const auto now = rig.contentNow(0);
        if (now == nullptr || !now->loaded) return false;
        const auto* object = rig.objectById(*now, imageId);
        if (object == nullptr || object->bounds.size.width < imageBefore.size.width + 1.0) return false;
        resized = object->bounds;
        return true;
    }));
    CHECK_NEAR(resized.size.width / resized.size.height, imageBefore.size.width / imageBefore.size.height, 0.01);
    CHECK_NEAR(resized.minX(), imageBefore.minX(), 0.5); // the opposite corner stays put
    CHECK_NEAR(resized.minY(), imageBefore.minY(), 0.5);
    CHECK_NEAR(resized.size.width, imageBefore.size.width * 1.5, 0.5);
    CHECK(rig.stamp(1) == page1);

    // Replace with an 8x8 BGRA bitmap: the object keeps its id and frame.
    CHECK(rig.content->selected().has_value());
    CHECK(replaceWith(rig, solidBgra(8, 0, 200, 0)));
    CHECK(awaitImagePixels(rig, 8));
    {
        const auto now = rig.contentNow(0);
        const auto* object = rig.objectById(*now, imageId);
        CHECK(object != nullptr);
        if (object != nullptr) {
            CHECK_EQ(object->pixelHeight, std::uint32_t{8});
            CHECK_NEAR(object->bounds.size.width, resized.size.width, 0.5);
            CHECK_NEAR(object->bounds.minX(), resized.minX(), 0.5);
        }
    }
    const editor::PageEntry* entry = rig.session().pageSnapshot()->find(rig.pageId(0));
    CHECK(entry != nullptr && entry->contentEdits != nullptr);
    if (entry == nullptr) return;
    auto& original = *rig.session().documentPtr();
    {
        const core::Bitmap green = renderOf(original, 0, entry->contentEdits);
        const Rgb px = centerPixel(green, resized);
        CHECK(px.g > 150 && px.r < 90 && px.b < 90);
    }

    // Then a (tiny) JPEG; undo returns to the bitmap, redo to the JPEG.
    CHECK(replaceWith(rig, jpegImage()));
    {
        const editor::PageEntry* jpegEntry = rig.session().pageSnapshot()->find(rig.pageId(0));
        CHECK(jpegEntry != nullptr);
        if (jpegEntry == nullptr) return;
        const core::Bitmap blue = renderOf(original, 0, jpegEntry->contentEdits);
        const Rgb px = centerPixel(blue, resized);
        CHECK(px.b > 150 && px.r < 120);
    }
    CHECK(rig.session().undo());
    CHECK(rig.session().redo());
    CHECK(awaitImagePixels(rig, 8));
    CHECK(rig.stamp(1) == page1);

    // Save As, reopen: the image has the replacement's pixel size at the
    // resized frame, the text and annotations are untouched.
    const core::Bitmap beforeSave = [&] {
        const editor::PageEntry* e = rig.session().pageSnapshot()->find(rig.pageId(0));
        return renderOf(original, 0, e != nullptr ? e->contentEdits : nullptr);
    }();
    const fs::path saved = rig.dir("images.pdf");
    CHECK(rig.saveAsAndWait(saved));
    {
        auto reopened = rig.reopen(saved);
        CHECK(reopened != nullptr);
        if (reopened == nullptr) return;
        const auto savedImage = firstOf(*reopened, PdfContentObjectType::Image);
        CHECK(savedImage.has_value());
        if (savedImage.has_value()) {
            CHECK_EQ(savedImage->pixelWidth, std::uint32_t{8});
            CHECK_EQ(savedImage->pixelHeight, std::uint32_t{8});
            // User-space frame: x 40..160 (80 * 1.5 = 120 wide).
            CHECK_NEAR(savedImage->bounds.width(), resized.size.width, 0.6);
        }
        CHECK(contains(textOf(*reopened, 0), "Hello World"));
        CHECK_EQ(annotationCount(*reopened, 0), std::size_t{2});
        CHECK_EQ(linkCount(*reopened, 0), std::size_t{1});
        CHECK_NEAR(differingFraction(beforeSave, renderOf(*reopened, 0)), 0.0, 0.01);
    }

    // After the rebase: delete the image (one step), undo, redo.
    CHECK(rig.loaded(0) != nullptr);
    CHECK(awaitImagePixels(rig, 8));
    rig.click(centerOf(resized));
    {
        const auto selected = rig.content->selected();
        CHECK(selected.has_value() && selected->info.id == imageId);
    }
    const std::size_t depth1 = rig.depth();
    CHECK(rig.key(ui::Key::Delete));
    CHECK_EQ(rig.depth(), depth1 + 1);
    CHECK(awaitNoObject(rig, PdfContentObjectType::Image));
    CHECK(rig.session().undo());
    CHECK(awaitObject(rig, PdfContentObjectType::Image));
    CHECK(rig.session().redo());
    CHECK(awaitNoObject(rig, PdfContentObjectType::Image));
    CHECK(rig.stamp(1) == page1);

    // The path: select, move (one step), then delete.
    rig.selectTool();
    rig.click(centerOf(pathBefore));
    {
        const auto selected = rig.content->selected();
        CHECK(selected.has_value());
        if (!selected.has_value()) return;
        CHECK(selected->info.id == pathId);
        CHECK(selected->info.kind == app::ContentInteraction::Kind::Path);
    }
    const std::size_t depth2 = rig.depth();
    rig.drag(centerOf(pathBefore), Point{centerOf(pathBefore).x - 30.0, centerOf(pathBefore).y + 20.0});
    CHECK_EQ(rig.depth(), depth2 + 1);
    CHECK(rig.dispatcher.waitUntil([&] {
        const auto now = rig.contentNow(0);
        if (now == nullptr || !now->loaded) return false;
        const auto* object = rig.objectById(*now, pathId);
        return object != nullptr && std::abs(object->bounds.minX() - (pathBefore.minX() - 30.0)) < 0.5;
    }));
    {
        const auto now = rig.contentNow(0);
        const auto* object = rig.objectById(*now, pathId);
        CHECK(object != nullptr);
        if (object != nullptr) CHECK_NEAR(object->bounds.minY(), pathBefore.minY() + 20.0, 0.5);
    }
    CHECK(rig.key(ui::Key::Delete));
    CHECK_EQ(rig.depth(), depth2 + 2);
    CHECK(awaitNoObject(rig, PdfContentObjectType::Path));
    {
        const auto now = rig.contentNow(0);
        CHECK(now->objects.size() + 2 <= objectsBefore + 0);
    }

    // Final save: no image, no path, and the rest of the page is intact.
    CHECK(rig.saveAndWait());
    {
        auto reopened = rig.reopen(saved);
        CHECK(reopened != nullptr);
        if (reopened == nullptr) return;
        CHECK_EQ(countOf(*reopened, PdfContentObjectType::Image), std::size_t{0});
        CHECK_EQ(countOf(*reopened, PdfContentObjectType::Path), std::size_t{0});
        CHECK(contains(textOf(*reopened, 0), "Hello World"));
        CHECK(contains(textOf(*reopened, 0), "dog and keeps going"));
        CHECK(contains(textOf(*reopened, 1), "Page Two"));
        CHECK_EQ(annotationCount(*reopened, 0), std::size_t{2});
        CHECK_EQ(countOf(*reopened, PdfContentObjectType::Text) > 0, true);
        // Where the red rectangle was (original and moved position): white.
        const core::Bitmap page = renderOf(*reopened, 0);
        const Rgb gone = centerPixel(page, pathBefore);
        CHECK(gone.r > 230 && gone.g > 230 && gone.b > 230);
        const Rgb nowhere = centerPixel(page, resized);
        CHECK(nowhere.r > 230 && nowhere.g > 230 && nowhere.b > 230);
    }
}
