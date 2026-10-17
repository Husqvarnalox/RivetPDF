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
#include <system_error>
#include <vector>

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

RIVET_TEST(markdownExportWritesDiagramsAndMathThroughPdfium) {
    std::unique_ptr<pdf::PdfEngine> engine = pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return;
    const char* source = "# Diagrams\n\n"
                         "Energy $E = mc^2$ and $\\alpha + \\beta$ inline.\n\n"
                         "$$\n\\sum_{k=1}^{n} k = \\frac{n(n+1)}{2}\n$$\n\n"
                         "```mermaid\nflowchart LR\n  A[Start] --> B{Check}\n  B -->|ok| C[Finish]\n  B -.-> A\n```\n\n"
                         "```mermaid\nsequenceDiagram\n  Alice->>Bob: Request\n  Bob-->>Alice: Response\n```\n";
    auto built = app::buildMarkdownPdf(source, app::MarkdownExportOptions{}, {});
    CHECK(built.has_value());
    if (!built.has_value()) return;
    StringSink sink;
    CHECK(engine->buildDocument(built->document, sink).has_value());
    // The bundled math face is embedded (its PostScript name is in the file).
    CHECK(sink.bytes.find("NotoSansMath") != std::string::npos);

    const fs::path file = fs::temp_directory_path() / "rivet-mdexport-graphics.pdf";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(sink.bytes.data(), static_cast<std::streamsize>(sink.bytes.size()));
    }
    {
        auto opened = engine->openDocument(file);
        CHECK(opened.has_value());
        if (opened.has_value()) {
            pdf::PdfDocument& document = **opened;
            std::string all;
            for (std::size_t p = 0; p < document.info().pageCount; ++p) {
                auto text = document.textPage(p);
                CHECK(text.has_value());
                if (text.has_value()) all += (*text)->text();
            }
            // Diagram labels.
            for (const char* label : {"Start", "Check", "Finish", "ok", "Alice", "Bob", "Request", "Response"}) {
                CHECK(all.find(label) != std::string::npos);
            }
            // Formula glyphs written in the math face: the summation sign,
            // the relations and operators, the digits. (The mathematical
            // italic letters are outside the BMP: PdfTextPage reports each
            // UTF-16 half as U+FFFD, so they are not compared here.)
            CHECK(all.find("\xE2\x88\x91") != std::string::npos); // U+2211
            CHECK(all.find("= ") != std::string::npos);
            CHECK(all.find("+ 1)") != std::string::npos);
            CHECK(all.find("=1") != std::string::npos);
            auto bitmap = document.renderPage(0, core::Rect{0.0, 0.0, 300.0, 400.0}, 1.0);
            CHECK(bitmap.has_value());
        }
    }
    std::error_code ignored;
    fs::remove(file, ignored);
}

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

// QA bug 6: the math italic letters (Mathematical Alphanumeric Symbols,
// outside the BMP) of an exported formula are real glyphs, not .notdef
// boxes: the italic capital E is taller than the italic m (every .notdef
// box is the same rectangle).
RIVET_TEST(markdownExportMathLettersHaveGlyphsThroughPdfium) {
    std::unique_ptr<pdf::PdfEngine> engine = pdf::createEngine();
    if (engine == nullptr || !engine->isAvailable()) return;
    auto built = app::buildMarkdownPdf("$$\nE\n$$\n\n$$\nm\n$$\n", app::MarkdownExportOptions{}, {});
    CHECK(built.has_value());
    if (!built.has_value()) return;
    StringSink sink;
    CHECK(engine->buildDocument(built->document, sink).has_value());
    const fs::path file = fs::temp_directory_path() / "rivet-mdexport-mathglyphs.pdf";
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out.write(sink.bytes.data(), static_cast<std::streamsize>(sink.bytes.size()));
    }
    {
        auto opened = engine->openDocument(file);
        CHECK(opened.has_value());
        if (opened.has_value()) {
            auto text = (*opened)->textPage(0);
            CHECK(text.has_value());
            if (text.has_value()) {
                // The letters' glyph boxes in reading order (PdfTextPage
                // reports each UTF-16 half of a letter as U+FFFD; only boxed
                // entries are kept).
                std::vector<core::Rect> boxes;
                for (const pdf::TextChar& ch : (*text)->chars()) {
                    if (ch.bounds.size.width > 0.0 && ch.bounds.size.height > 0.0) boxes.push_back(ch.bounds);
                }
                CHECK(boxes.size() >= 2);
                if (boxes.size() >= 2) CHECK(boxes.front().size.height > boxes.back().size.height * 1.2);
            }
        }
    }
    std::error_code ignored;
    fs::remove(file, ignored);
}
