// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

// Assembly must never leak render-time mutations of a LIVE document into its
// output. PDFium generates an /AP stream for an annotation without one when
// the page is rendered (CPDF_AnnotList); a page imported from the rendered
// live document would carry it, while a page imported from a private copy of
// the file bytes does not. PDFium-ON bodies only (skip when unavailable).

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using rivet::pdf::PdfAssemblyPage;
using rivet::pdf::PdfAssemblyRequest;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = rivet::pdf::createEngine();
    CHECK(engine != nullptr);
    if (!engine || !engine->isAvailable()) return nullptr;
    return engine;
}

class MemorySink final : public rivet::pdf::IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        const auto* bytes = static_cast<const char*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
        return core::ok();
    }
    std::string text() const { return std::string(bytes_.begin(), bytes_.end()); }

private:
    std::vector<char> bytes_;
};

// A one-page PDF (catalog, pages, page, empty contents, then the given
// annotation dictionaries) built in memory.
std::string buildPdf(const std::vector<std::string>& annotationBodies) {
    std::vector<std::string> objects;
    std::string annots;
    for (std::size_t i = 0; i < annotationBodies.size(); ++i) {
        annots += " " + std::to_string(5 + i) + " 0 R";
    }
    objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
    objects.push_back("<< /Type /Pages /Kids [3 0 R] /Count 1 >>");
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] /Contents 4 0 R /Annots [" +
                      annots + " ] >>");
    objects.push_back("<< /Length 0 >>\nstream\n\nendstream");
    for (const std::string& body : annotationBodies) objects.push_back(body);

    std::string out = "%PDF-1.7\n";
    std::vector<std::size_t> offsets;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(out.size());
        out += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const std::size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const std::size_t offset : offsets) {
        const std::string digits = std::to_string(offset);
        out += std::string(10 - digits.size(), '0') + digits + " 00000 n \n";
    }
    out += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return out;
}

// Square annotation WITHOUT /AP: PDFium generates one when it renders it.
const char* const kSquareWithoutAp =
    "<< /Type /Annot /Subtype /Square /Rect [50 50 150 150] /C [1 0 0] /IC [0 1 0] /F 4 /BS << /W 4 >> >>";

std::unique_ptr<PdfDocument> openBytes(PdfEngine& engine, const std::string& bytes) {
    static std::atomic<int> counter{0};
    const fs::path path = fs::temp_directory_path() /
                          ("rivet-assembly-purity-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                           std::to_string(counter.fetch_add(1)) + ".pdf");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        CHECK(out.good());
    }
    // The document holds its own descriptor, so the file can go right away.
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

// Renders page 0 so PDFium builds the annotation list (and generates the AP).
void renderPage(PdfDocument& document) {
    const auto bitmap = document.renderPage(0, core::Rect{0.0, 0.0, 200.0, 200.0}, 1.0);
    CHECK(bitmap.has_value());
}

std::size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    for (std::size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + 1)) {
        ++count;
    }
    return count;
}

void addPage(PdfAssemblyRequest& req, const PdfDocument& source, std::size_t index) {
    const auto info = source.pageInfo(index);
    CHECK(info.has_value());
    req.pages.push_back(PdfAssemblyPage{&source, index,
                                        info.has_value() ? info->view : rivet::pdf::PdfPageView{}, nullptr});
}

} // namespace

RIVET_TEST(assemblyPurityFixtureHasNoAppearanceStream) {
    const std::string bytes = buildPdf({kSquareWithoutAp});
    CHECK_EQ(countOccurrences(bytes, "/AP"), std::size_t{0});
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto document = openBytes(*engine, bytes);
    if (!document) return;
    CHECK_EQ(document->info().pageCount, std::size_t{1});
}

RIVET_TEST(pdfiumExtractFromRenderedDocumentDoesNotLeakGeneratedAppearance) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto source = openBytes(*engine, buildPdf({kSquareWithoutAp}));
    if (!source) return;
    renderPage(*source); // PDFium now holds a generated /AP in the live document

    PdfAssemblyRequest req;
    req.mode = PdfAssemblyRequest::Mode::Fresh;
    addPage(req, *source, 0);
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    CHECK(sink.text().find("/Square") != std::string::npos);
    CHECK_EQ(countOccurrences(sink.text(), "/AP"), std::size_t{0});
}

RIVET_TEST(pdfiumMergeFromRenderedDocumentDoesNotLeakGeneratedAppearance) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openBytes(*engine, buildPdf({}));
    auto other = openBytes(*engine, buildPdf({kSquareWithoutAp}));
    if (!base || !other) return;
    renderPage(*other);

    PdfAssemblyRequest req;
    req.mode = PdfAssemblyRequest::Mode::PreserveBase;
    req.base = base.get();
    addPage(req, *base, 0);
    addPage(req, *other, 0);
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    CHECK(sink.text().find("/Square") != std::string::npos);
    CHECK_EQ(countOccurrences(sink.text(), "/AP"), std::size_t{0});
}

RIVET_TEST(pdfiumDuplicateOfRenderedBasePageDoesNotLeakGeneratedAppearance) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openBytes(*engine, buildPdf({kSquareWithoutAp}));
    if (!base) return;
    renderPage(*base);

    PdfAssemblyRequest req;
    req.mode = PdfAssemblyRequest::Mode::PreserveBase;
    req.base = base.get();
    addPage(req, *base, 0);
    addPage(req, *base, 0); // the copy is imported, the original stays in place
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    CHECK_EQ(countOccurrences(sink.text(), "/Square"), std::size_t{2});
    CHECK_EQ(countOccurrences(sink.text(), "/AP"), std::size_t{0});
}
