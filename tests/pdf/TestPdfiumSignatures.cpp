// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "PdfFixtures.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSignatures.hpp"
#include "pdf/PdfSystem.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

// PDFium side of ADR-0025: visual signatures persist as /Stamp annotations
// with /Name RivetSignature and a generated /AP (ink, typed text, image) that
// survive save + reopen; PdfDocument::signatures() reads a hand-built
// signature dictionary and relates its /ByteRange to the file. PDFium-ON
// bodies only; every body returns early without a backend.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
namespace pdffix = rivet::test::pdffix;
using namespace rivet::pdf;

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return nullptr;
    return engine;
}

fs::path uniqueTempPath() {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("rivet-sigs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter++) + ".pdf");
}

std::unique_ptr<PdfDocument> openBytes(PdfEngine& engine, const std::string& bytes) {
    const fs::path path = uniqueTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored); // the document reads through its open descriptor
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

class MemorySink final : public IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        const auto* bytes = static_cast<const char*>(data);
        bytes_.append(bytes, size);
        return core::ok();
    }
    const std::string& bytes() const { return bytes_; }

private:
    std::string bytes_;
};

// One blank 300 x 300 page (display y = 300 - user y).
std::string blankPdf() {
    return pdffix::buildPdf({
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] >>",
    });
}

PdfPageView nativeView(const PdfDocument& document) {
    const auto info = document.pageInfo(0);
    CHECK(info.has_value());
    return info.has_value() ? info->view : PdfPageView{};
}

PdfAnnotationData signature(PdfSignatureAppearance appearance, PdfBox rect) {
    PdfAnnotationData data;
    data.kind = PdfAnnotationKind::Signature;
    data.rect = rect;
    data.color = PdfColor{0.0F, 0.0F, 0.0F};
    data.signature = std::make_shared<const PdfSignatureAppearance>(std::move(appearance));
    normalizeAnnotation(data);
    CHECK(isWritableAnnotation(data));
    return data;
}

struct Pixel {
    int b = 255, g = 255, r = 255, a = 255;
};

Pixel pixelAt(const core::Bitmap& bitmap, int x, int y) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(bitmap.data()) + static_cast<std::size_t>(y) * bitmap.stride() +
                    static_cast<std::size_t>(x) * 4;
    return Pixel{p[0], p[1], p[2], p[3]};
}

std::size_t darkPixels(const core::Bitmap& bitmap, int left, int top, int right, int bottom) {
    std::size_t count = 0;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const Pixel px = pixelAt(bitmap, x, y);
            if (px.r < 128 && px.g < 128 && px.b < 128) ++count;
        }
    }
    return count;
}

// A /Sig field whose /ByteRange describes the produced file: 0..'<' of
// /Contents and '>'+1..EOF. `trailing` is appended after %%EOF (an
// incremental update stand-in) and is NOT covered.
std::string signedPdf(const std::string& trailing) {
    const std::string placeholder = "[0 0000000000 0000000000 0000000000]";
    std::string pdf = pdffix::buildPdf({
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] /SigFlags 3 >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [4 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Signer1) /F 4 /Rect [40 40 200 90] /P 3 0 R /V 5 0 R >>",
        "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange " + placeholder +
            " /Contents <" + std::string(64, '0') +
            "> /Reason (Approval) /M (D:20240102030405+00'00') /Name (Signer Name) >>",
    });
    const std::size_t open = pdf.find("/Contents <") + std::string("/Contents ").size();
    const std::size_t close = pdf.find('>', open) + 1;
    const std::size_t total = pdf.size();
    const auto pad = [](std::size_t v) {
        std::string s = std::to_string(v);
        return std::string(10 - s.size(), '0') + s;
    };
    const std::string range = "[0 " + pad(open) + " " + pad(close) + " " + pad(total - close) + "]";
    pdf.replace(pdf.find(placeholder), placeholder.size(), range);
    return pdf + trailing;
}

} // namespace

RIVET_TEST(signatureAnnotationRoundTrip) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openBytes(*engine, blankPdf());
    if (!source) return;

    // Ink: a horizontal stroke through the middle of its box.
    const auto ink = makeInkSignature({{{0, 50}, {200, 50}}}, 4.0);
    CHECK(ink.has_value());
    if (!ink) return;
    // Text.
    PdfSignatureAppearance typed;
    typed.source = PdfSignatureSource::Text;
    typed.text = "Rivet";
    // Image: opaque red pixels.
    PdfSignatureAppearance image;
    image.source = PdfSignatureSource::Image;
    {
        auto created = core::Bitmap::create(20, 10);
        CHECK(created.has_value());
        if (!created) return;
        core::Bitmap pixels = std::move(*created);
        for (std::uint32_t y = 0; y < 10; ++y) {
            auto* row = reinterpret_cast<std::uint8_t*>(pixels.data()) + y * pixels.stride();
            for (std::uint32_t x = 0; x < 20; ++x) {
                row[4 * x + 0] = 0;   // B
                row[4 * x + 1] = 0;   // G
                row[4 * x + 2] = 255; // R
                row[4 * x + 3] = 255; // A
            }
        }
        image.image = std::make_shared<const core::Bitmap>(std::move(pixels));
    }

    auto edits = std::make_shared<PdfPageAnnotationEdits>();
    edits->create.push_back(signature(*ink, PdfBox{50, 220, 250, 280}));
    edits->create.push_back(signature(typed, PdfBox{50, 120, 250, 180}));
    edits->create.push_back(signature(image, PdfBox{50, 20, 250, 80}));

    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = source.get();
    request.pages.push_back(PdfAssemblyPage{source.get(), 0, nativeView(*source), edits});
    MemorySink sink;
    std::vector<PdfAssembledPageAnnotations> report;
    const auto status = engine->assembleDocument(request, sink, &report);
    CHECK(status.has_value());
    if (!status.has_value()) return;
    CHECK_EQ(report.size(), std::size_t{1});

    // The saved dictionaries: stamps named RivetSignature with an appearance.
    const std::string& bytes = sink.bytes();
    CHECK(bytes.find("/Stamp") != std::string::npos);
    CHECK(bytes.find("RivetSignature") != std::string::npos);
    CHECK(bytes.find("/AP") != std::string::npos);

    auto reloaded = openBytes(*engine, bytes);
    if (!reloaded) return;
    const auto read = reloaded->annotations(0);
    CHECK(read.has_value());
    if (!read.has_value()) return;
    const PdfPageAnnotations& page = **read;
    CHECK_EQ(page.items.size(), std::size_t{3});
    for (const PdfPageAnnotation& item : page.items) {
        CHECK(item.kind == PdfAnnotationKind::Signature);
        CHECK(!item.editable); // opaque after reopening: drawn from its /AP, preserved
    }

    // The appearances draw: the ink line, the glyphs and the red image.
    const auto bitmap = reloaded->renderPage(0, nativeView(*reloaded), std::span<const std::uint32_t>{},
                                             core::Rect{0, 0, 300, 300}, 1.0);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return;
    CHECK(darkPixels(*bitmap, 60, 45, 240, 55) > 100);   // ink at display y = 300 - 250
    CHECK(darkPixels(*bitmap, 50, 120, 250, 180) > 50);  // typed text
    const Pixel red = pixelAt(*bitmap, 150, 250);        // image at display y = 300 - 50
    CHECK(red.r > 200);
    CHECK(red.g < 60);
    CHECK(red.b < 60);
}

RIVET_TEST(signatureListReportsByteRangeCoverage) {
    auto engine = pdfiumEngine();
    if (!engine) return;

    const std::string pdf = signedPdf("");
    auto document = openBytes(*engine, pdf);
    if (!document) return;
    const auto list = document->signatures();
    CHECK(list.has_value());
    if (!list.has_value() || *list == nullptr) return;
    CHECK_EQ((*list)->signatures.size(), std::size_t{1});
    if ((*list)->signatures.empty()) return;
    const PdfSignatureInfo& info = (*list)->signatures[0];
    CHECK_EQ(info.subFilter, std::string("adbe.pkcs7.detached"));
    CHECK_EQ(info.reason, std::string("Approval"));
    CHECK_EQ(info.signingTime, std::string("D:20240102030405+00'00'"));
    CHECK_EQ(info.byteRange.size(), std::size_t{2});
    CHECK(info.coverage.valid);
    CHECK(info.coverage.coversWholeDocument);
    CHECK(!info.coverage.modifiedAfterSigning);
    CHECK_EQ(info.coverage.signedEnd, static_cast<std::uint64_t>(pdf.size()));
    CHECK(info.pageIndex.has_value());
    CHECK(info.pageIndex.value_or(9) == 0);
    CHECK(std::abs(info.widgetRect.left - 40.0) < 0.01);
    CHECK(std::abs(info.widgetRect.top - 90.0) < 0.01);

    // Bytes appended after signing: no longer the whole document.
    auto appended = openBytes(*engine, signedPdf("% appended after signing\n"));
    if (!appended) return;
    const auto later = appended->signatures();
    CHECK(later.has_value());
    if (!later.has_value() || *later == nullptr || (*later)->signatures.empty()) {
        CHECK(false);
        return;
    }
    CHECK((*later)->signatures[0].coverage.valid);
    CHECK(!(*later)->signatures[0].coverage.coversWholeDocument);
    CHECK((*later)->signatures[0].coverage.modifiedAfterSigning);

    // A document without signatures lists none; an empty /Sig field is not a signature.
    auto blank = openBytes(*engine, blankPdf());
    if (!blank) return;
    const auto none = blank->signatures();
    CHECK(none.has_value());
    if (none.has_value() && *none != nullptr) CHECK((*none)->empty());

    auto unsignedField = openBytes(
        *engine, pdffix::buildPdf({
                     "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [4 0 R] >> >>",
                     "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                     "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 300] /Annots [4 0 R] >>",
                     "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Empty) /Rect [40 40 200 90] /P 3 0 R >>",
                 }));
    if (!unsignedField) return;
    const auto empty = unsignedField->signatures();
    CHECK(empty.has_value());
    if (empty.has_value() && *empty != nullptr) CHECK((*empty)->empty());
}
