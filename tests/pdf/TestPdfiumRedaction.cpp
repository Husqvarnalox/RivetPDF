// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "PdfFixtures.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// True redaction through the content pipeline (ADR-0026), PDFium backend:
// glyph-level text removal, pixel-level image redaction, removal of
// intersecting vector objects, the fill boxes, the remove-metadata save and
// the probe refusal. PDFium-ON bodies only (every body returns early when no
// backend exists).

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
namespace pdffix = rivet::test::pdffix;
using rivet::pdf::PdfAssemblyPage;
using rivet::pdf::PdfAssemblyRequest;
using rivet::pdf::PdfBox;
using rivet::pdf::PdfContentObject;
using rivet::pdf::PdfContentObjectType;
using rivet::pdf::PdfContentOrigin;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfObjectEdit;
using rivet::pdf::PdfPageContent;
using rivet::pdf::PdfPageContentEdits;
using rivet::pdf::PdfPageContentEditsPtr;
using rivet::pdf::PdfPageView;
using rivet::pdf::PdfRedaction;

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
    if (!engine || !engine->isAvailable()) return nullptr;
    return engine;
}

std::unique_ptr<PdfDocument> openBytes(PdfEngine& engine, const void* data, std::size_t size) {
    static std::atomic<int> counter{0};
    const fs::path path = fs::temp_directory_path() /
                          ("rivet-redaction-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                           std::to_string(counter.fetch_add(1)) + ".pdf");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored); // the document keeps its own descriptor
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

std::unique_ptr<PdfDocument> openText(PdfEngine& engine, const std::string& pdf) {
    return openBytes(engine, pdf.data(), pdf.size());
}

PdfPageView nativeView(const PdfDocument& document, std::size_t page) {
    const auto info = document.pageInfo(page);
    CHECK(info.has_value());
    return info.has_value() ? info->view : PdfPageView{};
}

// 300 x 200 page, objects in z-order:
//   0 text "Secret Public" (Helvetica 20 at (20,150); "Secret" spans x 20..77.8)
//   1 image 2x2 in (150,20)-(250,120): top row red, green; bottom row blue, yellow
//   2 blue stroked line (20,40)-(120,40)
//   3 green square (260,150)-(280,170)
// Annotations: 0 link (20,90)-(60,110), 1 text field widget "secretfield"
// (70,90)-(130,110) with the value "Hidden". /Info carries a title and the
// catalog an XMP stream.
std::string redactionPdf() {
    using pdffix::buildPdf;
    using pdffix::streamObject;
    return buildPdf({
        "<< /Type /Catalog /Pages 2 0 R /Metadata 8 0 R /AcroForm << /Fields [10 0 R] "
        "/DR << /Font << /Helv 5 0 R >> >> /DA (/Helv 0 Tf 0 g) >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200] /Contents 4 0 R "
        "/Resources << /Font << /F1 5 0 R /Helv 5 0 R >> /XObject << /Im1 6 0 R >> >> /Annots [9 0 R 10 0 R] >>",
        streamObject("", "BT /F1 20 Tf 20 150 Td (Secret Public) Tj ET\n"
                         "q 100 0 0 100 150 20 cm /Im1 Do Q\n"
                         "0 0 1 RG 2 w 20 40 m 120 40 l S\n"
                         "0 1 0 rg 260 150 20 20 re f\n"),
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        streamObject("/Type /XObject /Subtype /Image /Width 2 /Height 2 /ColorSpace /DeviceRGB "
                     "/BitsPerComponent 8 /Filter /ASCIIHexDecode",
                     "FF0000 00FF00 0000FF FFFF00>"),
        "<< /Title (Confidential Title) /Author (Someone) >>",
        streamObject("/Type /Metadata /Subtype /XML", "<x:xmpmeta xmlns:x='adobe:ns:meta/'></x:xmpmeta>"),
        "<< /Type /Annot /Subtype /Link /Rect [20 90 60 110] /Border [0 0 0] "
        "/A << /S /URI /URI (https://example.com/) >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (secretfield) /V (Hidden) /DA (/Helv 10 Tf 0 g) "
        "/Rect [70 90 130 110] /F 4 /P 3 0 R >>",
    });
}

// Same as buildPdf but with an /Info entry in the trailer (object 7).
std::string withInfo(std::string pdf) {
    const std::string marker = "/Root 1 0 R >>";
    const std::size_t at = pdf.rfind(marker);
    CHECK(at != std::string::npos);
    if (at != std::string::npos) pdf.replace(at, marker.size(), "/Root 1 0 R /Info 7 0 R >>");
    return pdf;
}

const PdfBox kOverSecret{18.0, 145.0, 76.0, 175.0};
const PdfBox kOverImageCorner{140.0, 10.0, 190.0, 60.0};
const PdfBox kOverPathAndAnnots{10.0, 30.0, 140.0, 115.0};

PdfPageContentEditsPtr redactions(std::vector<PdfBox> areas) {
    auto edits = std::make_shared<PdfPageContentEdits>();
    for (const PdfBox& area : areas) edits->redactions.push_back(PdfRedaction{area, {}});
    return edits;
}

struct Saved {
    MemorySink sink;
    std::vector<rivet::pdf::PdfAssembledPageContent> report;
    core::Status status = core::ok();
};

Saved save(PdfEngine& engine, const PdfDocument& document, const PdfPageContentEditsPtr& edits,
           std::vector<std::uint32_t> removeAnnots = {}, bool removeMetadata = false) {
    Saved out;
    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = &document;
    request.removeMetadata = removeMetadata;
    PdfAssemblyPage page;
    page.source = &document;
    page.sourcePageIndex = 0;
    page.view = nativeView(document, 0);
    page.contentEdits = edits;
    if (!removeAnnots.empty()) {
        auto annotationEdits = std::make_shared<rivet::pdf::PdfPageAnnotationEdits>();
        annotationEdits->removeIndices = std::move(removeAnnots);
        page.annotationEdits = std::move(annotationEdits);
    }
    request.pages.push_back(std::move(page));
    out.status = engine.assembleDocument(request, out.sink, nullptr, &out.report);
    return out;
}

bool near(double a, double b, double tolerance = 1.0) { return std::fabs(a - b) <= tolerance; }

bool boxNear(const PdfBox& a, const PdfBox& b, double tolerance = 1.5) {
    return near(a.left, b.left, tolerance) && near(a.bottom, b.bottom, tolerance) &&
           near(a.right, b.right, tolerance) && near(a.top, b.top, tolerance);
}

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

Rgb pixelAt(const core::Bitmap& bitmap, int x, int y) {
    if (!bitmap.isValid() || x < 0 || y < 0 || x >= static_cast<int>(bitmap.width()) ||
        y >= static_cast<int>(bitmap.height())) {
        return {-1, -1, -1};
    }
    const auto* row = reinterpret_cast<const std::uint8_t*>(bitmap.data()) + static_cast<std::size_t>(y) * bitmap.stride();
    const std::uint8_t* px = row + static_cast<std::size_t>(x) * 4;
    return Rgb{px[2], px[1], px[0]};
}

bool rgbNear(const Rgb& c, int r, int g, int b, int tolerance = 40) {
    return std::abs(c.r - r) <= tolerance && std::abs(c.g - g) <= tolerance && std::abs(c.b - b) <= tolerance;
}

core::Bitmap render(PdfDocument& document, const PdfPageContentEditsPtr& edits) {
    const PdfPageView view = nativeView(document, 0);
    const core::Size size = rivet::pdf::displaySize(view);
    auto bitmap = document.renderPage(0, view, {}, edits, core::Rect(0.0, 0.0, size.width, size.height), 1.0);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return {};
    return std::move(*bitmap);
}

std::string pageText(const PdfDocument& document) {
    auto text = document.textPage(0);
    CHECK(text.has_value());
    return text.has_value() && *text != nullptr ? (*text)->text() : std::string{};
}

std::string trimmed(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

RIVET_TEST(redactionRemovesGlyphsPixelsPathsAndPaintsBoxesThroughSave) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, redactionPdf());
    if (!document) return;
    CHECK(pageText(*document).find("Secret") != std::string::npos);

    const PdfPageContentEditsPtr edits = redactions({kOverSecret, kOverImageCorner, kOverPathAndAnnots});
    const Saved saved = save(*engine, *document, edits, {0, 1});
    CHECK(saved.status.has_value());
    if (!saved.status.has_value()) return;
    auto reopened = openBytes(*engine, saved.sink.bytes().data(), saved.sink.bytes().size());
    if (!reopened) return;

    // Text: no character of "Secret" is extractable, "Public" still is.
    const std::string text = pageText(*reopened);
    CHECK(text.find("Public") != std::string::npos);
    CHECK(text.find("Secret") == std::string::npos);
    for (const char removed : {'S', 'e', 'r', 't'}) CHECK(text.find(removed) == std::string::npos);

    auto content = reopened->pageContent(0, nullptr);
    CHECK(content.has_value());
    if (!content.has_value()) return;
    const PdfPageContent& objects = **content;
    std::size_t texts = 0;
    std::size_t images = 0;
    std::vector<PdfBox> paths;
    for (const PdfContentObject& object : objects.objects) {
        switch (object.type) {
        case PdfContentObjectType::Text:
            ++texts;
            CHECK_EQ(trimmed(object.text), std::string("Public"));
            // The surviving run keeps its place on the baseline.
            CHECK(object.bounds.left > 76.0);
            CHECK(near(object.matrix.ty, 150.0, 0.5));
            break;
        case PdfContentObjectType::Image: ++images; break;
        case PdfContentObjectType::Path: paths.push_back(object.bounds); break;
        default: CHECK(false); break;
        }
    }
    CHECK_EQ(texts, std::size_t{1});
    CHECK_EQ(images, std::size_t{1});
    // The green square and the three fill boxes; the line is gone.
    CHECK_EQ(paths.size(), std::size_t{4});
    for (const PdfBox& area : {kOverSecret, kOverImageCorner, kOverPathAndAnnots}) {
        CHECK(std::any_of(paths.begin(), paths.end(), [&](const PdfBox& box) { return boxNear(box, area); }));
    }
    CHECK(std::none_of(paths.begin(), paths.end(),
                       [](const PdfBox& box) { return box.top < 45.0 && box.bottom > 35.0 && box.right > 100.0; }));
    // The fill boxes are the topmost objects, opaque black.
    const std::size_t count = objects.objects.size();
    CHECK(count >= 3);
    for (std::size_t i = count - 3; i < count; ++i) {
        const PdfContentObject& box = objects.objects[i];
        CHECK(box.type == PdfContentObjectType::Path);
        CHECK(box.fill.has_value());
        if (box.fill) CHECK(box.fill->r < 0.01F && box.fill->g < 0.01F && box.fill->b < 0.01F);
    }

    // Annotations (the link and the widget) are gone.
    auto annotations = reopened->annotations(0);
    CHECK(annotations.has_value());
    if (annotations.has_value()) CHECK_EQ((*annotations)->annotsCount, std::uint32_t{0});

    // Image pixels: with the fill boxes taken away again, the redacted
    // corner of the IMAGE itself is black; the other pixels are intact.
    auto unboxed = std::make_shared<PdfPageContentEdits>();
    for (std::size_t i = count - 3; i < count; ++i) {
        PdfObjectEdit remove;
        remove.sourceIndex = static_cast<std::uint32_t>(i);
        remove.remove = true;
        unboxed->objects.push_back(remove);
    }
    const core::Bitmap bitmap = render(*reopened, unboxed);
    CHECK(rgbNear(pixelAt(bitmap, 175, 200 - 45), 0, 0, 0));       // bottom-left (was blue)
    CHECK(rgbNear(pixelAt(bitmap, 225, 200 - 45), 255, 255, 0));   // bottom-right yellow
    CHECK(rgbNear(pixelAt(bitmap, 175, 200 - 95), 255, 0, 0));     // top-left red
    CHECK(rgbNear(pixelAt(bitmap, 225, 200 - 95), 0, 255, 0));     // top-right green
}

RIVET_TEST(redactionDisplayMatchesSaveAndReportsRedactionOrigins) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, redactionPdf());
    if (!document) return;
    const PdfPageContentEditsPtr edits = redactions({kOverSecret});
    auto content = document->pageContent(0, edits);
    CHECK(content.has_value());
    if (!content.has_value()) return;
    // Source objects 1..3 untouched, the text rewritten as one run, one box.
    std::size_t redactionObjects = 0;
    std::size_t sources = 0;
    for (const PdfContentObject& object : (*content)->objects) {
        if (object.origin.kind == PdfContentOrigin::Kind::Redaction) ++redactionObjects;
        if (object.origin.kind == PdfContentOrigin::Kind::Source) ++sources;
    }
    CHECK_EQ(redactionObjects, std::size_t{2});
    CHECK_EQ(sources, std::size_t{3});
    CHECK((*content)->objects.front().origin.kind == PdfContentOrigin::Kind::Redaction);
    CHECK_EQ(trimmed((*content)->objects.front().text), std::string("Public"));

    const Saved saved = save(*engine, *document, edits);
    CHECK(saved.status.has_value());
    CHECK_EQ(saved.report.size(), std::size_t{1});
    if (saved.report.size() == 1) CHECK_EQ(saved.report[0].origins.size(), (*content)->objects.size());
}

RIVET_TEST(redactionOfWholeTextObjectAndUntouchedPage) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, redactionPdf());
    if (!document) return;
    // An area covering the whole line removes the text object entirely.
    const Saved saved = save(*engine, *document, redactions({PdfBox{10.0, 140.0, 290.0, 180.0}}));
    CHECK(saved.status.has_value());
    if (!saved.status.has_value()) return;
    auto reopened = openBytes(*engine, saved.sink.bytes().data(), saved.sink.bytes().size());
    if (!reopened) return;
    CHECK(trimmed(pageText(*reopened)).empty());

    // An area over nothing only adds its box.
    const Saved empty = save(*engine, *document, redactions({PdfBox{200.0, 125.0, 210.0, 135.0}}));
    CHECK(empty.status.has_value());
    if (!empty.status.has_value()) return;
    auto again = openBytes(*engine, empty.sink.bytes().data(), empty.sink.bytes().size());
    if (!again) return;
    CHECK(pageText(*again).find("Secret Public") != std::string::npos);
}

RIVET_TEST(redactionIsRefusedOnPagesTheProbeRejects) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    using pdffix::buildPdf;
    using pdffix::streamObject;
    auto document = openText(
        *engine,
        buildPdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                  "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R /Resources << "
                  "/Shading << /Sh1 << /ShadingType 2 /ColorSpace /DeviceRGB /Coords [0 0 100 0] /Function << "
                  "/FunctionType 2 /Domain [0 1] /C0 [1 0 0] /C1 [0 0 1] /N 1 >> >> >> >> >>",
                  streamObject("", "1 0 0 rg 20 20 60 40 re f\n/Sh1 sh\n")}));
    if (!document) return;
    auto source = document->pageContent(0, nullptr);
    CHECK(source.has_value());
    if (source.has_value()) CHECK(!(*source)->regenerationSafe);
    const PdfPageContentEditsPtr edits = redactions({PdfBox{10.0, 10.0, 100.0, 100.0}});
    CHECK(!document->pageContent(0, edits).has_value());
    const Saved saved = save(*engine, *document, edits);
    CHECK(!saved.status.has_value());
}

RIVET_TEST(redactionSaveCanDropTheDocumentMetadata) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openText(*engine, withInfo(redactionPdf()));
    if (!document) return;
    CHECK_EQ(document->info().title, std::string("Confidential Title"));

    // Kept by default.
    const Saved kept = save(*engine, *document, redactions({kOverSecret}));
    CHECK(kept.status.has_value());
    if (kept.status.has_value()) {
        auto reopened = openBytes(*engine, kept.sink.bytes().data(), kept.sink.bytes().size());
        if (reopened) CHECK_EQ(reopened->info().title, std::string("Confidential Title"));
    }

    const Saved stripped = save(*engine, *document, redactions({kOverSecret}), {}, true);
    CHECK(stripped.status.has_value());
    if (!stripped.status.has_value()) return;
    const std::string bytes(stripped.sink.bytes().begin(), stripped.sink.bytes().end());
    CHECK(bytes.find("Confidential Title") == std::string::npos);
    CHECK(bytes.find("Someone") == std::string::npos);
    CHECK(bytes.find("xmpmeta") == std::string::npos);
    auto reopened = openBytes(*engine, stripped.sink.bytes().data(), stripped.sink.bytes().size());
    if (!reopened) return;
    CHECK(reopened->info().title.empty());
    CHECK_EQ(reopened->info().pageCount, std::size_t{1});
    // The redaction itself is in the rebuilt file too.
    CHECK(pageText(*reopened).find("Secret") == std::string::npos);
    CHECK(pageText(*reopened).find("Public") != std::string::npos);
}

RIVET_TEST(redactionValidationLimits) {
    PdfPageContentEdits edits;
    edits.redactions.push_back(PdfRedaction{PdfBox{0.0, 0.0, 10.0, 10.0}, {}});
    CHECK(rivet::pdf::validate(edits, 0).has_value());

    PdfPageContentEdits degenerate;
    degenerate.redactions.push_back(PdfRedaction{PdfBox{5.0, 0.0, 5.0, 10.0}, {}});
    CHECK(!rivet::pdf::validate(degenerate, 0).has_value());

    PdfPageContentEdits inverted;
    inverted.redactions.push_back(PdfRedaction{PdfBox{10.0, 0.0, 0.0, 10.0}, {}});
    CHECK(!rivet::pdf::validate(inverted, 0).has_value());

    PdfPageContentEdits infinite;
    infinite.redactions.push_back(PdfRedaction{PdfBox{0.0, 0.0, std::numeric_limits<double>::infinity(), 10.0}, {}});
    CHECK(!rivet::pdf::validate(infinite, 0).has_value());

    PdfPageContentEdits badColor;
    badColor.redactions.push_back(PdfRedaction{PdfBox{0.0, 0.0, 10.0, 10.0}, {2.0F, 0.0F, 0.0F}});
    CHECK(!rivet::pdf::validate(badColor, 0).has_value());

    PdfPageContentEdits many;
    many.redactions.assign(rivet::pdf::kMaxRedactionsPerPage, PdfRedaction{PdfBox{0.0, 0.0, 1.0, 1.0}, {}});
    CHECK(rivet::pdf::validate(many, 0).has_value());
    many.redactions.push_back(PdfRedaction{PdfBox{0.0, 0.0, 1.0, 1.0}, {}});
    CHECK(!rivet::pdf::validate(many, 0).has_value());

    CHECK(!edits.empty());
}
