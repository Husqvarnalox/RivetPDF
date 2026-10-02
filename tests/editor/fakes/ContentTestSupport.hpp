// SPDX-License-Identifier: MPL-2.0
#pragma once

// Shared fixture of the content-editing tests (Phase 5): synthetic page
// content objects (user space), and a DocumentSession over the fake backends
// with a queueing dispatcher plus helpers to wait for content extraction.

#include "AnnotationTestSupport.hpp"

#include "editor/ContentCommands.hpp"
#include "editor/ContentService.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace rivet::test {

// A text object with its baseline origin at (x, y), `width` wide, rotated by
// `angle` (radians) about the origin. Embedded non-subset font, so it is
// FullyEditable.
inline pdf::PdfContentObject makeTextObject(double x, double y, double size, const std::string& text, double width,
                                            double angle = 0.0) {
    pdf::PdfContentObject o;
    o.type = pdf::PdfContentObjectType::Text;
    o.matrix = core::Matrix::rotation(angle);
    o.matrix.tx = x;
    o.matrix.ty = y;
    o.text = text;
    o.fontSize = size;
    o.font.embedded = true;
    o.font.subset = false;
    o.font.baseName = "Arimo";
    o.fill = pdf::PdfColor{0.1F, 0.2F, 0.3F};
    const core::Point local[4] = {{0.0, -0.2 * size}, {width, -0.2 * size}, {width, 0.8 * size}, {0.0, 0.8 * size}};
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    for (std::size_t i = 0; i < 4; ++i) {
        const core::Point p = o.matrix.map(local[i]);
        o.quad[i] = pdf::PdfPoint{p.x, p.y};
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    o.bounds = pdf::PdfBox{minX, minY, maxX, maxY};
    return o;
}

// An axis-aligned image covering [left,right] x [bottom,top] (user space).
inline pdf::PdfContentObject makeImageObject(double left, double bottom, double right, double top) {
    pdf::PdfContentObject o;
    o.type = pdf::PdfContentObjectType::Image;
    o.matrix = core::Matrix{right - left, 0.0, 0.0, top - bottom, left, bottom};
    o.bounds = pdf::PdfBox{left, bottom, right, top};
    o.quad = {pdf::PdfPoint{left, bottom}, pdf::PdfPoint{right, bottom}, pdf::PdfPoint{right, top},
              pdf::PdfPoint{left, top}};
    o.pixelWidth = 16;
    o.pixelHeight = 8;
    return o;
}

inline pdf::PdfContentObject makePathObject(double left, double bottom, double right, double top) {
    pdf::PdfContentObject o = makeImageObject(left, bottom, right, top);
    o.type = pdf::PdfContentObjectType::Path;
    o.pixelWidth = 0;
    o.pixelHeight = 0;
    o.segmentCount = 4;
    return o;
}

// "Hello world" as two word objects at (x, y).
inline void addTextLine(pdf::PdfPageContent& content, double y, double size = 12.0, double x = 72.0) {
    content.objects.push_back(makeTextObject(x, y, size, "Hello", 30.0));
    content.objects.push_back(makeTextObject(x + 34.0, y, size, "world", 33.0));
}

// Page 0 of the fixtures below (user space, y up; the page is 612 x 792, so
// display y = 792 - user y):
//   0 image   (100,500)-(200,600)
//   1 image   (150,550)-(250,650)   overlaps 0 and is above it
//   2,3 text  "Hello" "world" at (72,700)  -> one block
//   4 unknown (400,100)-(450,150)   read-only
inline pdf::PdfPageContent samplePage() {
    pdf::PdfPageContent content;
    content.objects.push_back(makeImageObject(100, 500, 200, 600));
    content.objects.push_back(makeImageObject(150, 550, 250, 650));
    addTextLine(content, 700.0);
    pdf::PdfContentObject unknown = makeImageObject(400, 100, 450, 150);
    unknown.type = pdf::PdfContentObjectType::Unknown;
    content.objects.push_back(unknown);
    return content;
}

inline core::Point disp(double x, double userY) { return core::Point{x, 792.0 - userY}; }


inline std::shared_ptr<const pdf::PdfImageData> makeBgraImage(std::uint32_t width = 4, std::uint32_t height = 2) {
    auto image = std::make_shared<pdf::PdfImageData>();
    image->format = pdf::PdfImageData::Format::Bgra;
    image->width = width;
    image->height = height;
    image->stride = width * 4;
    image->bytes.assign(static_cast<std::size_t>(image->stride) * height, 0x7F);
    return image;
}

// Sets index / origin of every object to its position (what a backend reports).
inline pdf::PdfPageContent indexed(pdf::PdfPageContent content) {
    for (std::size_t i = 0; i < content.objects.size(); ++i) {
        content.objects[i].index = static_cast<std::uint32_t>(i);
        content.objects[i].origin = {pdf::PdfContentOrigin::Kind::Source, static_cast<std::uint32_t>(i), 0};
    }
    return content;
}

template <typename Engine>
struct BasicContentFixture {
    template <typename... Args>
    explicit BasicContentFixture(const std::filesystem::path& path, Args&&... args)
        : engine(std::forward<Args>(args)...) {
        init(path);
    }

    void init(const std::filesystem::path& path) {
        auto created = editor::DocumentSession::create(engine, scheduler, &dispatcher, path);
        CHECK(created.has_value());
        session = std::move(*created);
        document = dynamic_cast<FakePageDocument*>(session->documentPtr().get());
        CHECK(document != nullptr);
        for (std::size_t i = 0; i < session->pageCount(); ++i) ids.push_back(session->pageId(i));
        session->setOnContentChanged([this](core::PageId page) { changes.push_back(page); });
    }

    core::PageId id(std::size_t index) const { return ids.at(index); }

    // Configures the originals of a page (before its first load).
    void setContent(std::size_t page, pdf::PdfPageContent content) {
        document->pageContents[page] = indexed(std::move(content));
    }

    // Requests the page's content and waits until it is resolved.
    bool load(core::PageId page) {
        return settle(dispatcher, [&] { return session->contentService().content(page)->loaded; });
    }
    bool load(std::size_t index) { return load(id(index)); }

    editor::PageContentViewPtr content(core::PageId page) { return session->contentService().content(page); }
    editor::PageContentViewPtr content(std::size_t index) { return content(id(index)); }

    // Executes an edit; false on any error.
    bool run(core::Result<editor::ContentEdit> edit) {
        if (!edit.has_value()) return false;
        return session->execute(std::move(edit->command)).has_value();
    }

    const editor::PageEntry& entry(std::size_t index) const { return *session->pageSnapshot()->find(id(index)); }

    // The id of the `n`-th object (view order, bottom first) of a page.
    core::ObjectId objectId(std::size_t page, std::size_t n) { return content(page)->objects.at(n).id; }

    Engine engine;
    core::TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    std::unique_ptr<editor::DocumentSession> session;
    FakePageDocument* document = nullptr;
    std::vector<core::PageId> ids;
    std::vector<core::PageId> changes;
};

struct ContentFixture : BasicContentFixture<FakePageEngine> {
    explicit ContentFixture(std::size_t pages = 3) : BasicContentFixture("fake.pdf", pages) {}
};

} // namespace rivet::test
