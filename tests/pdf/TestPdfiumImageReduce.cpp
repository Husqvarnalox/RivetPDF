// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "PdfFixtures.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/ImageReduce.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// Save Reduced Copy (ADR-0028) and Remove Password (ADR-0030) against the
// PDFium backend. PDFium-ON bodies only. JPEG encoding is replaced by a
// test encoder that writes a constant mid-gray baseline JPEG, which PDFium's
// real decoder then reads back.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
namespace pdffix = rivet::test::pdffix;
using rivet::pdf::PdfAssemblyPage;
using rivet::pdf::PdfAssemblyRequest;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;

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

// A valid baseline gray JPEG whose every 8x8 block has all-zero
// coefficients (uniform mid-gray). One 1-bit Huffman code for the DC
// category 0 and one for the AC end-of-block: 2 bits per block.
class GrayJpegEncoder final : public rivet::pdf::IJpegEncoder {
public:
    core::Result<std::vector<std::uint8_t>> encode(std::span<const std::uint8_t>, std::uint32_t width,
                                                   std::uint32_t height, std::uint32_t, int) const override {
        calls.fetch_add(1);
        std::vector<std::uint8_t> out = {0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x43, 0x00};
        out.insert(out.end(), 64, 8);
        out.insert(out.end(), {0xFF, 0xC0, 0x00, 0x0B, 0x08, static_cast<std::uint8_t>(height >> 8),
                               static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(width >> 8),
                               static_cast<std::uint8_t>(width), 0x01, 0x01, 0x11, 0x00});
        for (const std::uint8_t tableClass : {0x00, 0x10}) {
            out.insert(out.end(), {0xFF, 0xC4, 0x00, 0x14, tableClass, 0x01});
            out.insert(out.end(), 15, 0x00);
            out.push_back(0x00);
        }
        out.insert(out.end(), {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00});
        const std::size_t blocks = static_cast<std::size_t>((width + 7) / 8) * ((height + 7) / 8);
        const std::size_t bits = blocks * 2;
        const std::size_t bytes = (bits + 7) / 8;
        for (std::size_t i = 0; i < bytes; ++i) {
            const bool last = i + 1 == bytes && bits % 8 != 0;
            const std::uint8_t value = last ? static_cast<std::uint8_t>(0xFFu >> (bits % 8)) : 0x00;
            out.push_back(value);
            if (value == 0xFF) out.push_back(0x00);
        }
        out.insert(out.end(), {0xFF, 0xD9});
        return out;
    }
    mutable std::atomic<int> calls{0};
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
                          ("rivet-reduce-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                           std::to_string(counter.fetch_add(1)) + ".pdf");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

std::string hexBytes(std::size_t count, const char* byte) {
    std::string out = "<";
    for (std::size_t i = 0; i < count; ++i) out += byte;
    return out + ">";
}

// 200x200 page; a 400x400 RGB image (288 dpi) placed 100x100 pt at (50,50),
// optionally with a soft mask.
std::string imagePdf(bool masked) {
    const std::string rgb = hexBytes(400 * 400 * 3, "80");
    std::vector<std::string> bodies = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R "
        "/Resources << /XObject << /Im1 5 0 R >> >> >>",
        pdffix::streamObject("", "q 100 0 0 100 50 50 cm /Im1 Do Q\n"),
        pdffix::streamObject(std::string("/Type /XObject /Subtype /Image /Width 400 /Height 400 "
                                         "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /ASCIIHexDecode") +
                                 (masked ? " /SMask 6 0 R" : ""),
                             rgb),
    };
    if (masked) {
        bodies.push_back(pdffix::streamObject("/Type /XObject /Subtype /Image /Width 400 /Height 400 "
                                              "/ColorSpace /DeviceGray /BitsPerComponent 8 "
                                              "/Filter /ASCIIHexDecode",
                                              hexBytes(400 * 400, "C0")));
    }
    return pdffix::buildPdf(bodies);
}

struct Reduced {
    MemorySink sink;
    core::Status status = core::ok();
    std::size_t reduced = 0;
};

Reduced reduceCopy(PdfEngine& engine, const PdfDocument& document, const GrayJpegEncoder& encoder,
                   rivet::pdf::ReduceQuality quality) {
    Reduced out;
    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = &document;
    const rivet::pdf::ReducePreset preset = rivet::pdf::reducePreset(quality);
    request.reduceImages =
        rivet::pdf::PdfImageReduction{preset.dpi, preset.jpegQuality, &encoder, &out.reduced};
    PdfAssemblyPage page;
    page.source = &document;
    page.sourcePageIndex = 0;
    const auto info = document.pageInfo(0);
    CHECK(info.has_value());
    if (info.has_value()) page.view = info->view;
    request.pages.push_back(std::move(page));
    out.status = engine.assembleDocument(request, out.sink, nullptr, nullptr);
    return out;
}

const rivet::pdf::PdfContentObject* firstImage(const rivet::pdf::PdfPageContent& content) {
    for (const auto& object : content.objects) {
        if (object.type == rivet::pdf::PdfContentObjectType::Image) return &object;
    }
    return nullptr;
}

RIVET_TEST(reducedCopyDownsamplesLargeImagesAndKeepsTheirPlacement) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const std::string pdf = imagePdf(false);
    auto document = openBytes(*engine, pdf.data(), pdf.size());
    if (!document) return;

    GrayJpegEncoder encoder;
    const Reduced reduced = reduceCopy(*engine, *document, encoder, rivet::pdf::ReduceQuality::Medium);
    CHECK(reduced.status.has_value());
    if (!reduced.status.has_value()) return;
    CHECK_EQ(reduced.reduced, std::size_t{1});
    CHECK_EQ(encoder.calls.load(), 1);
    CHECK_LT(reduced.sink.bytes().size(), pdf.size() / 4);

    auto reopened = openBytes(*engine, reduced.sink.bytes().data(), reduced.sink.bytes().size());
    if (!reopened) return;
    auto content = reopened->pageContent(0, nullptr);
    CHECK(content.has_value());
    if (!content.has_value()) return;
    const auto* image = firstImage(**content);
    CHECK(image != nullptr);
    if (image == nullptr) return;
    // 100 pt at 150 dpi = 208.3 -> 209 px per side.
    CHECK_EQ(image->pixelWidth, std::uint32_t{209});
    CHECK_EQ(image->pixelHeight, std::uint32_t{209});
    CHECK_NEAR(image->matrix.a, 100.0, 0.01);
    CHECK_NEAR(image->matrix.d, 100.0, 0.01);
    CHECK_NEAR(image->matrix.tx, 50.0, 0.01);
    CHECK_NEAR(image->matrix.ty, 50.0, 0.01);

    // The page still renders with the image in place.
    const auto info = reopened->pageInfo(0);
    CHECK(info.has_value());
    if (!info.has_value()) return;
    auto bitmap = reopened->renderPage(0, info->view, {}, nullptr, core::Rect(0.0, 0.0, 200.0, 200.0), 1.0);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return;
    const auto* row = reinterpret_cast<const std::uint8_t*>(bitmap->data()) + 100 * bitmap->stride();
    CHECK(row[100 * 4] > 90 && row[100 * 4] < 170); // mid-gray inside the image
    CHECK(row[10 * 4] > 240);                        // white page outside it
}

RIVET_TEST(reducedCopyLeavesMaskedImagesUntouched) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const std::string pdf = imagePdf(true);
    auto document = openBytes(*engine, pdf.data(), pdf.size());
    if (!document) return;

    GrayJpegEncoder encoder;
    const Reduced reduced = reduceCopy(*engine, *document, encoder, rivet::pdf::ReduceQuality::Small);
    CHECK(reduced.status.has_value());
    if (!reduced.status.has_value()) return;
    CHECK_EQ(reduced.reduced, std::size_t{0});
    CHECK_EQ(encoder.calls.load(), 0);

    auto reopened = openBytes(*engine, reduced.sink.bytes().data(), reduced.sink.bytes().size());
    if (!reopened) return;
    auto content = reopened->pageContent(0, nullptr);
    CHECK(content.has_value());
    if (!content.has_value()) return;
    const auto* image = firstImage(**content);
    CHECK(image != nullptr);
    if (image == nullptr) return;
    CHECK_EQ(image->pixelWidth, std::uint32_t{400});
    CHECK_EQ(image->pixelHeight, std::uint32_t{400});
}

RIVET_TEST(removeSecuritySaveReopensWithoutAPassword) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const fs::path path = fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / "password.pdf";
    auto opened = engine->openDocument(path, "rivet");
    CHECK(opened.has_value());
    if (!opened.has_value()) return;
    CHECK((*opened)->info().isEncrypted);

    for (const bool removeSecurity : {false, true}) {
        PdfAssemblyRequest request;
        request.mode = PdfAssemblyRequest::Mode::PreserveBase;
        request.base = opened->get();
        request.removeSecurity = removeSecurity;
        for (std::size_t i = 0; i < (*opened)->info().pageCount; ++i) {
            PdfAssemblyPage page;
            page.source = opened->get();
            page.sourcePageIndex = i;
            const auto info = (*opened)->pageInfo(i);
            CHECK(info.has_value());
            if (info.has_value()) page.view = info->view;
            request.pages.push_back(std::move(page));
        }
        MemorySink sink;
        const core::Status status = engine->assembleDocument(request, sink, nullptr, nullptr);
        CHECK(status.has_value());
        if (!status.has_value()) return;

        static std::atomic<int> counter{0};
        const fs::path out = fs::temp_directory_path() / ("rivet-nopw-" + std::to_string(counter.fetch_add(1)) +
                                                          "-" + std::to_string(removeSecurity) + ".pdf");
        {
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            file.write(sink.bytes().data(), static_cast<std::streamsize>(sink.bytes().size()));
        }
        auto plain = engine->openDocument(out);
        std::error_code ignored;
        if (removeSecurity) {
            CHECK(plain.has_value());
            if (plain.has_value()) {
                CHECK(!(*plain)->info().isEncrypted);
                CHECK_EQ((*plain)->info().pageCount, std::size_t{4});
            }
        } else {
            // Without the flag the save keeps the encryption.
            CHECK(!plain.has_value());
        }
        fs::remove(out, ignored);
    }
}

RIVET_TEST(removeSecurityCannotCombineWithRemoveMetadata) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const std::string pdf = imagePdf(false);
    auto document = openBytes(*engine, pdf.data(), pdf.size());
    if (!document) return;
    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = document.get();
    request.removeSecurity = true;
    request.removeMetadata = true;
    PdfAssemblyPage page;
    page.source = document.get();
    page.sourcePageIndex = 0;
    request.pages.push_back(std::move(page));
    MemorySink sink;
    CHECK(!engine->assembleDocument(request, sink, nullptr, nullptr).has_value());
}

} // namespace
