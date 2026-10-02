// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#ifndef RIVET_PDF_TEXT_FIXTURE_DIR
#define RIVET_PDF_TEXT_FIXTURE_DIR "tests/pdf/fixtures"
#endif

// PDFium annotation adapter: reading (PdfDocument::annotations), render-time
// hiding, and the annotation edits of PdfEngine::assembleDocument, against
// the annots.pdf fixture. PDFium-ON bodies only; every body returns early
// when no PDFium backend exists.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using namespace rivet::pdf;

fs::path fixture(const char* name) {
    return fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / name;
}

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine) return nullptr;
    if (!engine->isAvailable()) {
        CHECK_EQ(engine->backendName(), "none");
        return nullptr;
    }
    return engine;
}

std::unique_ptr<PdfDocument> openPath(PdfEngine& engine, const fs::path& path) {
    auto opened = engine.openDocument(path, {});
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

PdfPageView nativeView(const PdfDocument& document, std::size_t page) {
    const auto info = document.pageInfo(page);
    CHECK(info.has_value());
    return info.has_value() ? info->view : PdfPageView{};
}

class MemorySink final : public IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        const auto* bytes = static_cast<const char*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
        return core::ok();
    }
    const std::vector<char>& bytes() const { return bytes_; }

private:
    std::vector<char> bytes_;
};

fs::path uniqueTempPath() {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("rivet-annots-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter++) + ".pdf");
}

std::unique_ptr<PdfDocument> reload(PdfEngine& engine, const MemorySink& sink) {
    CHECK(!sink.bytes().empty());
    const fs::path path = uniqueTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(sink.bytes().data(), static_cast<std::streamsize>(sink.bytes().size()));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

template <typename A, typename B>
bool near(A a, B b, double eps = 0.01) {
    return std::fabs(static_cast<double>(a) - static_cast<double>(b)) <= eps;
}

bool nearBox(const PdfBox& a, const PdfBox& b, double eps = 0.01) {
    return near(a.left, b.left, eps) && near(a.bottom, b.bottom, eps) && near(a.right, b.right, eps) &&
           near(a.top, b.top, eps);
}

bool nearPoint(const PdfPoint& a, const PdfPoint& b, double eps = 0.01) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps);
}

bool nearColor(const PdfColor& a, const PdfColor& b, double eps = 1.0 / 255.0 + 1e-6) {
    return near(a.r, b.r, eps) && near(a.g, b.g, eps) && near(a.b, b.b, eps);
}

PdfPageAnnotationsPtr readPage(const PdfDocument& document, std::size_t page) {
    const auto read = document.annotations(page);
    CHECK(read.has_value());
    return read.has_value() ? *read : nullptr;
}

// --- Assembly helpers -----------------------------------------------------

struct PageSpec {
    std::size_t source = 0;
    std::shared_ptr<const PdfPageAnnotationEdits> edits;
};

std::shared_ptr<const PdfPageAnnotationEdits> makeEdits(std::vector<std::uint32_t> remove,
                                                        std::vector<PdfAnnotationData> create) {
    auto edits = std::make_shared<PdfPageAnnotationEdits>();
    edits->removeIndices = std::move(remove);
    edits->create = std::move(create);
    return edits;
}

core::Status assemble(PdfEngine& engine, PdfAssemblyRequest::Mode mode, const PdfDocument& base,
                      const std::vector<PageSpec>& specs, MemorySink& sink,
                      std::vector<PdfAssembledPageAnnotations>* report) {
    PdfAssemblyRequest request;
    request.mode = mode;
    request.base = &base;
    for (const PageSpec& spec : specs) {
        request.pages.push_back(PdfAssemblyPage{&base, spec.source, nativeView(base, spec.source), spec.edits});
    }
    return engine.assembleDocument(request, sink, report);
}

std::vector<PageSpec> allPages(std::size_t count) {
    std::vector<PageSpec> specs;
    for (std::size_t i = 0; i < count; ++i) specs.push_back(PageSpec{i, nullptr});
    return specs;
}

// --- Data builders (user-space geometry inside every annots.pdf page's crop
// box, so the same data is valid on all three pages) -----------------------

PdfAnnotationData markup(PdfAnnotationKind kind) {
    PdfAnnotationData data;
    data.kind = kind;
    data.color = PdfColor{1.0F, 0.5F, 0.0F};
    data.opacity = 0.4F;
    data.quads.push_back(PdfQuad{{150, 520}, {350, 520}, {150, 500}, {350, 500}});
    data.quads.push_back(PdfQuad{{150, 480}, {250, 480}, {150, 462}, {250, 462}});
    data.contents = "marked text";
    data.author = "Rivet Tester";
    data.name = "rt-markup";
    return data;
}

PdfAnnotationData note() {
    PdfAnnotationData data;
    data.kind = PdfAnnotationKind::Note;
    data.rect = PdfBox{200, 300, 220, 320};
    data.color = PdfColor{1.0F, 0.8F, 0.0F};
    data.contents = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\nsecond line\n\xE2\x82\xAC 5";
    data.author = "Author";
    data.name = "rt-note";
    return data;
}

PdfAnnotationData ink() {
    PdfAnnotationData data;
    data.kind = PdfAnnotationKind::Ink;
    data.color = PdfColor{0.0F, 0.0F, 1.0F};
    data.opacity = 0.8F;
    data.borderWidth = 3.0F;
    data.inkStrokes.push_back({{160, 250}, {200, 290}, {240, 270}, {280, 330}});
    data.inkStrokes.push_back({{300, 250}, {340, 260}});
    data.name = "rt-ink";
    return data;
}

PdfAnnotationData square() {
    PdfAnnotationData data;
    data.kind = PdfAnnotationKind::Square;
    data.rect = PdfBox{200, 300, 300, 360};
    data.color = PdfColor{0.0F, 0.5F, 0.0F};
    data.interiorColor = PdfColor{1.0F, 0.9F, 0.9F};
    data.opacity = 0.7F;
    data.borderWidth = 2.0F;
    data.contents = "a square";
    data.name = "rt-square";
    return data;
}

PdfAnnotationData circle() {
    PdfAnnotationData data;
    data.kind = PdfAnnotationKind::Circle;
    data.rect = PdfBox{210, 400, 330, 450};
    data.color = PdfColor{1.0F, 0.0F, 1.0F};
    data.borderWidth = 4.0F;
    data.name = "rt-circle";
    return data;
}

PdfAnnotationData lineLike(PdfAnnotationKind kind) {
    PdfAnnotationData data;
    data.kind = kind;
    data.lineStart = PdfPoint{160, 250};
    data.lineEnd = PdfPoint{400, 350};
    data.color = PdfColor{0.2F, 0.4F, 0.6F};
    data.borderWidth = 2.0F;
    data.name = kind == PdfAnnotationKind::Line ? "rt-line" : "rt-arrow";
    return data;
}

PdfAnnotationData stamp(PdfStampName name, int rotation) {
    PdfAnnotationData data;
    data.kind = PdfAnnotationKind::Stamp;
    data.rect = PdfBox{200, 400, 320, 460};
    data.stampName = name;
    data.rotation = rotation;
    data.color = PdfColor{0.8F, 0.0F, 0.0F};
    data.opacity = 0.9F;
    data.contents = "stamp";
    data.name = std::string("rt-stamp-") + stampNameText(name);
    return data;
}

PdfAnnotationData normalized(PdfAnnotationData data) {
    normalizeAnnotation(data);
    CHECK(isWritableAnnotation(data));
    return data;
}

// Compares what the data model says is meaningful for the kind.
void expectMatches(const PdfAnnotationData& want, const PdfPageAnnotation& got) {
    CHECK(got.editable);
    CHECK(!got.isPopup);
    CHECK(got.kind == want.kind);
    CHECK(got.data.kind == want.kind);
    CHECK(nearBox(got.data.rect, want.rect));
    CHECK(nearColor(got.data.color, want.color));
    CHECK(near(got.data.opacity, want.opacity, 0.01));
    CHECK_EQ(got.data.contents, want.contents);
    CHECK_EQ(got.data.author, want.author);
    CHECK_EQ(got.data.name, want.name);
    switch (want.kind) {
        case PdfAnnotationKind::Highlight:
        case PdfAnnotationKind::Underline:
        case PdfAnnotationKind::StrikeOut:
            CHECK_EQ(got.data.quads.size(), want.quads.size());
            for (std::size_t i = 0; i < std::min(got.data.quads.size(), want.quads.size()); ++i) {
                CHECK(nearPoint(got.data.quads[i].p1, want.quads[i].p1));
                CHECK(nearPoint(got.data.quads[i].p2, want.quads[i].p2));
                CHECK(nearPoint(got.data.quads[i].p3, want.quads[i].p3));
                CHECK(nearPoint(got.data.quads[i].p4, want.quads[i].p4));
            }
            break;
        case PdfAnnotationKind::Ink:
            CHECK(near(got.data.borderWidth, want.borderWidth, 0.05));
            CHECK_EQ(got.data.inkStrokes.size(), want.inkStrokes.size());
            for (std::size_t s = 0; s < std::min(got.data.inkStrokes.size(), want.inkStrokes.size()); ++s) {
                CHECK_EQ(got.data.inkStrokes[s].size(), want.inkStrokes[s].size());
                for (std::size_t p = 0; p < std::min(got.data.inkStrokes[s].size(), want.inkStrokes[s].size()); ++p) {
                    CHECK(nearPoint(got.data.inkStrokes[s][p], want.inkStrokes[s][p]));
                }
            }
            break;
        case PdfAnnotationKind::Square:
        case PdfAnnotationKind::Circle:
            CHECK(near(got.data.borderWidth, want.borderWidth, 0.05));
            CHECK_EQ(got.data.interiorColor.has_value(), want.interiorColor.has_value());
            if (got.data.interiorColor && want.interiorColor) {
                CHECK(nearColor(*got.data.interiorColor, *want.interiorColor));
            }
            break;
        case PdfAnnotationKind::Line:
        case PdfAnnotationKind::Arrow:
            CHECK(near(got.data.borderWidth, want.borderWidth, 0.05));
            CHECK(nearPoint(got.data.lineStart, want.lineStart));
            CHECK(nearPoint(got.data.lineEnd, want.lineEnd));
            break;
        case PdfAnnotationKind::Stamp:
            CHECK(got.data.stampName == want.stampName);
            CHECK_EQ(got.data.rotation, want.rotation);
            break;
        case PdfAnnotationKind::Note:
        case PdfAnnotationKind::Other:
            break;
    }
}

// Creates `data` on page `pageIndex` of annots.pdf (PreserveBase, all three
// pages), reloads, and checks the created entry round-trips.
void roundTripOnPage(PdfEngine& engine, const PdfDocument& source, std::size_t pageIndex,
                     const PdfAnnotationData& raw) {
    const PdfAnnotationData want = normalized(raw);
    std::vector<PageSpec> specs = allPages(source.info().pageCount);
    specs[pageIndex].edits = makeEdits({}, {want});
    const std::uint32_t before = readPage(source, pageIndex)->annotsCount;

    MemorySink sink;
    std::vector<PdfAssembledPageAnnotations> report;
    const auto status = assemble(engine, PdfAssemblyRequest::Mode::PreserveBase, source, specs, sink, &report);
    CHECK(status.has_value());
    if (!status.has_value()) return;
    CHECK_EQ(report.size(), specs.size());
    if (report.size() != specs.size()) return;
    CHECK_EQ(report[pageIndex].createdIndices.size(), std::size_t{1});
    CHECK_EQ(report[pageIndex].annotsCount, before + 1);
    if (report[pageIndex].createdIndices.size() != 1) return;
    CHECK_EQ(report[pageIndex].createdIndices[0], before);

    auto reloaded = reload(engine, sink);
    if (!reloaded) return;
    const auto page = readPage(*reloaded, pageIndex);
    if (!page) return;
    CHECK_EQ(page->annotsCount, before + 1);
    if (page->items.size() != before + 1) {
        CHECK_EQ(page->items.size(), std::size_t{before + 1});
        return;
    }
    expectMatches(want, page->items[report[pageIndex].createdIndices[0]]);
}

void roundTripEverywhere(const PdfAnnotationData& data) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openPath(*engine, fixture("annots.pdf"));
    if (!source) return;
    CHECK_EQ(source->info().pageCount, std::size_t{3});
    for (std::size_t page = 0; page < 3; ++page) roundTripOnPage(*engine, *source, page, data);
}

// Fraction of non-white pixels in a device-pixel region.
double inkRatio(const core::Bitmap& bitmap, std::uint32_t px, std::uint32_t py, std::uint32_t pw, std::uint32_t ph) {
    if (px >= bitmap.width() || py >= bitmap.height()) return 0.0;
    const std::uint32_t w = std::min(pw, bitmap.width() - px);
    const std::uint32_t h = std::min(ph, bitmap.height() - py);
    const auto* base = reinterpret_cast<const std::uint8_t*>(bitmap.data());
    std::size_t ink = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* row = base + (static_cast<std::size_t>(py) + y) * bitmap.stride() + static_cast<std::size_t>(px) * 4;
        for (std::uint32_t x = 0; x < w; ++x) {
            if (row[x * 4] < 250 || row[x * 4 + 1] < 250 || row[x * 4 + 2] < 250) ++ink;
        }
    }
    return w * h == 0 ? 0.0 : static_cast<double>(ink) / static_cast<double>(w * h);
}

bool sameBytes(const core::Bitmap& a, const core::Bitmap& b) {
    if (a.width() != b.width() || a.height() != b.height() || a.stride() != b.stride()) return false;
    return std::equal(a.data(), a.data() + a.sizeBytes(), b.data());
}

} // namespace

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

RIVET_TEST(annotations_read_fixture_page1) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots.pdf"));
    if (!doc) return;

    const auto page = readPage(*doc, 0);
    if (!page) return;
    CHECK_EQ(page->annotsCount, 12u);
    CHECK_EQ(page->items.size(), std::size_t{12});
    if (page->items.size() != 12) return;

    constexpr auto K = PdfAnnotationKind::Other;
    const PdfAnnotationKind kinds[12] = {
        PdfAnnotationKind::Highlight, PdfAnnotationKind::Underline, PdfAnnotationKind::StrikeOut,
        PdfAnnotationKind::Note,      K /*Popup*/,                  PdfAnnotationKind::Ink,
        PdfAnnotationKind::Square,    PdfAnnotationKind::Circle,    K /*Line*/,
        PdfAnnotationKind::Stamp,     K /*FreeText*/,               K /*Squiggly*/,
    };
    for (std::size_t i = 0; i < 12; ++i) {
        const PdfPageAnnotation& item = page->items[i];
        CHECK_EQ(item.index, static_cast<std::uint32_t>(i));
        CHECK(item.kind == kinds[i]);
        CHECK(item.editable == (kinds[i] != K));
        CHECK_EQ(item.isPopup, i == 4);
        CHECK_EQ(item.flags, i == 4 ? 0u : 4u);
        if (kinds[i] == K) CHECK(item.data.kind == K);
    }
    CHECK(page->items[3].popupIndex.has_value());
    if (page->items[3].popupIndex) CHECK_EQ(*page->items[3].popupIndex, 4u);
    for (std::size_t i = 0; i < 12; ++i) {
        if (i != 3) CHECK(!page->items[i].popupIndex.has_value());
    }
    // Opaque entries keep their stored rect.
    CHECK(nearBox(page->items[8].data.rect, PdfBox{95, 395, 305, 455}));
    CHECK(nearBox(page->items[10].data.rect, PdfBox{300, 300, 450, 350}));
    CHECK(nearBox(page->items[11].data.rect, PdfBox{100, 250, 300, 270}));

    // Highlight: quad in Z order, CA, contents.
    const PdfAnnotationData& highlight = page->items[0].data;
    CHECK_EQ(highlight.quads.size(), std::size_t{1});
    if (highlight.quads.size() == 1) {
        CHECK(nearPoint(highlight.quads[0].p1, PdfPoint{100, 720}));
        CHECK(nearPoint(highlight.quads[0].p2, PdfPoint{300, 720}));
        CHECK(nearPoint(highlight.quads[0].p3, PdfPoint{100, 700}));
        CHECK(nearPoint(highlight.quads[0].p4, PdfPoint{300, 700}));
    }
    CHECK(nearColor(highlight.color, PdfColor{1, 1, 0}));
    CHECK(near(highlight.opacity, 0.5, 0.01));
    CHECK_EQ(highlight.contents, "Highlighted");
    CHECK(nearBox(highlight.rect, PdfBox{100, 700, 300, 720}));

    // Underline without an /AP is still read; opacity defaults to 1.
    CHECK(nearColor(page->items[1].data.color, PdfColor{0, 0.5F, 0}));
    CHECK(near(page->items[1].data.opacity, 1.0, 0.01));
    CHECK_EQ(page->items[1].data.quads.size(), std::size_t{1});
    CHECK(nearColor(page->items[2].data.color, PdfColor{1, 0, 0}));

    // Note: Cyrillic contents round to UTF-8 exactly.
    const PdfAnnotationData& noteData = page->items[3].data;
    CHECK_EQ(noteData.contents, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, \xD0\xBC\xD0\xB8\xD1\x80");
    CHECK_EQ(noteData.author, "Tester");
    CHECK_EQ(noteData.name, "note-1");
    CHECK(nearColor(noteData.color, PdfColor{1, 0.8F, 0}));
    CHECK(nearBox(noteData.rect, PdfBox{400, 700, 420, 720}));

    // Ink: two strokes of three points, width from the appearance.
    const PdfAnnotationData& inkData = page->items[5].data;
    CHECK_EQ(inkData.inkStrokes.size(), std::size_t{2});
    if (inkData.inkStrokes.size() == 2) {
        CHECK_EQ(inkData.inkStrokes[0].size(), std::size_t{3});
        CHECK_EQ(inkData.inkStrokes[1].size(), std::size_t{3});
        CHECK(nearPoint(inkData.inkStrokes[0][0], PdfPoint{100, 500}));
        CHECK(nearPoint(inkData.inkStrokes[0][2], PdfPoint{160, 560}));
        CHECK(nearPoint(inkData.inkStrokes[1][1], PdfPoint{200, 520}));
    }
    CHECK(nearColor(inkData.color, PdfColor{0, 0, 1}));
    CHECK(near(inkData.borderWidth, 2.0, 0.05));

    // Square: fill, border width, rect; Circle width.
    const PdfAnnotationData& squareData = page->items[6].data;
    CHECK(nearBox(squareData.rect, PdfBox{300, 500, 400, 560}));
    CHECK(nearColor(squareData.color, PdfColor{0, 0.5F, 0}));
    CHECK(squareData.interiorColor.has_value());
    if (squareData.interiorColor) CHECK(nearColor(*squareData.interiorColor, PdfColor{0.8F, 1, 0.8F}));
    CHECK(near(squareData.borderWidth, 3.0, 0.05));
    CHECK(near(page->items[7].data.borderWidth, 4.0, 0.05));
    CHECK(nearColor(page->items[7].data.color, PdfColor{1, 0, 1}));
    CHECK(!page->items[7].data.interiorColor.has_value());

    // Stamp.
    const PdfAnnotationData& stampData = page->items[9].data;
    CHECK(stampData.stampName == PdfStampName::Approved);
    CHECK_EQ(stampData.rotation, 0);
    CHECK_EQ(stampData.name, "stamp-1");
    CHECK(nearColor(stampData.color, PdfColor{0, 0.6F, 0}));

    // Pages 2 and 3: one editable Square each, in user space.
    const auto page2 = readPage(*doc, 1);
    const auto page3 = readPage(*doc, 2);
    if (!page2 || !page3) return;
    CHECK_EQ(page2->items.size(), std::size_t{1});
    CHECK_EQ(page3->items.size(), std::size_t{1});
    if (page2->items.size() == 1) {
        CHECK(page2->items[0].kind == PdfAnnotationKind::Square);
        CHECK(nearBox(page2->items[0].data.rect, PdfBox{100, 100, 200, 160}));
        CHECK(nearColor(page2->items[0].data.color, PdfColor{1, 0, 0}));
    }
    if (page3->items.size() == 1) {
        CHECK(page3->items[0].kind == PdfAnnotationKind::Square);
        CHECK(nearBox(page3->items[0].data.rect, PdfBox{150, 150, 250, 250}));
    }
    CHECK(!doc->annotations(3).has_value());
}

RIVET_TEST(annotations_read_is_repeatable_and_survives_rendering) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots.pdf"));
    if (!doc) return;
    const auto first = readPage(*doc, 0);
    const auto second = readPage(*doc, 0);
    if (!first || !second) return;
    CHECK(first->items.size() == second->items.size());
    for (std::size_t i = 0; i < std::min(first->items.size(), second->items.size()); ++i) {
        CHECK(first->items[i].data == second->items[i].data);
        CHECK_EQ(first->items[i].flags, second->items[i].flags);
    }

    // A document whose page was rendered (live doc mutated by PDFium) before
    // the first read still yields the file values.
    auto rendered = openPath(*engine, fixture("annots.pdf"));
    if (!rendered) return;
    const PdfPageView view = nativeView(*rendered, 0);
    const std::uint32_t hidden[] = {0, 3, 5};
    const auto bitmap = rendered->renderPage(0, view, hidden, core::Rect{0, 0, 612, 792}, 1.0);
    CHECK(bitmap.has_value());
    const auto after = readPage(*rendered, 0);
    if (!after) return;
    CHECK_EQ(after->items.size(), first->items.size());
    for (std::size_t i = 0; i < std::min(after->items.size(), first->items.size()); ++i) {
        CHECK(after->items[i].data == first->items[i].data);
        CHECK_EQ(after->items[i].flags, first->items[i].flags);
        CHECK(after->items[i].kind == first->items[i].kind);
    }
}

// ---------------------------------------------------------------------------
// Render-time hiding
// ---------------------------------------------------------------------------

RIVET_TEST(annotations_render_hide) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots.pdf"));
    if (!doc) return;
    const PdfPageView view = nativeView(*doc, 0);
    const core::Rect all{0, 0, 612, 792};

    const auto shown = doc->renderPage(0, view, std::span<const std::uint32_t>{}, all, 1.0);
    CHECK(shown.has_value());
    if (!shown.has_value()) return;
    // Highlight (index 0) occupies display y 72..92, x 100..300; the Square
    // (index 6) display y 232..292, x 300..400.
    CHECK_GT(inkRatio(*shown, 110, 75, 180, 14), 0.9);
    CHECK_GT(inkRatio(*shown, 310, 240, 80, 40), 0.9);

    const std::uint32_t hideHighlight[] = {0};
    const auto hidden = doc->renderPage(0, view, hideHighlight, all, 1.0);
    CHECK(hidden.has_value());
    if (!hidden.has_value()) return;
    CHECK(inkRatio(*hidden, 110, 75, 180, 14) < 0.001);
    CHECK_GT(inkRatio(*hidden, 310, 240, 80, 40), 0.9); // others untouched

    const std::uint32_t hideTwo[] = {0, 6, 6, 4000000};
    const auto hiddenTwo = doc->renderPage(0, view, hideTwo, all, 1.0);
    CHECK(hiddenTwo.has_value());
    if (hiddenTwo.has_value()) {
        CHECK(inkRatio(*hiddenTwo, 110, 75, 180, 14) < 0.001);
        CHECK(inkRatio(*hiddenTwo, 310, 240, 80, 40) < 0.001);
    }

    // Flags were restored: rendering again without hiding is identical.
    const auto again = doc->renderPage(0, view, std::span<const std::uint32_t>{}, all, 1.0);
    CHECK(again.has_value());
    if (again.has_value()) CHECK(sameBytes(*shown, *again));

    // Out-of-range indices only: nothing changes.
    const std::uint32_t outOfRange[] = {12, 999};
    const auto ignored = doc->renderPage(0, view, outOfRange, all, 1.0);
    CHECK(ignored.has_value());
    if (ignored.has_value()) CHECK(sameBytes(*shown, *ignored));

    // The persistent state (read through /F) is unchanged.
    const auto flags = readPage(*doc, 0);
    if (flags && flags->items.size() == 12) {
        CHECK_EQ(flags->items[0].flags, 4u);
        CHECK_EQ(flags->items[6].flags, 4u);
    }
}

// ---------------------------------------------------------------------------
// Round trips
// ---------------------------------------------------------------------------

RIVET_TEST(annotations_roundtrip_highlight) { roundTripEverywhere(markup(PdfAnnotationKind::Highlight)); }
RIVET_TEST(annotations_roundtrip_underline) { roundTripEverywhere(markup(PdfAnnotationKind::Underline)); }
RIVET_TEST(annotations_roundtrip_strikeout) { roundTripEverywhere(markup(PdfAnnotationKind::StrikeOut)); }
RIVET_TEST(annotations_roundtrip_note) { roundTripEverywhere(note()); }
RIVET_TEST(annotations_roundtrip_ink) { roundTripEverywhere(ink()); }
RIVET_TEST(annotations_roundtrip_square) { roundTripEverywhere(square()); }
RIVET_TEST(annotations_roundtrip_circle) { roundTripEverywhere(circle()); }
RIVET_TEST(annotations_roundtrip_line) { roundTripEverywhere(lineLike(PdfAnnotationKind::Line)); }
RIVET_TEST(annotations_roundtrip_arrow) { roundTripEverywhere(lineLike(PdfAnnotationKind::Arrow)); }

RIVET_TEST(annotations_roundtrip_stamps) {
    for (const PdfStampName name : {PdfStampName::Approved, PdfStampName::Draft, PdfStampName::Confidential,
                                    PdfStampName::Final}) {
        roundTripEverywhere(stamp(name, 90));
    }
    roundTripEverywhere(stamp(PdfStampName::Approved, 0));
    roundTripEverywhere(stamp(PdfStampName::Final, 270));
}

RIVET_TEST(annotations_roundtrip_generated_name_and_square_without_fill) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openPath(*engine, fixture("annots.pdf"));
    if (!source) return;
    PdfAnnotationData data = normalized(square());
    data.interiorColor.reset();
    data.name.clear();
    data.contents.clear();
    std::vector<PageSpec> specs = allPages(3);
    specs[0].edits = makeEdits({}, {data});
    MemorySink sink;
    CHECK(assemble(*engine, PdfAssemblyRequest::Mode::PreserveBase, *source, specs, sink, nullptr).has_value());
    auto reloaded = reload(*engine, sink);
    if (!reloaded) return;
    const auto page = readPage(*reloaded, 0);
    if (!page || page->items.size() != 13) {
        CHECK(page != nullptr);
        return;
    }
    const PdfAnnotationData& got = page->items[12].data;
    CHECK(page->items[12].kind == PdfAnnotationKind::Square);
    CHECK(!got.interiorColor.has_value());
    CHECK_EQ(got.name.size(), std::size_t{36}); // generated UUID
    CHECK(got.contents.empty());
}

// ---------------------------------------------------------------------------
// Remove + recreate, preservation, duplicates, Fresh, invalid
// ---------------------------------------------------------------------------

RIVET_TEST(annotations_remove_and_recreate) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openPath(*engine, fixture("annots.pdf"));
    if (!source) return;
    const auto original = readPage(*source, 0);
    if (!original || original->items.size() != 12) return;

    PdfAnnotationData edited = normalized(note());
    edited.name = "note-1";
    std::vector<PageSpec> specs = allPages(3);
    specs[0].edits = makeEdits({3, 4}, {edited});
    MemorySink sink;
    std::vector<PdfAssembledPageAnnotations> report;
    const auto status = assemble(*engine, PdfAssemblyRequest::Mode::PreserveBase, *source, specs, sink, &report);
    CHECK(status.has_value());
    if (!status.has_value()) return;
    CHECK_EQ(report.size(), std::size_t{3});
    if (report.size() != 3) return;
    CHECK_EQ(report[0].annotsCount, 11u);
    CHECK_EQ(report[0].createdIndices.size(), std::size_t{1});
    if (report[0].createdIndices.size() == 1) CHECK_EQ(report[0].createdIndices[0], 10u);
    CHECK_EQ(report[1].annotsCount, 1u);
    CHECK(report[1].createdIndices.empty());
    CHECK_EQ(report[2].annotsCount, 1u);

    auto reloaded = reload(*engine, sink);
    if (!reloaded) return;
    const auto page = readPage(*reloaded, 0);
    if (!page || page->items.size() != 11) {
        CHECK(page != nullptr);
        return;
    }
    // The other originals keep their relative order and kinds.
    const std::size_t from[10] = {0, 1, 2, 5, 6, 7, 8, 9, 10, 11};
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK(page->items[i].kind == original->items[from[i]].kind);
        CHECK(nearBox(page->items[i].data.rect, original->items[from[i]].data.rect));
        CHECK(!page->items[i].isPopup);
        CHECK(!page->items[i].popupIndex.has_value());
    }
    expectMatches(edited, page->items[10]);
    // Other pages are untouched.
    const auto page2 = readPage(*reloaded, 1);
    if (page2) CHECK_EQ(page2->items.size(), std::size_t{1});
}

RIVET_TEST(annotations_unsupported_preserved_without_edits) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openPath(*engine, fixture("annots.pdf"));
    if (!source) return;
    MemorySink sink;
    std::vector<PdfAssembledPageAnnotations> report;
    const auto status =
        assemble(*engine, PdfAssemblyRequest::Mode::PreserveBase, *source, allPages(3), sink, &report);
    CHECK(status.has_value());
    if (!status.has_value()) return;
    CHECK_EQ(report.size(), std::size_t{3});
    if (report.size() == 3) {
        CHECK_EQ(report[0].annotsCount, 12u);
        CHECK(report[0].createdIndices.empty());
    }
    auto reloaded = reload(*engine, sink);
    if (!reloaded) return;
    for (std::size_t p = 0; p < 3; ++p) {
        const auto want = readPage(*source, p);
        const auto got = readPage(*reloaded, p);
        if (!want || !got) continue;
        CHECK_EQ(got->annotsCount, want->annotsCount);
        CHECK_EQ(got->items.size(), want->items.size());
        for (std::size_t i = 0; i < std::min(got->items.size(), want->items.size()); ++i) {
            CHECK(got->items[i].kind == want->items[i].kind);
            CHECK_EQ(got->items[i].editable, want->items[i].editable);
            CHECK_EQ(got->items[i].isPopup, want->items[i].isPopup);
            CHECK(got->items[i].popupIndex == want->items[i].popupIndex);
            CHECK_EQ(got->items[i].flags, want->items[i].flags);
            CHECK(nearBox(got->items[i].data.rect, want->items[i].data.rect));
            CHECK_EQ(got->items[i].data.contents, want->items[i].data.contents);
            CHECK_EQ(got->items[i].data.author, want->items[i].data.author);
            CHECK_EQ(got->items[i].data.name, want->items[i].data.name);
        }
    }
}

RIVET_TEST(annotations_duplicated_page_edits_are_independent) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openPath(*engine, fixture("annots.pdf"));
    if (!source) return;
    // Page 2 has an INDIRECT /Annots array, the case where imports of the
    // same source page would share the array without per-copy imports.
    for (const std::size_t src : {std::size_t{1}, std::size_t{2}}) {
        PdfAnnotationData a = normalized(note());
        a.name = "copy-0";
        PdfAnnotationData b = normalized(square());
        b.name = "copy-1";
        PdfAnnotationData c = normalized(ink());
        c.name = "copy-2";
        const std::vector<PageSpec> specs = {
            PageSpec{src, makeEdits({}, {a})},
            PageSpec{src, makeEdits({0}, {b})},
            PageSpec{src, makeEdits({}, {c})},
        };
        MemorySink sink;
        std::vector<PdfAssembledPageAnnotations> report;
        const auto status = assemble(*engine, PdfAssemblyRequest::Mode::PreserveBase, *source, specs, sink, &report);
        CHECK(status.has_value());
        if (!status.has_value()) continue;
        CHECK_EQ(report.size(), std::size_t{3});
        auto reloaded = reload(*engine, sink);
        if (!reloaded) continue;
        CHECK_EQ(reloaded->info().pageCount, std::size_t{3});
        if (reloaded->info().pageCount != 3) continue;
        const auto p0 = readPage(*reloaded, 0);
        const auto p1 = readPage(*reloaded, 1);
        const auto p2 = readPage(*reloaded, 2);
        if (!p0 || !p1 || !p2) continue;
        CHECK_EQ(p0->items.size(), std::size_t{2});
        CHECK_EQ(p1->items.size(), std::size_t{1});
        CHECK_EQ(p2->items.size(), std::size_t{2});
        if (p0->items.size() == 2 && p1->items.size() == 1 && p2->items.size() == 2) {
            CHECK(p0->items[0].kind == PdfAnnotationKind::Square);
            CHECK(p0->items[1].kind == PdfAnnotationKind::Note);
            CHECK_EQ(p0->items[1].data.name, "copy-0");
            CHECK(p1->items[0].kind == PdfAnnotationKind::Square);
            CHECK_EQ(p1->items[0].data.name, "copy-1");
            CHECK(p2->items[0].kind == PdfAnnotationKind::Square);
            CHECK(p2->items[1].kind == PdfAnnotationKind::Ink);
            CHECK_EQ(p2->items[1].data.name, "copy-2");
        }
        if (report.size() == 3) {
            CHECK_EQ(report[0].annotsCount, 2u);
            CHECK_EQ(report[1].annotsCount, 1u);
            CHECK_EQ(report[2].annotsCount, 2u);
        }
    }
}

RIVET_TEST(annotations_fresh_mode_with_edits) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openPath(*engine, fixture("annots.pdf"));
    if (!source) return;
    PdfAnnotationData added = normalized(circle());
    PdfAnnotationData addedOnCrop = normalized(square());
    const std::vector<PageSpec> specs = {
        PageSpec{2, makeEdits({0}, {addedOnCrop})},
        PageSpec{0, makeEdits({3, 4}, {added})},
    };
    MemorySink sink;
    std::vector<PdfAssembledPageAnnotations> report;
    const auto status = assemble(*engine, PdfAssemblyRequest::Mode::Fresh, *source, specs, sink, &report);
    CHECK(status.has_value());
    if (!status.has_value()) return;
    CHECK_EQ(report.size(), std::size_t{2});
    auto reloaded = reload(*engine, sink);
    if (!reloaded) return;
    CHECK_EQ(reloaded->info().pageCount, std::size_t{2});
    if (reloaded->info().pageCount != 2 || report.size() != 2) return;
    const auto p0 = readPage(*reloaded, 0);
    const auto p1 = readPage(*reloaded, 1);
    if (!p0 || !p1) return;
    CHECK_EQ(p0->items.size(), std::size_t{1});
    CHECK_EQ(report[0].annotsCount, 1u);
    if (p0->items.size() == 1) expectMatches(addedOnCrop, p0->items[0]);
    CHECK_EQ(p1->items.size(), std::size_t{11});
    CHECK_EQ(report[1].annotsCount, 11u);
    if (p1->items.size() == 11 && report[1].createdIndices.size() == 1) {
        CHECK_EQ(report[1].createdIndices[0], 10u);
        expectMatches(added, p1->items[10]);
        CHECK(p1->items[0].kind == PdfAnnotationKind::Highlight);
        CHECK(p1->items[3].kind == PdfAnnotationKind::Ink);
    }
}

RIVET_TEST(annotations_invalid_edits_are_rejected) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openPath(*engine, fixture("annots.pdf"));
    if (!source) return;

    PdfAnnotationData nonFinite = normalized(square());
    nonFinite.rect.right = std::numeric_limits<double>::quiet_NaN();
    CHECK(!isWritableAnnotation(nonFinite));
    PdfAnnotationData other;
    other.kind = PdfAnnotationKind::Other;
    CHECK(!isWritableAnnotation(other));

    const std::vector<std::shared_ptr<const PdfPageAnnotationEdits>> bad = {
        makeEdits({12}, {}),                  // out of range (page 1 has 12 entries)
        makeEdits({999}, {}),                 // far out of range
        makeEdits({5, 3}, {}),                // unsorted
        makeEdits({3, 3}, {}),                // duplicate
        makeEdits({}, {nonFinite}),           // non-writable create
        makeEdits({}, {other}),               // non-writable kind
    };
    for (const auto& edits : bad) {
        std::vector<PageSpec> specs = allPages(3);
        specs[0].edits = edits;
        MemorySink sink;
        std::vector<PdfAssembledPageAnnotations> report;
        const auto status = assemble(*engine, PdfAssemblyRequest::Mode::PreserveBase, *source, specs, sink, &report);
        CHECK(!status.has_value());
        if (!status.has_value()) CHECK(status.error().code == core::ErrorCode::InvalidArgument);
    }
    // The source is unaffected by the failures.
    const auto page = readPage(*source, 0);
    if (page) CHECK_EQ(page->items.size(), std::size_t{12});
}
