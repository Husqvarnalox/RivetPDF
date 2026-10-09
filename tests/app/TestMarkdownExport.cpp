// SPDX-License-Identifier: MPL-2.0
// The portable Markdown -> PDF exporter (ADR-0023): font mapping, character
// replacement, the bundled-font measurer and the emitted build description
// (validity, pagination, links, images, placeholders). No PDF backend
// needed: runs in every build.
#include "RivetTest.h"

#include "app/MarkdownPdfExport.hpp"
#include "core/Bitmap.hpp"
#include "pdf/BundledFontMetrics.hpp"
#include "pdf/PdfBuild.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <variant>

namespace {

using namespace rivet;
using app::buildMarkdownPdf;
using app::exportCodepoint;
using app::exportFontFor;
using app::ExportImage;
using app::ExportImageMap;
using app::MarkdownExportOptions;
using markdown::TextKind;
using markdown::TextStyle;
using pdf::PdfBundledFont;

TextStyle style(TextKind kind, bool bold = false, bool mono = false, double size = 11.0) {
    TextStyle s;
    s.kind = kind;
    s.bold = bold;
    s.monospace = mono;
    s.size = size;
    return s;
}

template <typename T>
std::size_t countItems(const pdf::PdfBuildDocument& doc) {
    std::size_t n = 0;
    for (const auto& page : doc.pages) {
        for (const auto& item : page.items) n += std::holds_alternative<T>(item) ? 1 : 0;
    }
    return n;
}

std::size_t countStrokes(const pdf::PdfBuildDocument& doc, float grey) {
    std::size_t n = 0;
    for (const auto& page : doc.pages) {
        for (const auto& item : page.items) {
            const auto* rect = std::get_if<pdf::PdfBuildRect>(&item);
            if (rect != nullptr && rect->stroke.has_value() && rect->stroke->r == grey) ++n;
        }
    }
    return n;
}

bool hasText(const pdf::PdfBuildDocument& doc, const std::string& needle) {
    for (const auto& page : doc.pages) {
        for (const auto& item : page.items) {
            const auto* text = std::get_if<pdf::PdfBuildText>(&item);
            if (text != nullptr && text->utf8.find(needle) != std::string::npos) return true;
        }
    }
    return false;
}

const char* kSample = "# Heading One\n"
                      "\n"
                      "Plain paragraph with **bold**, *italic*, ~~strike~~ and `inline code`.\n"
                      "\n"
                      "Привет, мир — кириллица.\n"
                      "\n"
                      "## Second\n"
                      "\n"
                      "```\n"
                      "int main() { return 0; }\n"
                      "```\n"
                      "\n"
                      "1. first\n"
                      "2. second\n"
                      "\n"
                      "- bullet a\n"
                      "- bullet b\n"
                      "\n"
                      "- [ ] open task\n"
                      "- [x] done task\n"
                      "\n"
                      "> quoted text\n"
                      "\n"
                      "---\n"
                      "\n"
                      "| A | B |\n"
                      "|---|---|\n"
                      "| 1 | 2 |\n"
                      "\n"
                      "Visit [the site](https://example.com/page) or [jump](#heading-one).\n"
                      "\n"
                      "![picture](pic.png)\n"
                      "\n"
                      "![absent](missing.png)\n"
                      "\n"
                      "CJK: 中文字\n";

ExportImageMap sampleImages() {
    ExportImageMap images;
    auto bitmap = core::Bitmap::create(40, 20);
    CHECK(bitmap.has_value());
    ExportImage image;
    image.info.state = markdown::ImageState::Known;
    image.info.width = 40;
    image.info.height = 20;
    image.bitmap = std::make_shared<const core::Bitmap>(std::move(*bitmap));
    images.emplace("pic.png", std::move(image));
    return images;
}

} // namespace

RIVET_TEST(exportFontMapping) {
    CHECK(exportFontFor(style(TextKind::Body)) == PdfBundledFont::SansRegular);
    CHECK(exportFontFor(style(TextKind::Body, true)) == PdfBundledFont::SansBold);
    CHECK(exportFontFor(style(TextKind::H2)) == PdfBundledFont::SansBold);
    CHECK(exportFontFor(style(TextKind::Code, false, true)) == PdfBundledFont::MonoRegular);
    CHECK(exportFontFor(style(TextKind::Code, true, true)) == PdfBundledFont::MonoBold);
    CHECK(exportFontFor(style(TextKind::Link)) == PdfBundledFont::SansRegular);
    TextStyle italic = style(TextKind::Quote);
    italic.italic = true;
    CHECK(exportFontFor(italic) == PdfBundledFont::SansRegular);
}

RIVET_TEST(exportCodepointReplacesUncovered) {
    CHECK(exportCodepoint(PdfBundledFont::SansRegular, U'A') == U'A');
    CHECK(exportCodepoint(PdfBundledFont::SansRegular, U'Ж') == U'Ж');
    CHECK(exportCodepoint(PdfBundledFont::SansRegular, U'中') == U'?');
    CHECK(exportCodepoint(PdfBundledFont::MonoRegular, U'\t') == U'?');
    CHECK(exportCodepoint(PdfBundledFont::SansBold, char32_t{0x1F600}) == U'?');
}

RIVET_TEST(bundledMeasurerMatchesFontMetrics) {
    const app::BundledFontMeasurer measurer;
    const std::string texts[] = {"Hello, world", "Привет", "x = 42;"};
    for (const std::string& text : texts) {
        const TextStyle body = style(TextKind::Body, false, false, 11.0);
        CHECK_NEAR(measurer.measure(text, body).width,
                   pdf::bundledFontMetrics(PdfBundledFont::SansRegular).textWidth(text, 11.0), 1e-9);
        const TextStyle code = style(TextKind::Code, false, true, 9.5);
        CHECK_NEAR(measurer.measure(text, code).width,
                   pdf::bundledFontMetrics(PdfBundledFont::MonoRegular).textWidth(text, 9.5), 1e-9);
    }
    // Replaced characters measure as '?'.
    const TextStyle body = style(TextKind::Body);
    CHECK_NEAR(measurer.measure("中", body).width, measurer.measure("?", body).width, 1e-9);
    const markdown::TextMetrics m = measurer.measure("Ag", body);
    CHECK_GT(m.ascent, 0.0);
    CHECK_GT(m.descent, 0.0); // positive, below the baseline
}

RIVET_TEST(exportSampleDocumentIsValid) {
    const ExportImageMap images = sampleImages();
    auto result = buildMarkdownPdf(kSample, MarkdownExportOptions{}, images);
    CHECK(result.has_value());
    if (!result.has_value()) return;
    CHECK(pdf::validatePdfBuild(result->document).has_value());
    CHECK_GE(result->pageCount, std::size_t{1});
    CHECK_EQ(result->pageCount, result->document.pages.size());
    CHECK_EQ(result->replacedCharacters, std::size_t{3});
    CHECK_EQ(result->skippedImages, std::size_t{0});
    CHECK(hasText(result->document, "Heading One"));
    CHECK(hasText(result->document, "Привет"));

    std::size_t links = 0;
    for (const auto& page : result->document.pages) {
        for (const auto& link : page.links) {
            ++links;
            CHECK_EQ(link.uri, std::string("https://example.com/page"));
        }
    }
    CHECK_EQ(links, std::size_t{1}); // the #anchor link is not exported

    CHECK_EQ(countItems<pdf::PdfBuildImage>(result->document), std::size_t{1});
    CHECK_EQ(countStrokes(result->document, 0.7F), std::size_t{1}); // the missing image's placeholder
    CHECK_GE(countStrokes(result->document, 0.75F), std::size_t{4}); // table cells
    CHECK_EQ(countStrokes(result->document, 0.45F), std::size_t{2}); // task checkboxes
}

RIVET_TEST(exportLongDocumentPaginates) {
    std::string source;
    for (int i = 0; i < 300; ++i) {
        source += "Paragraph number " + std::to_string(i) +
                  " with enough words to wrap across the line at least once when set in eleven point type.\n\n";
    }
    const MarkdownExportOptions options;
    auto result = buildMarkdownPdf(source, options, {});
    CHECK(result.has_value());
    if (!result.has_value()) return;
    CHECK_GT(result->pageCount, std::size_t{1});
    CHECK(pdf::validatePdfBuild(result->document).has_value());
    for (const auto& page : result->document.pages) {
        CHECK(!page.items.empty());
        for (const auto& item : page.items) {
            if (const auto* text = std::get_if<pdf::PdfBuildText>(&item)) {
                CHECK(text->origin.y >= options.margin - 1e-6);
                CHECK(text->origin.y <= options.pageHeight - options.margin + 1e-6);
                CHECK(text->origin.x >= options.margin - 1e-6);
            }
        }
    }
    CHECK(hasText(result->document, "Paragraph number 299"));
}

RIVET_TEST(exportEmptySourceGivesOneEmptyPage) {
    auto result = buildMarkdownPdf("", MarkdownExportOptions{}, {});
    CHECK(result.has_value());
    if (!result.has_value()) return;
    CHECK_EQ(result->pageCount, std::size_t{1});
    CHECK(result->document.pages.front().items.empty());
    CHECK(pdf::validatePdfBuild(result->document).has_value());
}

RIVET_TEST(exportOversizedImageFallsBackToPlaceholder) {
    ExportImageMap images;
    auto bitmap = core::Bitmap::create(pdf::kMaxImageSide + 1, 2);
    CHECK(bitmap.has_value());
    if (!bitmap.has_value()) return;
    ExportImage image;
    image.info = {markdown::ImageState::Known, static_cast<double>(pdf::kMaxImageSide + 1), 2.0};
    image.bitmap = std::make_shared<const core::Bitmap>(std::move(*bitmap));
    images.emplace("wide.png", std::move(image));
    auto result = buildMarkdownPdf("![wide](wide.png)\n", MarkdownExportOptions{}, images);
    CHECK(result.has_value());
    if (!result.has_value()) return;
    CHECK_EQ(result->skippedImages, std::size_t{1});
    CHECK_EQ(countItems<pdf::PdfBuildImage>(result->document), std::size_t{0});
    CHECK_EQ(countStrokes(result->document, 0.7F), std::size_t{1});
}

RIVET_TEST(exportRejectsUnusableGeometry) {
    MarkdownExportOptions options;
    options.margin = 400.0;
    CHECK(!buildMarkdownPdf("x", options, {}).has_value());
    options = {};
    options.pageWidth = -1.0;
    CHECK(!buildMarkdownPdf("x", options, {}).has_value());
}
