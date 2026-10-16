// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "PdfFixtures.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/BundledFontMetrics.hpp"
#include "pdf/PdfBuild.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"
#include "pdf/PdfText.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// PdfEngine::buildDocument against PDFium (ADR-0023): round trips through
// the engine's own readers (text, links, rendering) and the metrics
// consistency guarantee the Markdown export relies on. PDFium-ON bodies
// only; every body returns early when no backend exists (the null engine
// must report NotAvailable).

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using namespace rivet::pdf;

constexpr PdfBundledFont kAllFonts[] = {PdfBundledFont::SansRegular,  PdfBundledFont::SansBold,
                                        PdfBundledFont::SerifRegular, PdfBundledFont::SerifBold,
                                        PdfBundledFont::MonoRegular,  PdfBundledFont::MonoBold};

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

// Fails the write that would push the total beyond `limit` bytes.
class FailingSink final : public IPdfByteSink {
public:
    explicit FailingSink(std::size_t limit) : limit_(limit) {}
    core::Status write(const void* data, std::size_t size) override {
        (void)data;
        if (written_ + size > limit_) {
            return std::unexpected(core::makeError(core::ErrorCode::DiskFull, "sink full", "test"));
        }
        written_ += size;
        return core::ok();
    }

private:
    std::size_t limit_;
    std::size_t written_ = 0;
};

fs::path uniqueTempPath() {
    static std::atomic<int> counter{0};
    return fs::temp_directory_path() /
           ("rivet-build-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter.fetch_add(1)) + ".pdf");
}

// Engine with a backend, or nullptr after checking the null engine's
// NotAvailable contract.
std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine) return nullptr;
    if (!engine->isAvailable()) {
        MemorySink sink;
        PdfBuildDocument doc;
        doc.pages.emplace_back();
        const core::Status status = engine->buildDocument(doc, sink);
        CHECK(!status.has_value());
        if (!status.has_value()) CHECK_EQ(status.error().code, core::ErrorCode::NotAvailable);
        return nullptr;
    }
    return engine;
}

std::unique_ptr<PdfDocument> reopen(PdfEngine& engine, const MemorySink& sink) {
    CHECK(!sink.bytes().empty());
    const fs::path path = uniqueTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(sink.bytes().data(), static_cast<std::streamsize>(sink.bytes().size()));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored); // the document keeps its own descriptor
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

PdfBuildText run(PdfBundledFont font, double x, double y, std::string utf8, double size = 11.0) {
    PdfBuildText t;
    t.font = font;
    t.fontSize = size;
    t.origin = {x, y};
    t.utf8 = std::move(utf8);
    return t;
}

// Red 4x4 BGRA pixels.
PdfImageData redBitmap() {
    PdfImageData image;
    image.format = PdfImageData::Format::Bgra;
    image.width = 4;
    image.height = 4;
    image.stride = 16;
    for (int i = 0; i < 16; ++i) {
        image.bytes.insert(image.bytes.end(), {0x00, 0x00, 0xFF, 0xFF});
    }
    return image;
}

PdfImageData blueJpegImage() {
    PdfImageData image;
    image.format = PdfImageData::Format::Jpeg;
    image.width = 8;
    image.height = 8;
    image.bytes = rivet::test::pdffix::blueJpeg();
    return image;
}

const std::string kLatin = "Hello, World!";
const std::string kCyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";      // Привет
const std::string kGreek = "\xCE\xA9\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1";                  // Ωμέγα

// Page 0 (US Letter): one line per bundled font, a gray filled rect, a
// stroked rect, the red BGRA image and the blue JPEG, two links.
// Page 1 (A4): one line and one link-free rect.
PdfBuildDocument sampleDocument() {
    PdfBuildDocument doc;
    PdfBuildPage first;
    double y = 720.0;
    for (const PdfBundledFont font : kAllFonts) {
        first.items.emplace_back(run(font, 72.0, y, kLatin + " " + kCyrillic + " " + kGreek));
        y -= 20.0;
    }
    PdfBuildRect gray;
    gray.box = {72.0, 100.0, 172.0, 150.0};
    gray.fill = PdfColor{0.5F, 0.5F, 0.5F};
    first.items.emplace_back(gray);
    PdfBuildRect frame;
    frame.box = {200.0, 100.0, 260.0, 150.0};
    frame.stroke = PdfColor{0.0F, 0.0F, 0.0F};
    frame.strokeWidth = 2.0F;
    first.items.emplace_back(frame);
    first.items.emplace_back(PdfBuildImage{redBitmap(), {300.0, 300.0, 400.0, 400.0}});
    first.items.emplace_back(PdfBuildImage{blueJpegImage(), {420.0, 300.0, 520.0, 400.0}});
    first.links.push_back({{72.0, 715.0, 200.0, 732.0}, "https://example.com/first"});
    first.links.push_back({{72.0, 695.0, 200.0, 712.0}, "mailto:someone@example.com"});
    doc.pages.push_back(std::move(first));

    PdfBuildPage second;
    second.width = 595.0;
    second.height = 842.0;
    second.items.emplace_back(run(PdfBundledFont::SerifRegular, 72.0, 770.0, "Second page"));
    doc.pages.push_back(std::move(second));
    return doc;
}

// BGRA pixel of a bitmap rendered at scale 1 (display space, y-down).
struct Pixel {
    int b = 0, g = 0, r = 0;
};
Pixel pixelAt(const core::Bitmap& bitmap, std::uint32_t x, std::uint32_t y) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(bitmap.data()) + y * bitmap.stride() + x * 4u;
    return Pixel{p[0], p[1], p[2]};
}

} // namespace

RIVET_TEST(builderRoundTripsTextLinksAndPixels) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const PdfBuildDocument doc = sampleDocument();
    MemorySink sink;
    const core::Status built = engine->buildDocument(doc, sink);
    CHECK(built.has_value());
    if (!built.has_value()) return;
    auto reopened = reopen(*engine, sink);
    if (!reopened) return;

    CHECK_EQ(reopened->info().pageCount, std::size_t{2});
    const auto info0 = reopened->pageInfo(0);
    const auto info1 = reopened->pageInfo(1);
    CHECK(info0.has_value() && info1.has_value());
    if (!info0.has_value() || !info1.has_value()) return;
    CHECK_NEAR(info0->sizePoints.width, 612.0, 0.01);
    CHECK_NEAR(info0->sizePoints.height, 792.0, 0.01);
    CHECK_NEAR(info1->sizePoints.width, 595.0, 0.01);
    CHECK_NEAR(info1->sizePoints.height, 842.0, 0.01);

    // Real text objects: the extracted text holds every run, in order.
    const auto text0 = reopened->textPage(0);
    CHECK(text0.has_value());
    if (text0.has_value()) {
        const std::string& text = (*text0)->text();
        std::size_t from = 0;
        for (std::size_t i = 0; i < std::size(kAllFonts); ++i) {
            for (const std::string* part : {&kLatin, &kCyrillic, &kGreek}) {
                const std::size_t at = text.find(*part, from);
                CHECK(at != std::string::npos);
                if (at == std::string::npos) return;
                from = at + part->size();
            }
        }
    }
    const auto text1 = reopened->textPage(1);
    CHECK(text1.has_value() && (*text1)->text().find("Second page") != std::string::npos);

    // Links: two external URIs on page 0, none on page 1.
    const auto links0 = reopened->pageLinks(0);
    CHECK(links0.has_value());
    if (links0.has_value()) {
        CHECK_EQ(links0->size(), std::size_t{2});
        bool sawHttps = false;
        bool sawMail = false;
        for (const PdfPageLink& link : *links0) {
            CHECK_EQ(link.kind, PdfPageLink::Kind::External);
            sawHttps = sawHttps || link.url == "https://example.com/first";
            sawMail = sawMail || link.url == "mailto:someone@example.com";
            CHECK(!link.rects.empty());
        }
        CHECK(sawHttps);
        CHECK(sawMail);
    }
    const auto links1 = reopened->pageLinks(1);
    CHECK(links1.has_value() && links1->empty());

    // Pixels: rect, BGRA image and JPEG where they were placed; paper white.
    auto bitmap = reopened->renderPage(0, core::Rect(0.0, 0.0, 612.0, 792.0), 1.0);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return;
    const Pixel paper = pixelAt(*bitmap, 580, 20);
    CHECK(paper.r > 240 && paper.g > 240 && paper.b > 240);
    const Pixel gray = pixelAt(*bitmap, 120, 792 - 125); // user (120,125)
    CHECK(std::abs(gray.r - 128) < 12 && std::abs(gray.g - 128) < 12 && std::abs(gray.b - 128) < 12);
    const Pixel red = pixelAt(*bitmap, 350, 792 - 350);
    CHECK(red.r > 200 && red.g < 60 && red.b < 60);
    const Pixel blue = pixelAt(*bitmap, 470, 792 - 350);
    CHECK(blue.b > 150 && blue.r < 100);
    const Pixel inside = pixelAt(*bitmap, 230, 792 - 125); // stroked, not filled
    CHECK(inside.r > 240 && inside.g > 240 && inside.b > 240);
    const Pixel edge = pixelAt(*bitmap, 200, 792 - 125); // on the 2 pt stroke
    CHECK(edge.r < 100);
}

RIVET_TEST(builderWritesFilledAndDashedPaths) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    PdfBuildDocument doc;
    PdfBuildPage page;
    // A filled red square as a path (with a cubic edge), a dashed black
    // polyline and a round-joined stroke.
    PdfBuildPath square;
    square.segments.push_back({PdfPathSegment::Op::MoveTo, {100.0, 600.0}, {}, {}});
    square.segments.push_back({PdfPathSegment::Op::LineTo, {200.0, 600.0}, {}, {}});
    square.segments.push_back({PdfPathSegment::Op::CubicTo, {200.0, 700.0}, {205.0, 630.0}, {205.0, 670.0}});
    square.segments.push_back({PdfPathSegment::Op::LineTo, {100.0, 700.0}, {}, {}});
    square.segments.push_back({PdfPathSegment::Op::Close, {}, {}, {}});
    square.fill = PdfColor{1.0F, 0.0F, 0.0F};
    page.items.emplace_back(square);
    PdfBuildPath dashed;
    dashed.segments.push_back({PdfPathSegment::Op::MoveTo, {300.0, 650.0}, {}, {}});
    dashed.segments.push_back({PdfPathSegment::Op::LineTo, {500.0, 650.0}, {}, {}});
    dashed.stroke = PdfColor{0.0F, 0.0F, 0.0F};
    dashed.strokeWidth = 6.0F;
    dashed.dash = {20.0F, 20.0F};
    page.items.emplace_back(dashed);
    PdfBuildPath round = dashed;
    round.dash.clear();
    round.roundJoins = true;
    round.segments = {{PdfPathSegment::Op::MoveTo, {300.0, 500.0}, {}, {}},
                      {PdfPathSegment::Op::LineTo, {400.0, 500.0}, {}, {}},
                      {PdfPathSegment::Op::LineTo, {400.0, 400.0}, {}, {}}};
    page.items.emplace_back(round);
    doc.pages.push_back(std::move(page));

    MemorySink sink;
    CHECK(engine->buildDocument(doc, sink).has_value());
    auto reopened = reopen(*engine, sink);
    if (!reopened) return;
    auto bitmap = reopened->renderPage(0, core::Rect(0.0, 0.0, 612.0, 792.0), 1.0);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return;
    // Display space is y-down: user y 650 is row 792 - 650 = 142.
    const Pixel inside = pixelAt(*bitmap, 150, 142);
    CHECK(inside.r > 200 && inside.g < 60 && inside.b < 60);
    const Pixel dashOn = pixelAt(*bitmap, 310, 142);  // first dash: x 300..320
    const Pixel dashOff = pixelAt(*bitmap, 330, 142); // first gap: x 320..340
    CHECK(dashOn.r < 80 && dashOn.g < 80 && dashOn.b < 80);
    CHECK(dashOff.r > 200 && dashOff.g > 200 && dashOff.b > 200);
    const Pixel corner = pixelAt(*bitmap, 400, 292); // the round-joined corner
    CHECK(corner.r < 80);
}

RIVET_TEST(builderTextWidthsMatchBundledMetrics) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    // The run is followed by a copy of its first glyph: the distance between
    // the two copies' boxes is exactly the run's advance sum (both share the
    // same left side bearing), independent of glyph ink extents. ADR-0023
    // relies on this being what bundledFontMetrics measures; the required
    // bound is 0.5 pt, but the widths match to float precision.
    const std::string body = kLatin + " " + kCyrillic + " " + kGreek + " fijl WAVE";
    for (const PdfBundledFont font : kAllFonts) {
        for (const double size : {11.0, 23.5}) {
            PdfBuildDocument doc;
            doc.pages.emplace_back();
            doc.pages[0].items.emplace_back(run(font, 50.0, 400.0, body + "H", size));
            MemorySink sink;
            const core::Status built = engine->buildDocument(doc, sink);
            CHECK(built.has_value());
            if (!built.has_value()) return;
            auto reopened = reopen(*engine, sink);
            if (!reopened) return;
            const auto text = reopened->textPage(0);
            CHECK(text.has_value());
            if (!text.has_value()) return;
            const std::vector<rivet::pdf::TextChar>& chars = (*text)->chars();
            CHECK(chars.size() >= 2);
            if (chars.size() < 2) return;
            // Skip generated zero-area chars at the end, if any.
            std::size_t last = chars.size() - 1;
            while (last > 0 && chars[last].bounds.size.width <= 0.0) --last;
            CHECK_EQ(chars.front().unicode, U'H');
            CHECK_EQ(chars[last].unicode, U'H');
            const double measured = chars[last].bounds.origin.x - chars.front().bounds.origin.x;
            const double expected = bundledFontMetrics(font).textWidth(body, size);
            // PDFium positions glyphs with float precision only.
            CHECK_NEAR(measured, expected, 0.01);
        }
    }
}

RIVET_TEST(builderRejectsInvalidBuildsWithoutWriting) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const auto expectRejected = [&](const PdfBuildDocument& doc) {
        MemorySink sink;
        const core::Status status = engine->buildDocument(doc, sink);
        CHECK(!status.has_value());
        if (!status.has_value()) CHECK_EQ(status.error().code, core::ErrorCode::InvalidArgument);
        CHECK(sink.bytes().empty());
    };
    PdfBuildDocument uncovered = sampleDocument();
    uncovered.pages[1].items.emplace_back(run(PdfBundledFont::SansRegular, 72.0, 700.0, "\xE4\xB8\xAD"));
    expectRejected(uncovered);
    PdfBuildDocument outside = sampleDocument();
    PdfBuildRect rect;
    rect.box = {500.0, 500.0, 700.0, 600.0};
    rect.fill = PdfColor{};
    outside.pages[0].items.emplace_back(rect);
    expectRejected(outside);
    PdfBuildDocument scheme = sampleDocument();
    scheme.pages[0].links[0].uri = "javascript:alert(1)";
    expectRejected(scheme);
    expectRejected(PdfBuildDocument{});
}

RIVET_TEST(builderReportsSinkFailures) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const PdfBuildDocument doc = sampleDocument();
    for (const std::size_t limit : {std::size_t{0}, std::size_t{100}, std::size_t{20000}}) {
        FailingSink sink(limit);
        const core::Status status = engine->buildDocument(doc, sink);
        CHECK(!status.has_value());
        if (!status.has_value()) CHECK_EQ(status.error().code, core::ErrorCode::DiskFull);
    }
    // A sink failing only on the very last write is still reported (PDFium
    // drops the result of its final flush).
    MemorySink measure;
    CHECK(engine->buildDocument(doc, measure).has_value());
    FailingSink lastWrite(measure.bytes().size() - 1);
    const core::Status status = engine->buildDocument(doc, lastWrite);
    CHECK(!status.has_value());
}
