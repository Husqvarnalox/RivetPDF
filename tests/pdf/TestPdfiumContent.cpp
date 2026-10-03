// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Matrix.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"
#include "pdf/PdfText.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Phase 5 PDFium content backend (ADR-0014..0017): extraction, the
// regeneration probe, edit application (display and save) and assembly.
// PDFium-ON bodies only; every body returns early when no backend exists.
// Fixtures are tiny PDFs built from raw strings below (deterministic, no
// randomness, no timestamps).

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using rivet::pdf::PdfAssemblyPage;
using rivet::pdf::PdfAssemblyRequest;
using rivet::pdf::PdfBundledFont;
using rivet::pdf::PdfContentObject;
using rivet::pdf::PdfContentObjectType;
using rivet::pdf::PdfContentOrigin;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfFontRef;
using rivet::pdf::PdfObjectEdit;
using rivet::pdf::PdfPageContent;
using rivet::pdf::PdfPageContentEdits;
using rivet::pdf::PdfPageContentEditsPtr;
using rivet::pdf::PdfPageView;
using rivet::pdf::PdfTextBlockEdit;

// --- PDF fixtures ------------------------------------------------------------

// Serializes objects 1..N (bodies without the "n 0 obj" wrapper) with a valid
// xref table; the catalog must be object 1.
std::string buildPdf(const std::vector<std::string>& bodies) {
    std::string out = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < bodies.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + bodies[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(bodies.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        std::string number = std::to_string(offset);
        out += std::string(10 - number.size(), '0') + number + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + std::to_string(bodies.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return out;
}

std::string streamObject(const std::string& dictEntries, const std::string& data) {
    return "<< " + dictEntries + " /Length " + std::to_string(data.size()) + " >>\nstream\n" + data +
           "\nendstream";
}

constexpr const char* kFonts =
    "/Font << /F1 5 0 R >> /XObject << /Im1 6 0 R /Fm1 7 0 R >> ";

// The standard test page: 200x200, objects in z-order
//   0 red filled rectangle (20,20)-(80,60)
//   1 text "Hello World" Helvetica 18 at (20,150)
//   2 image 2x2 in (120,20)-(170,70)
//   3 blue stroked line (100,100)-(150,120)
std::string simplePdf(const std::string& mediaAndRotate = "/MediaBox [0 0 200 200]",
                      const std::string& content =
                          "1 0 0 rg 20 20 60 40 re f\n"
                          "BT /F1 18 Tf 20 150 Td (Hello World) Tj ET\n"
                          "q 50 0 0 50 120 20 cm /Im1 Do Q\n"
                          "0 0 1 RG 2 w 100 100 m 150 120 l S\n",
                      const std::string& extraResources = "",
                      const std::string& fontObject = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>") {
    return buildPdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R " + mediaAndRotate + " /Contents 4 0 R /Resources << " + kFonts +
            extraResources + ">> >>",
        streamObject("", content),
        fontObject,
        streamObject("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB "
                     "/BitsPerComponent 8 /Filter /ASCIIHexDecode",
                     "FF0000 00FF00 0000FF FFFF00>"),
        // A small form XObject (object 7), drawn by tests that use /Fm1.
        streamObject("/Type /XObject /Subtype /Form /BBox [0 0 30 30]", "0 1 0 rg 0 0 30 30 re f"),
    });
}

fs::path uniqueTempPath(const char* tag) {
    static std::atomic<int> counter{0};
    return fs::temp_directory_path() /
           ("rivet-content-" + std::string(tag) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter.fetch_add(1)) + ".pdf");
}

void removeQuietly(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
}

class MemorySink final : public rivet::pdf::IPdfByteSink {
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

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = rivet::pdf::createEngine();
    CHECK(engine != nullptr);
    if (!engine) return nullptr;
    if (!engine->isAvailable()) {
        CHECK_EQ(engine->backendName(), "none");
        return nullptr;
    }
    return engine;
}

std::unique_ptr<PdfDocument> openBytes(PdfEngine& engine, const void* data, std::size_t size) {
    const fs::path path = uniqueTempPath("open");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    removeQuietly(path); // the document keeps its own descriptor
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

std::unique_ptr<PdfDocument> openText(PdfEngine& engine, const std::string& pdf) {
    return openBytes(engine, pdf.data(), pdf.size());
}

std::unique_ptr<PdfDocument> openSink(PdfEngine& engine, const MemorySink& sink) {
    CHECK(!sink.bytes().empty());
    return openBytes(engine, sink.bytes().data(), sink.bytes().size());
}

PdfPageView nativeView(const PdfDocument& document, std::size_t page) {
    const auto info = document.pageInfo(page);
    CHECK(info.has_value());
    return info.has_value() ? info->view : PdfPageView{};
}

std::shared_ptr<const PdfPageContent> contentOf(const PdfDocument& document, std::size_t page,
                                                const PdfPageContentEditsPtr& edits = nullptr) {
    auto content = document.pageContent(page, edits);
    CHECK(content.has_value());
    if (!content.has_value()) return nullptr;
    return *content;
}

// --- Edits -------------------------------------------------------------------

struct Edits {
    std::shared_ptr<PdfPageContentEdits> value = std::make_shared<PdfPageContentEdits>();

    Edits& remove(std::uint32_t index) {
        PdfObjectEdit edit;
        edit.sourceIndex = index;
        edit.remove = true;
        value->objects.push_back(edit);
        return *this;
    }
    Edits& transform(std::uint32_t index, const core::Matrix& matrix) {
        PdfObjectEdit edit;
        edit.sourceIndex = index;
        edit.transform = matrix;
        value->objects.push_back(edit);
        return *this;
    }
    Edits& replaceImage(std::uint32_t index, std::shared_ptr<const rivet::pdf::PdfImageData> image,
                        std::optional<core::Matrix> matrix = std::nullopt) {
        PdfObjectEdit edit;
        edit.sourceIndex = index;
        edit.replaceImage = std::move(image);
        edit.transform = matrix;
        value->objects.push_back(edit);
        return *this;
    }
    Edits& text(PdfTextBlockEdit block) {
        value->textBlocks.push_back(std::move(block));
        return *this;
    }
    PdfPageContentEditsPtr ptr() const { return value; }
};


// --- Rendering / assembly helpers ---------------------------------------------

core::Bitmap renderFull(const PdfDocument& document, std::size_t page, const PdfPageContentEditsPtr& edits,
                        double scale = 1.0) {
    const PdfPageView view = nativeView(document, page);
    const auto info = document.pageInfo(page);
    CHECK(info.has_value());
    if (!info.has_value()) return {};
    const core::Size size = rivet::pdf::displaySize(view);
    // renderPage is non-const on the interface.
    auto& mutableDocument = const_cast<PdfDocument&>(document);
    auto bitmap = mutableDocument.renderPage(page, view, {}, edits, core::Rect(0.0, 0.0, size.width, size.height), scale);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return {};
    return std::move(*bitmap);
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb pixelAt(const core::Bitmap& bitmap, int x, int y) {
    CHECK(bitmap.isValid());
    if (!bitmap.isValid() || x < 0 || y < 0 || x >= static_cast<int>(bitmap.width()) ||
        y >= static_cast<int>(bitmap.height())) {
        return {-1, -1, -1};
    }
    const auto* row = reinterpret_cast<const std::uint8_t*>(bitmap.data()) + static_cast<std::size_t>(y) * bitmap.stride();
    const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
    return Rgb{px[2], px[1], px[0]};
}

bool near(const Rgb& c, int r, int g, int b, int tolerance = 40) {
    return std::abs(c.r - r) <= tolerance && std::abs(c.g - g) <= tolerance && std::abs(c.b - b) <= tolerance;
}

// Fraction of pixels whose channels differ by more than `channelTolerance`.
double differingFraction(const core::Bitmap& a, const core::Bitmap& b, int channelTolerance = 24) {
    CHECK_EQ(a.width(), b.width());
    CHECK_EQ(a.height(), b.height());
    if (a.width() != b.width() || a.height() != b.height() || a.width() == 0) return 1.0;
    std::size_t differing = 0;
    for (std::uint32_t y = 0; y < a.height(); ++y) {
        for (std::uint32_t x = 0; x < a.width(); ++x) {
            const Rgb pa = pixelAt(a, static_cast<int>(x), static_cast<int>(y));
            const Rgb pb = pixelAt(b, static_cast<int>(x), static_cast<int>(y));
            if (std::abs(pa.r - pb.r) > channelTolerance || std::abs(pa.g - pb.g) > channelTolerance ||
                std::abs(pa.b - pb.b) > channelTolerance) {
                ++differing;
            }
        }
    }
    return static_cast<double>(differing) / (static_cast<double>(a.width()) * static_cast<double>(a.height()));
}

struct Assembled {
    MemorySink sink;
    std::vector<rivet::pdf::PdfAssembledPageContent> report;
    core::Status status = core::ok();
};

// Preserve-base save of `document` with `edits` applied to page `page` (all
// other pages carried over unchanged).
Assembled assemble(PdfEngine& engine, const PdfDocument& document, std::size_t page,
                   const PdfPageContentEditsPtr& edits) {
    Assembled out;
    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = &document;
    for (std::size_t i = 0; i < document.info().pageCount; ++i) {
        PdfAssemblyPage entry;
        entry.source = &document;
        entry.sourcePageIndex = i;
        entry.view = nativeView(document, i);
        if (i == page) entry.contentEdits = edits;
        request.pages.push_back(std::move(entry));
    }
    out.status = engine.assembleDocument(request, out.sink, nullptr, &out.report);
    return out;
}

PdfContentOrigin sourceOrigin(std::uint32_t index) {
    PdfContentOrigin origin;
    origin.kind = PdfContentOrigin::Kind::Source;
    origin.sourceIndex = index;
    return origin;
}

PdfContentOrigin createdOrigin(std::uint64_t tag) {
    PdfContentOrigin origin;
    origin.kind = PdfContentOrigin::Kind::Created;
    origin.tag = tag;
    return origin;
}

PdfTextBlockEdit textBlock(std::uint64_t tag, std::string text, PdfFontRef font, core::Matrix placement,
                           double size = 18.0) {
    PdfTextBlockEdit block;
    block.tag = tag;
    block.text = std::move(text);
    block.font = font;
    block.placement = placement;
    block.fontSize = size;
    block.lineAdvance = size * 1.2;
    return block;
}

PdfFontRef fromObject(std::uint32_t index, PdfBundledFont fallback = PdfBundledFont::SansRegular) {
    PdfFontRef font;
    font.kind = PdfFontRef::Kind::FromObject;
    font.sourceIndex = index;
    font.fallback = fallback;
    return font;
}

PdfFontRef bundled(PdfBundledFont face) {
    PdfFontRef font;
    font.kind = PdfFontRef::Kind::Bundled;
    font.fallback = face;
    return font;
}

std::shared_ptr<const rivet::pdf::PdfImageData> solidBgra(std::uint32_t w, std::uint32_t h, std::uint8_t r,
                                                          std::uint8_t g, std::uint8_t b) {
    auto image = std::make_shared<rivet::pdf::PdfImageData>();
    image->format = rivet::pdf::PdfImageData::Format::Bgra;
    image->width = w;
    image->height = h;
    image->stride = w * 4;
    image->bytes.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < image->bytes.size(); i += 4) {
        image->bytes[i] = b;
        image->bytes[i + 1] = g;
        image->bytes[i + 2] = r;
        image->bytes[i + 3] = 255;
    }
    return image;
}

// Display-space pixel (y down) of a user-space point on an unrotated 200x200
// page at scale 1.
int dispY(double userY) { return static_cast<int>(200.0 - userY); }

} // namespace

// --- Extraction ---------------------------------------------------------------

RIVET_TEST(contentExtractionListsTopLevelObjects) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const auto content = contentOf(*document, 0);
    if (!content) return;
    CHECK(!content->truncated);
    CHECK(content->regenerationSafe);
    CHECK_EQ(content->objects.size(), std::size_t{4});
    if (content->objects.size() != 4) return;

    CHECK(content->objects[0].type == PdfContentObjectType::Path);
    CHECK(content->objects[1].type == PdfContentObjectType::Text);
    CHECK(content->objects[2].type == PdfContentObjectType::Image);
    CHECK(content->objects[3].type == PdfContentObjectType::Path);
    for (std::uint32_t i = 0; i < 4; ++i) {
        CHECK_EQ(content->objects[i].index, i);
        CHECK(content->objects[i].origin == sourceOrigin(i));
        CHECK_EQ(content->objects[i].blockTag, std::uint64_t{0});
        CHECK(!content->objects[i].hasClip);
    }

    const PdfContentObject& rect = content->objects[0];
    CHECK_NEAR(rect.bounds.left, 20.0, 0.5);
    CHECK_NEAR(rect.bounds.bottom, 20.0, 0.5);
    CHECK_NEAR(rect.bounds.right, 80.0, 0.5);
    CHECK_NEAR(rect.bounds.top, 60.0, 0.5);
    CHECK(rect.fill.has_value());
    if (rect.fill.has_value()) {
        CHECK_NEAR(static_cast<double>(rect.fill->r), 1.0, 0.01);
        CHECK_NEAR(static_cast<double>(rect.fill->g), 0.0, 0.01);
    }

    const PdfContentObject& text = content->objects[1];
    CHECK_EQ(text.text, "Hello World");
    CHECK_NEAR(text.fontSize, 18.0, 0.01);
    CHECK_NEAR(text.matrix.tx, 20.0, 0.01);
    CHECK_NEAR(text.matrix.ty, 150.0, 0.01);
    CHECK(text.font.baseName.find("Helvetica") != std::string::npos);
    CHECK(!text.font.type3);
    CHECK(!text.textUnmappable);
    CHECK_EQ(text.renderMode, 0);
    CHECK(!text.fontSubstituted);

    const PdfContentObject& image = content->objects[2];
    CHECK_EQ(image.pixelWidth, std::uint32_t{2});
    CHECK_EQ(image.pixelHeight, std::uint32_t{2});
    CHECK_NEAR(image.bounds.left, 120.0, 0.5);
    CHECK_NEAR(image.bounds.right, 170.0, 0.5);
    CHECK_NEAR(image.bounds.bottom, 20.0, 0.5);
    CHECK_NEAR(image.bounds.top, 70.0, 0.5);
    CHECK_NEAR(image.matrix.a, 50.0, 0.01);
    CHECK_NEAR(image.matrix.d, 50.0, 0.01);

    const PdfContentObject& line = content->objects[3];
    CHECK_GE(line.segmentCount, std::uint32_t{2});
    CHECK(line.stroke.has_value());
}

RIVET_TEST(contentExtractionIsCachedAndStable) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const auto first = contentOf(*document, 0);
    const auto second = contentOf(*document, 0);
    CHECK(first != nullptr && second != nullptr);
    if (!first || !second) return;
    CHECK_EQ(first->objects.size(), second->objects.size());
    // Out of range.
    CHECK(!document->pageContent(5, nullptr).has_value());
}

// --- Probe --------------------------------------------------------------------

RIVET_TEST(contentProbeFlagsShadingPages) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const std::string shading =
        "/Shading << /Sh1 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 100 0] /Function << "
        "/FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >> ";
    auto document = openText(*engine, simplePdf("/MediaBox [0 0 200 200]",
                                               "1 0 0 rg 20 20 60 40 re f\n/Sh1 sh\n", shading));
    if (!document) return;
    const auto content = contentOf(*document, 0);
    if (!content) return;
    CHECK(!content->regenerationSafe);
    CHECK(!content->regenerationIssue.empty());

    // Editing such a page is refused (assembly), never silently lossy.
    Edits edits;
    edits.remove(0);
    const Assembled assembled = assemble(*engine, *document, 0, edits.ptr());
    CHECK(!assembled.status.has_value());
    CHECK(assembled.sink.bytes().empty() || !assembled.status.has_value());
}

// --- Edit application ----------------------------------------------------------

namespace {

// 8x8 solid blue baseline JPEG (macOS `sips`, quality 60; 776 bytes).
const std::vector<std::uint8_t>& blueJpeg() {
    static const std::vector<std::uint8_t> bytes = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x48,
    0x00, 0x48, 0x00, 0x00, 0xFF, 0xE1, 0x00, 0x4C, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x4D, 0x4D,
    0x00, 0x2A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x01, 0x87, 0x69, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xA0, 0x01, 0x00, 0x03, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xA0, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x08, 0xA0, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00,
    0x00, 0x00, 0xFF, 0xED, 0x00, 0x38, 0x50, 0x68, 0x6F, 0x74, 0x6F, 0x73, 0x68, 0x6F, 0x70, 0x20,
    0x33, 0x2E, 0x30, 0x00, 0x38, 0x42, 0x49, 0x4D, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x38, 0x42, 0x49, 0x4D, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0xD4, 0x1D, 0x8C, 0xD9,
    0x8F, 0x00, 0xB2, 0x04, 0xE9, 0x80, 0x09, 0x98, 0xEC, 0xF8, 0x42, 0x7E, 0xFF, 0xC0, 0x00, 0x11,
    0x08, 0x00, 0x08, 0x00, 0x08, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF,
    0xC4, 0x00, 0x1F, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0xFF, 0xC4, 0x00, 0xB5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41,
    0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1,
    0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19,
    0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2,
    0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9,
    0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3,
    0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF, 0xC4, 0x00, 0x1F, 0x01, 0x00, 0x03, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0xFF, 0xC4, 0x00, 0xB5, 0x11, 0x00, 0x02, 0x01,
    0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02,
    0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62, 0x72,
    0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29,
    0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73,
    0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A,
    0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8,
    0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6,
    0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4,
    0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFF,
    0xDB, 0x00, 0x43, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x06, 0x04, 0x04, 0x06, 0x09, 0x06,
    0x06, 0x06, 0x09, 0x0C, 0x09, 0x09, 0x09, 0x09, 0x0C, 0x0F, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0F,
    0x12, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x15, 0x19, 0x19, 0x19, 0x19, 0x19, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C, 0x1C,
    0x1C, 0x1C, 0x1C, 0x1C, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x04, 0x05, 0x05, 0x07, 0x07, 0x07, 0x0C,
    0x07, 0x07, 0x0C, 0x1D, 0x14, 0x10, 0x14, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D,
    0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D,
    0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D,
    0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0xFF, 0xDD, 0x00, 0x04, 0x00, 0x01, 0xFF,
    0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00, 0xF0, 0x3A, 0x28,
    0xA2, 0xBF, 0xAB, 0x8F, 0xCF, 0x4F, 0xFF, 0xD9,
    };
    return bytes;
}

// Common round trip: assemble, reopen, return the reopened document.
struct RoundTrip {
    Assembled assembled;
    std::unique_ptr<PdfDocument> reopened;
    std::shared_ptr<const PdfPageContent> content; // page 0 of the reopened file
};

RoundTrip roundTrip(PdfEngine& engine, const PdfDocument& document, const PdfPageContentEditsPtr& edits,
                    std::size_t page = 0) {
    RoundTrip out;
    out.assembled = assemble(engine, document, page, edits);
    CHECK(out.assembled.status.has_value());
    if (!out.assembled.status.has_value()) return out;
    out.reopened = openSink(engine, out.assembled.sink);
    if (out.reopened) out.content = contentOf(*out.reopened, page);
    return out;
}

} // namespace

RIVET_TEST(contentDeleteRemovesObjectAndReportsOrigins) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    Edits edits;
    edits.remove(0);

    const auto preview = contentOf(*document, 0, edits.ptr());
    CHECK(preview != nullptr);
    if (preview) {
        CHECK_EQ(preview->objects.size(), std::size_t{3});
        if (preview->objects.size() == 3) {
            CHECK(preview->objects[0].origin == sourceOrigin(1));
            CHECK(preview->objects[1].origin == sourceOrigin(2));
            CHECK(preview->objects[2].origin == sourceOrigin(3));
        }
    }

    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content) return;
    CHECK_EQ(saved.assembled.report.size(), std::size_t{1});
    if (saved.assembled.report.size() == 1) {
        const std::vector<PdfContentOrigin> expected = {sourceOrigin(1), sourceOrigin(2), sourceOrigin(3)};
        CHECK(saved.assembled.report[0].origins == expected);
        CHECK_EQ(saved.assembled.report[0].blockTags.size(), std::size_t{3});
    }
    CHECK_EQ(saved.content->objects.size(), std::size_t{3});
    if (saved.content->objects.size() == 3) {
        CHECK(saved.content->objects[0].type == PdfContentObjectType::Text);
        CHECK(saved.content->objects[1].type == PdfContentObjectType::Image);
        CHECK(saved.content->objects[2].type == PdfContentObjectType::Path);
    }

    // The red rectangle is gone in both the preview and the saved file, and
    // the two renders agree (what is shown is what is written).
    const core::Bitmap original = renderFull(*document, 0, nullptr);
    const core::Bitmap preview1 = renderFull(*document, 0, edits.ptr());
    const core::Bitmap written = renderFull(*saved.reopened, 0, nullptr);
    CHECK(near(pixelAt(original, 50, dispY(40)), 255, 0, 0));
    CHECK(near(pixelAt(preview1, 50, dispY(40)), 255, 255, 255));
    CHECK(near(pixelAt(written, 50, dispY(40)), 255, 255, 255));
    CHECK_LT(differingFraction(preview1, written), 0.001);
}

RIVET_TEST(contentMoveAndResizeTransformObjects) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    Edits edits;
    edits.transform(2, core::Matrix::translation(-100.0, 0.0));
    edits.transform(3, core::Matrix::translation(120.0, 20.0) * core::Matrix::scaling(0.5, 0.5) *
                           core::Matrix::translation(-120.0, -20.0)); // about (120,20)
    // The line (100,100)-(150,120) is object 3: scaled about (120,20).
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 4) {
        CHECK(false);
        return;
    }
    const PdfContentObject& image = saved.content->objects[2];
    CHECK(image.type == PdfContentObjectType::Image);
    CHECK_NEAR(image.bounds.left, 20.0, 0.5);
    CHECK_NEAR(image.bounds.right, 70.0, 0.5);
    CHECK_NEAR(image.bounds.bottom, 20.0, 0.5);
    CHECK_NEAR(image.bounds.top, 70.0, 0.5);
    const PdfContentObject& line = saved.content->objects[3];
    // x: 120 + (100-120)/2 .. 120 + (150-120)/2 = 110..135; y: 20 + (100-20)/2 .. = 60..70
    CHECK_NEAR(line.bounds.left, 110.0, 1.5);
    CHECK_NEAR(line.bounds.right, 135.0, 1.5);
    CHECK_NEAR(line.bounds.bottom, 60.0, 1.5);
    CHECK_NEAR(line.bounds.top, 70.0, 1.5);

    const core::Bitmap preview = renderFull(*document, 0, edits.ptr());
    const core::Bitmap written = renderFull(*saved.reopened, 0, nullptr);
    CHECK_LT(differingFraction(preview, written), 0.001);
    // The image moved: its old place is empty, its new place is not.
    CHECK(near(pixelAt(written, 145, dispY(45)), 255, 255, 255));
    CHECK(!near(pixelAt(written, 45, dispY(45)), 255, 255, 255));
}

RIVET_TEST(contentReplaceImageBgraFitsFrame) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    Edits edits;
    edits.replaceImage(2, solidBgra(4, 2, 0, 200, 0)); // 2:1 into a square frame: centered, full width
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 4) {
        CHECK(false);
        return;
    }
    const PdfContentObject& image = saved.content->objects[2];
    CHECK(image.type == PdfContentObjectType::Image);
    CHECK_EQ(image.pixelWidth, std::uint32_t{4});
    CHECK_EQ(image.pixelHeight, std::uint32_t{2});
    // Aspect kept, centered in the old frame (120..170 x 20..70): 50x25.
    CHECK_NEAR(image.bounds.left, 120.0, 0.6);
    CHECK_NEAR(image.bounds.right, 170.0, 0.6);
    CHECK_NEAR(image.bounds.top - image.bounds.bottom, 25.0, 0.6);
    CHECK_NEAR((image.bounds.top + image.bounds.bottom) / 2.0, 45.0, 0.6);
    const core::Bitmap written = renderFull(*saved.reopened, 0, nullptr);
    CHECK(near(pixelAt(written, 145, dispY(45)), 0, 200, 0));
    const core::Bitmap preview = renderFull(*document, 0, edits.ptr());
    CHECK_LT(differingFraction(preview, written), 0.001);
    // The original document still shows its own image.
    const core::Bitmap original = renderFull(*document, 0, nullptr);
    CHECK(!near(pixelAt(original, 145, dispY(45)), 0, 200, 0));
}

RIVET_TEST(contentReplaceImageJpegIsPassedThrough) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    auto jpeg = std::make_shared<rivet::pdf::PdfImageData>();
    jpeg->format = rivet::pdf::PdfImageData::Format::Jpeg;
    jpeg->width = 8;
    jpeg->height = 8;
    jpeg->bytes = blueJpeg();
    Edits edits;
    edits.replaceImage(2, jpeg);
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 4) {
        CHECK(false);
        return;
    }
    CHECK_EQ(saved.content->objects[2].pixelWidth, std::uint32_t{8});
    const core::Bitmap written = renderFull(*saved.reopened, 0, nullptr);
    CHECK(near(pixelAt(written, 145, dispY(45)), 20, 60, 220, 60));
    // The raw DCT stream is carried as is.
    const std::string saveText(saved.assembled.sink.bytes().begin(), saved.assembled.sink.bytes().end());
    CHECK(saveText.find("DCTDecode") != std::string::npos);
}

RIVET_TEST(contentTextEditInPlaceKeepsZOrderAndFont) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    PdfTextBlockEdit block = textBlock(7, "Howdy", fromObject(1), core::Matrix::translation(20.0, 150.0));
    block.members = {1};
    Edits edits;
    edits.text(block);

    const auto preview = contentOf(*document, 0, edits.ptr());
    if (!preview || preview->objects.size() != 4) {
        CHECK(false);
        return;
    }
    CHECK(preview->objects[1].origin == sourceOrigin(1));
    CHECK_EQ(preview->objects[1].blockTag, std::uint64_t{7});
    CHECK_EQ(preview->objects[1].text, "Howdy");
    CHECK(!preview->objects[1].fontSubstituted);

    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 4) {
        CHECK(false);
        return;
    }
    CHECK(saved.content->objects[0].type == PdfContentObjectType::Path);
    CHECK(saved.content->objects[1].type == PdfContentObjectType::Text);
    CHECK(saved.content->objects[2].type == PdfContentObjectType::Image);
    CHECK_EQ(saved.content->objects[1].text, "Howdy");
    CHECK(saved.content->objects[1].font.baseName.find("Helvetica") != std::string::npos);
    CHECK_NEAR(saved.content->objects[1].fontSize, 18.0, 0.01);
    CHECK_NEAR(saved.content->objects[1].matrix.tx, 20.0, 0.01);
    CHECK_NEAR(saved.content->objects[1].matrix.ty, 150.0, 0.01);
    if (saved.assembled.report.size() == 1 && saved.assembled.report[0].blockTags.size() == 4) {
        CHECK_EQ(saved.assembled.report[0].blockTags[1], std::uint64_t{7});
        CHECK(saved.assembled.report[0].origins[1] == sourceOrigin(1));
    }
    const core::Bitmap a = renderFull(*document, 0, edits.ptr());
    const core::Bitmap b = renderFull(*saved.reopened, 0, nullptr);
    CHECK_LT(differingFraction(a, b), 0.001);

    // Text API sees the edited text.
    const PdfPageView view = nativeView(*document, 0);
    const auto textPage = document->textPage(0, view, edits.ptr());
    CHECK(textPage.has_value());
    if (textPage.has_value()) {
        CHECK((*textPage)->text().find("Howdy") != std::string::npos);
        CHECK((*textPage)->text().find("Hello") == std::string::npos);
    }
}

RIVET_TEST(contentTextEditWithUnencodableCharactersSubstitutesTheBundledFont) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const std::string cyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82"; // Привет
    PdfTextBlockEdit block =
        textBlock(3, cyrillic + " ok", fromObject(1, PdfBundledFont::SerifRegular), core::Matrix::translation(20.0, 150.0));
    block.members = {1};
    Edits edits;
    edits.text(block);
    // The Helvetica object cannot encode the text, so the whole block uses the
    // bundled font: PDFium cannot re-font an object, so the member is removed
    // and the block's lines are appended on top (origin Created).
    const auto preview = contentOf(*document, 0, edits.ptr());
    if (!preview || preview->objects.size() != 4) {
        CHECK(false);
        return;
    }
    const PdfContentObject& created = preview->objects[3];
    CHECK(created.origin == createdOrigin(3));
    CHECK_EQ(created.blockTag, std::uint64_t{3});
    CHECK(created.fontSubstituted);
    CHECK_EQ(created.text, cyrillic + " ok");
    CHECK(created.font.baseName.find("Tinos") != std::string::npos);
    CHECK(preview->objects[0].origin == sourceOrigin(0));
    CHECK(preview->objects[1].origin == sourceOrigin(2));
    CHECK(preview->objects[2].origin == sourceOrigin(3));
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 4) {
        CHECK(false);
        return;
    }
    CHECK_EQ(saved.content->objects[3].text, cyrillic + " ok");
    CHECK(saved.content->objects[3].font.embedded);
    CHECK(!saved.content->objects[3].textUnmappable);
    const core::Bitmap a = renderFull(*document, 0, edits.ptr());
    const core::Bitmap b = renderFull(*saved.reopened, 0, nullptr);
    CHECK_LT(differingFraction(a, b), 0.001);
}

RIVET_TEST(contentCheckEditsIsADryRunThatReportsApplyErrors) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const core::Bitmap before = renderFull(*document, 0, nullptr);
    const auto contentBefore = contentOf(*document, 0);
    if (!contentBefore) return;
    const std::size_t objectsBefore = contentBefore->objects.size();

    const std::string hebrew = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D"; // shalom
    const core::Matrix where = core::Matrix::translation(20.0, 150.0);

    // No bundled face has Hebrew: the apply error is reported.
    PdfTextBlockEdit uncovered = textBlock(5, hebrew, bundled(PdfBundledFont::SansRegular), where);
    Edits bad;
    bad.text(uncovered);
    const core::Status refused = document->checkContentEdits(0, *bad.value);
    CHECK(!refused.has_value());
    if (!refused.has_value()) CHECK(refused.error().code == core::ErrorCode::InvalidArgument);

    // The block's own (Helvetica) font cannot write it either and the
    // bundled fallback has no Hebrew: same error.
    PdfTextBlockEdit ownUncovered = textBlock(6, hebrew, fromObject(1), where);
    ownUncovered.members = {1};
    Edits badOwn;
    badOwn.text(ownUncovered);
    const core::Status refusedOwn = document->checkContentEdits(0, *badOwn.value);
    CHECK(!refusedOwn.has_value());
    if (!refusedOwn.has_value()) CHECK(refusedOwn.error().code == core::ErrorCode::InvalidArgument);

    // Ordinary Latin text is accepted.
    PdfTextBlockEdit covered = textBlock(7, "Hello again", bundled(PdfBundledFont::SansRegular), where);
    Edits good;
    good.text(covered);
    CHECK(document->checkContentEdits(0, *good.value).has_value());

    // Out-of-range page index is refused.
    CHECK(!document->checkContentEdits(99, *good.value).has_value());

    // The dry run changed nothing: same objects, same render.
    const auto contentAfter = contentOf(*document, 0);
    if (contentAfter) CHECK_EQ(contentAfter->objects.size(), objectsBefore);
    const core::Bitmap after = renderFull(*document, 0, nullptr);
    CHECK_EQ(differingFraction(before, after, 0), 0.0);
}

RIVET_TEST(contentAddTextUsesEveryBundledFaceAndAppendsOnTop) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const std::string line2 = "\xD0\xA1\xD1\x82\xD1\x80\xD0\xBE\xD0\xBA\xD0\xB0 two"; // Строка two
    const PdfBundledFont faces[] = {PdfBundledFont::SansRegular, PdfBundledFont::SansBold,
                                    PdfBundledFont::SerifRegular, PdfBundledFont::SerifBold,
                                    PdfBundledFont::MonoRegular, PdfBundledFont::MonoBold};
    for (const PdfBundledFont face : faces) {
        PdfTextBlockEdit block =
            textBlock(9, "Line one \xCE\xB1\n" + line2, bundled(face), core::Matrix::translation(30.0, 100.0), 14.0);
        block.color = rivet::pdf::PdfColor{0.0F, 0.5F, 0.0F};
        Edits edits;
        edits.text(block);
        const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
        if (!saved.content || saved.content->objects.size() != 6) {
            CHECK(false);
            continue;
        }
        const PdfContentObject& first = saved.content->objects[4];
        const PdfContentObject& second = saved.content->objects[5];
        CHECK(first.type == PdfContentObjectType::Text);
        CHECK(second.type == PdfContentObjectType::Text);
        CHECK_EQ(first.text, "Line one \xCE\xB1");
        CHECK_EQ(second.text, line2);
        CHECK_NEAR(first.matrix.tx, 30.0, 0.01);
        CHECK_NEAR(first.matrix.ty, 100.0, 0.01);
        CHECK_NEAR(second.matrix.ty, 100.0 - 14.0 * 1.2, 0.01);
        CHECK_NEAR(first.fontSize, 14.0, 0.01);
        CHECK(first.font.embedded);
        CHECK(first.fill.has_value());
        if (first.fill.has_value()) {
            CHECK_NEAR(static_cast<double>(first.fill->g), 0.5, 0.01);
        }
        if (saved.assembled.report.size() == 1) {
            const auto& report = saved.assembled.report[0];
            CHECK_EQ(report.origins.size(), std::size_t{6});
            if (report.origins.size() == 6) {
                CHECK(report.origins[3] == sourceOrigin(3));
                CHECK(report.origins[4] == createdOrigin(9));
                CHECK(report.origins[5] == createdOrigin(9));
                CHECK_EQ(report.blockTags[4], std::uint64_t{9});
                CHECK_EQ(report.blockTags[0], std::uint64_t{0});
            }
        }
        const core::Bitmap a = renderFull(*document, 0, edits.ptr());
        const core::Bitmap b = renderFull(*saved.reopened, 0, nullptr);
        CHECK_LT(differingFraction(a, b), 0.001);
    }
}

RIVET_TEST(contentAddTextWrapsAtTheWrapWidth) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    PdfTextBlockEdit block = textBlock(5, "alpha beta gamma delta epsilon zeta",
                                       bundled(PdfBundledFont::SansRegular), core::Matrix::translation(20.0, 120.0), 14.0);
    block.wrapWidth = 70.0;
    Edits edits;
    edits.text(block);
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content) return;
    CHECK_GE(saved.content->objects.size(), std::size_t{4 + 3});
    for (std::size_t i = 4; i < saved.content->objects.size(); ++i) {
        CHECK_LE(saved.content->objects[i].bounds.right, 20.0 + 70.0 + 0.5);
        CHECK(!saved.content->objects[i].text.empty());
    }
}

// PDFium writes no serif/fixed-pitch descriptor flags for fonts it loads from
// bytes: the bundled faces must still classify by family after a round trip
// (the properties bar and the retype coverage check depend on it).
RIVET_TEST(contentBundledFontsKeepTheirFamilyAfterARoundTrip) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    Edits edits;
    edits.text(textBlock(5, "Serif", bundled(PdfBundledFont::SerifRegular), core::Matrix::translation(20.0, 120.0)));
    edits.text(textBlock(6, "Mono", bundled(PdfBundledFont::MonoBold), core::Matrix::translation(20.0, 100.0)));
    edits.text(textBlock(7, "Sans", bundled(PdfBundledFont::SansRegular), core::Matrix::translation(20.0, 80.0)));
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content) return;
    int seen = 0;
    for (const PdfContentObject& object : saved.content->objects) {
        if (object.text == "Serif") {
            ++seen;
            CHECK(object.font.serif);
            CHECK(!object.font.monospace);
            CHECK(!object.font.bold);
        } else if (object.text == "Mono") {
            ++seen;
            CHECK(object.font.monospace);
            CHECK(!object.font.serif);
            CHECK(object.font.bold);
        } else if (object.text == "Sans") {
            ++seen;
            CHECK(!object.font.serif);
            CHECK(!object.font.monospace);
        }
    }
    CHECK_EQ(seen, 3);
}

RIVET_TEST(contentAddRotatedTextKeepsTheRotation) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const double pi = 3.14159265358979323846;
    const core::Matrix placement = core::Matrix::rotation(pi / 2.0); // text runs upwards
    core::Matrix withTranslation = placement;
    withTranslation.tx = 180.0;
    withTranslation.ty = 40.0;
    PdfTextBlockEdit block = textBlock(2, "Vertical", bundled(PdfBundledFont::SansBold), withTranslation, 16.0);
    Edits edits;
    edits.text(block);
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 5) {
        CHECK(false);
        return;
    }
    const PdfContentObject& text = saved.content->objects[4];
    CHECK_GT(text.bounds.top - text.bounds.bottom, text.bounds.right - text.bounds.left);
    CHECK_NEAR(text.matrix.tx, 180.0, 0.01);
    CHECK_NEAR(text.matrix.ty, 40.0, 0.01);
    CHECK_NEAR(text.matrix.a, 0.0, 1e-6);
    CHECK_NEAR(text.matrix.b, 1.0, 1e-3);
    CHECK_NEAR(text.fontSize, 16.0, 0.01);
}

RIVET_TEST(contentLiveDocumentIsNeverMutated) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const core::Bitmap before = renderFull(*document, 0, nullptr);
    Edits edits;
    edits.remove(0);
    edits.transform(2, core::Matrix::translation(-50.0, 0.0));
    PdfTextBlockEdit block = textBlock(4, "New", bundled(PdfBundledFont::SerifRegular), core::Matrix::translation(10.0, 10.0));
    edits.text(block);
    (void)renderFull(*document, 0, edits.ptr());
    (void)contentOf(*document, 0, edits.ptr());
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    CHECK(saved.content != nullptr);
    const core::Bitmap after = renderFull(*document, 0, nullptr);
    CHECK_EQ(differingFraction(before, after, 0), 0.0);
    const auto content = contentOf(*document, 0);
    if (content) CHECK_EQ(content->objects.size(), std::size_t{4});
    // Saving the untouched document again still yields the original content.
    const RoundTrip plain = roundTrip(*engine, *document, nullptr);
    if (plain.content) CHECK_EQ(plain.content->objects.size(), std::size_t{4});
}

// --- Pages, fonts, geometry and safety ------------------------------------------

namespace {

struct PagePlan {
    std::size_t sourcePage = 0;
    PdfPageContentEditsPtr edits;
};

Assembled assemblePlan(PdfEngine& engine, const PdfDocument& document, const std::vector<PagePlan>& plan,
                       PdfAssemblyRequest::Mode mode = PdfAssemblyRequest::Mode::PreserveBase) {
    Assembled out;
    PdfAssemblyRequest request;
    request.mode = mode;
    request.base = &document;
    for (const PagePlan& item : plan) {
        PdfAssemblyPage entry;
        entry.source = &document;
        entry.sourcePageIndex = item.sourcePage;
        entry.view = nativeView(document, item.sourcePage);
        entry.contentEdits = item.edits;
        request.pages.push_back(std::move(entry));
    }
    out.status = engine.assembleDocument(request, out.sink, nullptr, &out.report);
    return out;
}

// Two pages sharing ONE content stream (object 4), font and image.
std::string sharedStreamPdf() {
    const std::string content =
        "1 0 0 rg 20 20 60 40 re f\n"
        "BT /F1 18 Tf 20 150 Td (Hello World) Tj ET\n"
        "q 50 0 0 50 120 20 cm /Im1 Do Q\n"
        "0 0 1 RG 2 w 100 100 m 150 120 l S\n";
    const std::string page = " /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R /Resources << " +
                             std::string(kFonts) + ">> ";
    return buildPdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 8 0 R] /Count 2 >>",
        "<<" + page + ">>",
        streamObject("", content),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        streamObject("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB "
                     "/BitsPerComponent 8 /Filter /ASCIIHexDecode",
                     "FF0000 00FF00 0000FF FFFF00>"),
        streamObject("/Type /XObject /Subtype /Form /BBox [0 0 30 30]", "0 1 0 rg 0 0 30 30 re f"),
        "<<" + page + ">>",
    });
}

std::string probeFixture(const std::string& content, const std::string& resources) {
    return simplePdf("/MediaBox [0 0 200 200]", content, resources);
}

} // namespace

RIVET_TEST(contentSharedContentStreamIsNotEditedThroughTheOtherPage) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const std::string pdf = sharedStreamPdf();
    auto document = openText(*engine, pdf);
    if (!document) return;
    CHECK_EQ(document->info().pageCount, std::size_t{2});
    const core::Bitmap original0 = renderFull(*document, 0, nullptr);
    Edits remove;
    remove.remove(0);

    // Edit the first page, carry the second one over.
    {
        const Assembled saved = assemblePlan(*engine, *document, {{0, remove.ptr()}, {1, nullptr}});
        CHECK(saved.status.has_value());
        if (!saved.status.has_value()) return;
        auto reopened = openSink(*engine, saved.sink);
        if (!reopened) return;
        const auto page0 = contentOf(*reopened, 0);
        const auto page1 = contentOf(*reopened, 1);
        if (!page0 || !page1) return;
        CHECK_EQ(page0->objects.size(), std::size_t{3});
        CHECK_EQ(page1->objects.size(), std::size_t{4});
        const core::Bitmap second = renderFull(*reopened, 1, nullptr);
        CHECK_LT(differingFraction(original0, second), 0.001);
    }
    // Edit the second page; the first stays.
    {
        const Assembled saved = assemblePlan(*engine, *document, {{0, nullptr}, {1, remove.ptr()}});
        CHECK(saved.status.has_value());
        if (!saved.status.has_value()) return;
        auto reopened = openSink(*engine, saved.sink);
        if (!reopened) return;
        const auto page0 = contentOf(*reopened, 0);
        const auto page1 = contentOf(*reopened, 1);
        if (!page0 || !page1) return;
        CHECK_EQ(page0->objects.size(), std::size_t{4});
        CHECK_EQ(page1->objects.size(), std::size_t{3});
    }
    // The same source page twice with different edits, in both orders.
    Edits move;
    move.transform(2, core::Matrix::translation(-100.0, 0.0));
    for (const bool editedFirst : {true, false}) {
        const std::vector<PagePlan> plan = editedFirst
                                               ? std::vector<PagePlan>{{0, remove.ptr()}, {0, move.ptr()}, {0, nullptr}}
                                               : std::vector<PagePlan>{{0, nullptr}, {0, move.ptr()}, {0, remove.ptr()}};
        const Assembled saved = assemblePlan(*engine, *document, plan);
        CHECK(saved.status.has_value());
        if (!saved.status.has_value()) return;
        auto reopened = openSink(*engine, saved.sink);
        if (!reopened) return;
        CHECK_EQ(reopened->info().pageCount, std::size_t{3});
        const std::size_t removedAt = editedFirst ? 0 : 2;
        for (std::size_t i = 0; i < 3; ++i) {
            const auto content = contentOf(*reopened, i);
            if (!content) return;
            CHECK_EQ(content->objects.size(), i == removedAt ? std::size_t{3} : std::size_t{4});
            if (i == 1 && content->objects.size() == 4) {
                CHECK_NEAR(content->objects[2].bounds.left, 20.0, 0.5); // moved
            }
            if (i != 1 && content->objects.size() >= 3) {
                const PdfContentObject& image = content->objects[removedAt == i ? 1 : 2];
                CHECK_NEAR(image.bounds.left, 120.0, 0.5); // untouched
            }
        }
    }
    // Fresh mode (Extract) with content edits.
    {
        const Assembled saved = assemblePlan(*engine, *document, {{1, remove.ptr()}}, PdfAssemblyRequest::Mode::Fresh);
        CHECK(saved.status.has_value());
        if (!saved.status.has_value()) return;
        CHECK_EQ(saved.report.size(), std::size_t{1});
        auto reopened = openSink(*engine, saved.sink);
        if (!reopened) return;
        const auto content = contentOf(*reopened, 0);
        if (content) CHECK_EQ(content->objects.size(), std::size_t{3});
    }
    // The live document is unchanged.
    const core::Bitmap after = renderFull(*document, 0, nullptr);
    CHECK_EQ(differingFraction(original0, after, 0), 0.0);
}

RIVET_TEST(contentMultiStreamPageEditsAcrossStreams) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const std::string pdf = buildPdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents [4 0 R 7 0 R] /Resources << " +
            std::string(kFonts) + ">> >>",
        streamObject("", "1 0 0 rg 20 20 60 40 re f\nBT /F1 18 Tf 20 150 Td (Hello World) Tj ET\n"),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        streamObject("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB "
                     "/BitsPerComponent 8 /Filter /ASCIIHexDecode",
                     "FF0000 00FF00 0000FF FFFF00>"),
        streamObject("", "q 50 0 0 50 120 20 cm /Im1 Do Q\n0 0 1 RG 2 w 100 100 m 150 120 l S\n"),
    });
    auto document = openText(*engine, pdf);
    if (!document) return;
    const auto before = contentOf(*document, 0);
    if (!before) return;
    CHECK(before->regenerationSafe);
    CHECK_EQ(before->objects.size(), std::size_t{4});
    Edits edits;
    edits.remove(0);
    edits.transform(3, core::Matrix::translation(0.0, 30.0));
    PdfTextBlockEdit block = textBlock(1, "Edited", fromObject(1), core::Matrix::translation(20.0, 150.0));
    block.members = {1};
    edits.text(block);
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 3) {
        CHECK(false);
        return;
    }
    CHECK_EQ(saved.content->objects[0].text, "Edited");
    CHECK(saved.content->objects[1].type == PdfContentObjectType::Image);
    // The stroked line (130..150) plus PDFium's stroke outset.
    CHECK_NEAR(saved.content->objects[2].bounds.bottom, 129.0, 3.5);
    CHECK_GT(saved.content->objects[2].bounds.bottom, 120.0);
    const core::Bitmap a = renderFull(*document, 0, edits.ptr());
    const core::Bitmap b = renderFull(*saved.reopened, 0, nullptr);
    CHECK_LT(differingFraction(a, b), 0.001);
}

RIVET_TEST(contentRotatedCroppedPageUsesSourceUserSpace) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf("/MediaBox [0 0 300 300] /CropBox [50 50 250 250] /Rotate 90"));
    if (!document) return;
    const PdfPageView view = nativeView(*document, 0);
    CHECK(view.rotation == core::PageRotation::Clockwise90);
    PdfTextBlockEdit block = textBlock(1, "Rotated page", bundled(PdfBundledFont::SansBold),
                                       core::Matrix::translation(100.0, 100.0), 20.0);
    Edits edits;
    edits.text(block);
    edits.transform(0, core::Matrix::translation(100.0, 0.0));
    // transform of object 0 and a text block: objects must come first in vector order only.
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content || saved.content->objects.size() != 5) {
        CHECK(false);
        return;
    }
    const PdfContentObject& text = saved.content->objects[4];
    CHECK_NEAR(text.matrix.tx, 100.0, 0.01);
    CHECK_NEAR(text.matrix.ty, 100.0, 0.01);
    CHECK_NEAR(saved.content->objects[0].bounds.left, 120.0, 0.5);
    // The saved page keeps its rotation and crop box, and preview == file.
    const PdfPageView savedView = nativeView(*saved.reopened, 0);
    CHECK(savedView == view);
    const core::Bitmap plain = renderFull(*document, 0, nullptr);
    const core::Bitmap preview = renderFull(*document, 0, edits.ptr());
    const core::Bitmap written = renderFull(*saved.reopened, 0, nullptr);
    CHECK_GT(differingFraction(plain, preview), 0.001);
    CHECK_LT(differingFraction(preview, written), 0.001);
}

RIVET_TEST(contentMovingAClippedObjectMovesItsClip) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf("/MediaBox [0 0 200 200]",
                                               "q 0 0 100 100 re W n 1 0 0 rg 20 20 200 200 re f Q\n"));
    if (!document) return;
    const auto before = contentOf(*document, 0);
    if (!before || before->objects.size() != 1) {
        CHECK(false);
        return;
    }
    CHECK(before->objects[0].hasClip);
    Edits edits;
    edits.transform(0, core::Matrix::translation(50.0, 0.0));
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content) return;
    const core::Bitmap written = renderFull(*saved.reopened, 0, nullptr);
    CHECK(near(pixelAt(written, 60, dispY(50)), 255, 255, 255));  // left of the moved path
    CHECK(near(pixelAt(written, 120, dispY(50)), 255, 0, 0));      // inside the moved clip
    CHECK(near(pixelAt(written, 160, dispY(50)), 255, 255, 255)); // right of the moved clip
    const core::Bitmap preview = renderFull(*document, 0, edits.ptr());
    CHECK_LT(differingFraction(preview, written), 0.001);
}

RIVET_TEST(contentUnknownFontFallsBackOrKeepsTheText) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    std::string widths;
    for (int i = 32; i <= 126; ++i) widths += "520 ";
    auto document = openText(
        *engine, simplePdf("/MediaBox [0 0 200 200]", "BT /F1 18 Tf 20 150 Td (Hello World) Tj ET\n", "",
                           "<< /Type /Font /Subtype /Type1 /BaseFont /NoSuchFontXyz /FirstChar 32 /LastChar 126 "
                           "/Widths [" + widths + "] /Encoding /WinAnsiEncoding >>"));
    if (!document) return;
    PdfTextBlockEdit block = textBlock(1, "Hi there", fromObject(0, PdfBundledFont::SerifRegular),
                                       core::Matrix::translation(20.0, 150.0));
    block.members = {0};
    Edits edits;
    edits.text(block);
    const RoundTrip saved = roundTrip(*engine, *document, edits.ptr());
    if (!saved.content) return;
    // Whatever font was chosen, the text survives and is visible.
    std::string all;
    for (const PdfContentObject& object : saved.content->objects) all += object.text;
    CHECK_EQ(all, "Hi there");
    const core::Bitmap preview = renderFull(*document, 0, edits.ptr());
    const core::Bitmap written = renderFull(*saved.reopened, 0, nullptr);
    CHECK_LT(differingFraction(preview, written), 0.001);
}

RIVET_TEST(contentOtherAnnotationsAndLinksSurviveContentEdits) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    for (const char* name : {"annots.pdf", "external-link.pdf"}) {
        const fs::path path = fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / name;
        auto opened = engine->openDocument(path, {});
        CHECK(opened.has_value());
        if (!opened.has_value()) return;
        PdfDocument& document = **opened;
        const auto annotsBefore = document.annotations(0);
        const auto linksBefore = document.pageLinks(0);
        PdfTextBlockEdit block = textBlock(1, "Added", bundled(PdfBundledFont::SansRegular),
                                           core::Matrix::translation(40.0, 40.0), 14.0);
        Edits edits;
        edits.text(block);
        const RoundTrip saved = roundTrip(*engine, document, edits.ptr());
        if (!saved.reopened || !saved.content) {
            CHECK(false);
            return;
        }
        const auto sourceContent = contentOf(document, 0);
        if (sourceContent) CHECK_EQ(saved.content->objects.size(), sourceContent->objects.size() + 1);
        const auto annotsAfter = saved.reopened->annotations(0);
        CHECK_EQ(annotsBefore.has_value(), annotsAfter.has_value());
        if (annotsBefore.has_value() && annotsAfter.has_value()) {
            CHECK_EQ((*annotsAfter)->annotsCount, (*annotsBefore)->annotsCount);
            CHECK_EQ((*annotsAfter)->items.size(), (*annotsBefore)->items.size());
            for (std::size_t i = 0; i < (*annotsBefore)->items.size() && i < (*annotsAfter)->items.size(); ++i) {
                CHECK((*annotsAfter)->items[i].kind == (*annotsBefore)->items[i].kind);
            }
        }
        const auto linksAfter = saved.reopened->pageLinks(0);
        CHECK_EQ(linksBefore.has_value(), linksAfter.has_value());
        if (linksBefore.has_value() && linksAfter.has_value()) CHECK_EQ(linksAfter->size(), linksBefore->size());
    }
}

RIVET_TEST(contentInvalidEditsAreRejected) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf());
    if (!document) return;
    const auto expectRejected = [&](const Edits& edits) {
        const Assembled assembled = assemble(*engine, *document, 0, edits.ptr());
        CHECK(!assembled.status.has_value());
        if (!assembled.status.has_value()) {
            CHECK(assembled.status.error().code == core::ErrorCode::InvalidArgument);
        }
        CHECK(!document->pageContent(0, edits.ptr()).has_value());
    };
    {
        Edits edits; // out-of-range object
        edits.remove(99);
        expectRejected(edits);
    }
    {
        Edits edits; // image bomb: side beyond the limit
        auto bomb = std::make_shared<rivet::pdf::PdfImageData>();
        bomb->format = rivet::pdf::PdfImageData::Format::Bgra;
        bomb->width = 20000;
        bomb->height = 1;
        bomb->stride = 80000;
        bomb->bytes.assign(80000, 0);
        edits.replaceImage(2, bomb);
        expectRejected(edits);
    }
    {
        Edits edits; // replaceImage on a non-image object
        edits.replaceImage(0, solidBgra(2, 2, 1, 2, 3));
        expectRejected(edits);
    }
    {
        Edits edits; // characters no bundled font covers (CJK)
        edits.text(textBlock(1, "\xE4\xB8\xAD\xE6\x96\x87", bundled(PdfBundledFont::SansRegular),
                             core::Matrix::translation(10.0, 10.0)));
        expectRejected(edits);
    }
    {
        Edits edits; // ill-formed UTF-8
        edits.text(textBlock(1, "bad \xC3", bundled(PdfBundledFont::SansRegular), core::Matrix::translation(10.0, 10.0)));
        expectRejected(edits);
    }
    {
        Edits edits; // block member that is not a text object
        PdfTextBlockEdit block = textBlock(1, "x", bundled(PdfBundledFont::SansRegular), core::Matrix::translation(10.0, 10.0));
        block.members = {0};
        edits.text(block);
        expectRejected(edits);
    }
    // Nothing was written for any of the above and the document still works.
    const auto content = contentOf(*document, 0);
    if (content) CHECK_EQ(content->objects.size(), std::size_t{4});
}

RIVET_TEST(contentTooManyObjectsMakesThePageReadOnly) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    std::string content;
    for (std::size_t i = 0; i < rivet::pdf::kMaxContentObjectsPerPage + 5; ++i) content += "0 0 1 1 re f\n";
    auto document = openText(*engine, simplePdf("/MediaBox [0 0 200 200]", content));
    if (!document) return;
    const auto page = contentOf(*document, 0);
    if (!page) return;
    CHECK(page->truncated);
    CHECK(page->objects.empty());
    CHECK(!page->regenerationSafe);
    Edits edits;
    edits.remove(0);
    CHECK(!assemble(*engine, *document, 0, edits.ptr()).status.has_value());
}

RIVET_TEST(contentExtractionReportsFormsMarkedContentAndRenderModes) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, simplePdf("/MediaBox [0 0 200 200]",
                                               "q 1 0 0 1 100 100 cm /Fm1 Do Q\n"
                                               "/Span << /MCID 0 >> BDC BT /F1 12 Tf 10 10 Td (Tagged) Tj ET EMC\n"
                                               "BT /F1 12 Tf 1 Tr 10 50 Td (Outline) Tj ET\n"));
    if (!document) return;
    const auto content = contentOf(*document, 0);
    if (!content || content->objects.size() != 3) {
        CHECK(false);
        return;
    }
    CHECK(content->objects[0].type == PdfContentObjectType::Form);
    CHECK_GE(content->objects[0].childCount, std::uint32_t{1});
    CHECK(content->objects[1].hasMarkedContent);
    CHECK(!content->objects[0].hasMarkedContent);
    CHECK_EQ(content->objects[2].renderMode, 1);
}

RIVET_TEST(contentProbeAcceptsCommonContentAndRejectsLossyPages) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    struct Case {
        const char* name;
        std::string content;
        std::string resources;
        bool safe;
    };
    const std::vector<Case> cases = {
        {"gray-fill", "0.5 g 20 20 60 40 re f\n", "", true},
        {"gray-stroke", "0.5 G 3 w 20 20 m 100 100 l S\n", "", true},
        // PDFium's content generator writes only RGB colors: a CMYK color
        // would come back black, which the probe detects.
        {"cmyk-fill", "0 1 1 0 k 20 20 60 40 re f\n", "", false},
        {"cmyk-stroke", "0 1 1 0 K 3 w 20 20 m 100 100 l S\n", "", false},
        {"dashed-stroke", "[4 2] 0 d 2 w 10 10 m 150 150 l S\n", "", true},
        {"alpha", "/G1 gs 1 0 0 rg 20 20 100 100 re f\n", "/ExtGState << /G1 << /ca 0.5 /CA 0.5 >> >> ", true},
        // PDFium's generator does not write Tc: the glyph run would shrink.
        {"char-spacing", "BT /F1 18 Tf 3 Tc 20 100 Td (Spaced) Tj ET\n", "", false},
        {"horizontal-scaling", "BT /F1 18 Tf 150 Tz 20 100 Td (Wide) Tj ET\n", "", true},
        {"kerned-tj", "BT /F1 18 Tf 20 100 Td [(A) -200 (V) 100 (A)] TJ ET\n", "", true},
        {"text-rise", "BT /F1 18 Tf 5 Ts 20 100 Td (Rise) Tj ET\n", "", true},
        {"stroke-text", "BT /F1 18 Tf 1 Tr 20 100 Td (Stroke) Tj ET\n", "", true},
        {"invisible-text", "BT /F1 18 Tf 3 Tr 20 100 Td (Hidden) Tj ET\n", "", true},
        {"form", "q 1 0 0 1 100 100 cm /Fm1 Do Q\n", "", true},
        {"clip", "q 10 10 80 80 re W n 1 0 0 rg 0 0 200 200 re f Q\n", "", true},
        {"nested-q", "q q 2 0 0 2 0 0 cm 0 1 0 rg 10 10 20 20 re f Q Q\n", "", true},
        {"shading", "/Sh1 sh\n",
         "/Shading << /Sh1 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 100 0] /Function << "
         "/FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >> ",
         false},
        {"text-render-clip", "BT /F1 40 Tf 7 Tr 20 100 Td (Clip) Tj ET 1 0 0 rg 0 0 200 200 re f\n", "", false},
    };
    int mismatches = 0;
    for (const Case& item : cases) {
        auto document = openText(*engine, probeFixture(item.content, item.resources));
        if (!document) return;
        const auto content = contentOf(*document, 0);
        if (!content) return;
        if (content->regenerationSafe != item.safe) {
            std::fprintf(stderr, "    probe case '%s': safe=%d (%s)\n", item.name, content->regenerationSafe ? 1 : 0,
                         content->regenerationIssue.c_str());
        }
        mismatches += content->regenerationSafe != item.safe ? 1 : 0;
        CHECK_EQ(content->regenerationIssue.empty(), content->regenerationSafe);
    }
    CHECK_EQ(mismatches, 0);
}


