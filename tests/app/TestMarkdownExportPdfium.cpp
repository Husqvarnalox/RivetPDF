// SPDX-License-Identifier: MPL-2.0
// Markdown -> PDF through the real engine: the exported file reopens, has
// the expected pages, extractable heading and Cyrillic text, the external
// link annotation, and renders. Early return without a PDF backend.
#include "RivetTest.h"

#include "app/MarkdownPdfExport.hpp"
#include "core/Error.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfSystem.hpp"
#include "pdf/PdfText.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace {

namespace fs = std::filesystem;
using namespace rivet;

class StringSink final : public pdf::IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        bytes.append(static_cast<const char*>(data), size);
        return core::ok();
    }
    std::string bytes;
};

const char* kSource = "# Export Heading\n"
                      "\n"
                      "Привет мир and **bold** text.\n"
                      "\n"
                      "See [the site](https://example.com/x) for more.\n"
                      "\n"
                      "- one\n"
                      "- two\n";

} // namespace

RIVET_TEST(markdownExportReopensThroughPdfium) {
    std::unique_ptr<pdf::PdfEngine> engine = pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return;

    auto built = app::buildMarkdownPdf(kSource, app::MarkdownExportOptions{}, {});
    CHECK(built.has_value());
    if (!built.has_value()) return;
    StringSink sink;
    CHECK(engine->buildDocument(built->document, sink).has_value());
    CHECK(sink.bytes.rfind("%PDF-", 0) == 0);

    const fs::path file = fs::temp_directory_path() / "rivet-mdexport-pdfium.pdf";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(sink.bytes.data(), static_cast<std::streamsize>(sink.bytes.size()));
    }
    {
        auto opened = engine->openDocument(file);
        CHECK(opened.has_value());
        if (opened.has_value()) {
            pdf::PdfDocument& document = **opened;
            CHECK_EQ(document.info().pageCount, built->pageCount);
            CHECK_EQ(document.info().pageCount, std::size_t{1});

            auto info = document.pageInfo(0);
            CHECK(info.has_value());
            if (info.has_value()) {
                CHECK_NEAR(info->sizePoints.width, 595.0, 1.0);
                CHECK_NEAR(info->sizePoints.height, 842.0, 1.0);
            }

            auto text = document.textPage(0);
            CHECK(text.has_value());
            if (text.has_value()) {
                CHECK((*text)->text().find("Export Heading") != std::string::npos);
                CHECK((*text)->text().find("Привет") != std::string::npos);
            }

            auto links = document.pageLinks(0);
            CHECK(links.has_value());
            std::size_t external = 0;
            if (links.has_value()) {
                for (const auto& link : *links) {
                    if (link.kind == pdf::PdfPageLink::Kind::External) {
                        ++external;
                        CHECK_EQ(link.url, std::string("https://example.com/x"));
                        CHECK(!link.rects.empty());
                    }
                }
            }
            CHECK_EQ(external, std::size_t{1});

            auto bitmap = document.renderPage(0, core::Rect{0.0, 0.0, 200.0, 100.0}, 1.0);
            CHECK(bitmap.has_value());
        }
    }
    std::error_code ignored;
    fs::remove(file, ignored);
}
