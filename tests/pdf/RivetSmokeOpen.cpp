// SPDX-License-Identifier: MPL-2.0
//
// rivet-smoke-open: headless end-to-end smoke check of the PDFium-backed
// engine, run by CI on every OS (ctest "pdfium_smoke_open").
//
// Opens a PDF, renders page 1, saves a copy through the document assembly
// (the same path the editor's Save uses), reopens the copy and verifies it.
// Exit status 0 = all steps passed. Usage: rivet-smoke-open [input.pdf [out.pdf]]

#include "pdf/PdfSystem.hpp"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"

#include <cstddef>
#include <expected>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

class FileSink final : public rivet::pdf::IPdfByteSink {
public:
    explicit FileSink(const fs::path& path) : out_(path, std::ios::binary | std::ios::trunc) {}
    bool ok() const { return static_cast<bool>(out_); }
    rivet::core::Status write(const void* data, std::size_t size) override {
        out_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!out_) {
            return std::unexpected(rivet::core::Error{rivet::core::ErrorCode::Io, "write failed"});
        }
        return {};
    }
    void close() { out_.close(); }

private:
    std::ofstream out_;
};

int fail(const char* step) {
    std::fprintf(stderr, "rivet-smoke-open: FAILED at %s\n", step);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
#ifdef RIVET_PDF_FIXTURE_DIR
    fs::path input = fs::path(RIVET_PDF_FIXTURE_DIR) / "hello.pdf";
#else
    fs::path input = "hello.pdf";
#endif
    if (argc > 1) input = argv[1];
    std::error_code ec;
    fs::path output = argc > 2 ? fs::path(argv[2]) : fs::temp_directory_path(ec) / "rivet-smoke-open-copy.pdf";

    auto engine = rivet::pdf::createEngine();
    if (!engine || !engine->isAvailable() || engine->backendName() != "pdfium") {
        return fail("engine is not the PDFium backend");
    }

    auto opened = engine->openDocument(input);
    if (!opened.has_value()) return fail("open");
    auto& document = *opened.value();
    if (document.info().pageCount == 0) return fail("page count");

    auto page = document.pageInfo(0);
    if (!page.has_value()) return fail("page info");
    const rivet::core::Rect area{0.0, 0.0, page.value().sizePoints.width, page.value().sizePoints.height};
    auto bitmap = document.renderPage(0, area, 1.0);
    if (!bitmap.has_value() || !bitmap.value().isValid()) return fail("render page 1");
    bool anyNonZero = false;
    const std::size_t bytes = bitmap.value().sizeBytes();
    const auto* pixels = bitmap.value().data();
    for (std::size_t i = 0; i < bytes && !anyNonZero; ++i) anyNonZero = pixels[i] != std::byte{0};
    if (!anyNonZero) return fail("rendered bitmap is blank");

    rivet::pdf::PdfAssemblyRequest request;
    request.mode = rivet::pdf::PdfAssemblyRequest::Mode::PreserveBase;
    request.base = &document;
    for (std::size_t i = 0; i < document.info().pageCount; ++i) {
        auto info = document.pageInfo(i);
        if (!info.has_value()) return fail("page info (assembly)");
        rivet::pdf::PdfAssemblyPage entry;
        entry.source = &document;
        entry.sourcePageIndex = i;
        entry.view = info.value().view;
        request.pages.push_back(std::move(entry));
    }
    {
        FileSink sink(output);
        if (!sink.ok()) return fail("create output file");
        if (!engine->assembleDocument(request, sink).has_value()) return fail("save copy");
        sink.close();
    }

    auto reopened = engine->openDocument(output);
    if (!reopened.has_value()) return fail("reopen saved copy");
    if (reopened.value()->info().pageCount != document.info().pageCount) return fail("saved page count");

    std::printf("rivet-smoke-open: OK (%zu page(s), %ux%u px, saved %s)\n", document.info().pageCount,
                bitmap.value().width(), bitmap.value().height(), output.string().c_str());
    fs::remove(output, ec);
    return 0;
}
