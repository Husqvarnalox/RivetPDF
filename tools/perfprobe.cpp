// SPDX-License-Identifier: MPL-2.0
// Performance probe: ad-hoc tool for the Phase 2 measurement pass (spec
// section 49: measure and report, no hard assertions). Built out-of-tree:
//
//   clang++ -std=c++23 -O2 -I src -I <pdfium>/include tools/perfprobe.cpp -o /tmp/perfprobe \
//       build/debug-pdfium/src/pdf/librivet_pdf.a build/debug-pdfium/src/pdf/pdfium/librivet_pdfium.a \
//       build/debug-pdfium/src/core/librivet_core.a <pdfium>/lib/libpdfium.a \
//       -framework CoreFoundation -framework CoreGraphics -framework CoreText -framework Security
//
// Prints wall-clock timings for: document open (1 page / N pages), first
// raster, tile raster, one-page text extraction, full-document search, and
// search cancellation restart.

#include "core/Time.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfSystem.hpp"
#include "pdf/PdfText.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::Rect;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;

#define CHECK_ENGINE()                                                        \
    do {                                                                      \
        if (!engine->isAvailable()) {                                         \
            std::fprintf(stderr, "no PDFium backend; rebuild with PDFium\n"); \
            return 2;                                                          \
        }                                                                     \
    } while (false)

namespace {

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

void report(const char* label, double ms) { std::printf("%-46s %10.2f ms\n", label, ms); }

// Mirrors DocumentSession::create's synchronous metadata work: open + one
// pageInfo + one pageLabel round-trip per page.
std::unique_ptr<PdfDocument> openAndScan(PdfEngine& engine, const std::string& path) {
    auto opened = engine.openDocument(path);
    if (!opened.has_value()) return nullptr;
    PdfDocument& doc = **opened;
    const std::size_t pages = doc.info().pageCount;
    for (std::size_t i = 0; i < pages; ++i) {
        (void)doc.pageInfo(i);
        (void)doc.pageLabel(i);
    }
    return std::move(*opened);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: perfprobe <one-page.pdf> <many-page.pdf> [password]\n");
        return 2;
    }
    const std::string one = argv[1];
    const std::string many = argv[2];
    const std::string password = argc > 3 ? argv[3] : "";

    auto engine = rivet::pdf::createEngine();
    CHECK_ENGINE();

    // 1. Open: 1-page document.
    {
        const auto start = std::chrono::steady_clock::now();
        auto doc = openAndScan(*engine, one);
        report("open 1-page document (open + metadata)", msSince(start));
        if (doc == nullptr) return 1;
    }
    // 2. Open: many-page document (+ metadata scan + labels).
    std::unique_ptr<PdfDocument> doc;
    {
        const auto start = std::chrono::steady_clock::now();
        doc = openAndScan(*engine, many);
        report("open 300-page document (open + metadata + labels)", msSince(start));
    }
    if (doc == nullptr) {
        std::fprintf(stderr, "failed to open %s\n", many.c_str());
        return 1;
    }

    // 3. First visible raster: page 0 top band at 2x density.
    {
        const auto watch = std::chrono::steady_clock::now();
        auto bitmap = doc->renderPage(0, Rect{0.0, 0.0, 512.0, 512.0}, 2.0);
        const double ms = msSince(watch);
        report("first tile raster (512x512 @2x)", ms);
        if (!bitmap.has_value()) std::printf("  (render failed: %s)\n", bitmap.error().message.c_str());
    }

    // 4. Warm tile raster (same region again).
    {
        const auto watch = std::chrono::steady_clock::now();
        auto bitmap = doc->renderPage(0, Rect{0.0, 0.0, 512.0, 512.0}, 2.0);
        report("second tile raster (backend cost only)", msSince(watch));
    }

    // 5. One-page text extraction (cold).
    {
        const auto watch = std::chrono::steady_clock::now();
        auto text = doc->textPage(0);
        report("text extraction, one page", msSince(watch));
        if (text.has_value()) {
            std::printf("  (chars: %zu)\n", (*text)->charCount());
        }
    }

    // 6. Whole-document search (extraction + match per page).
    {
        rivet::pdf::TextSearchOptions options;
        const auto watch = std::chrono::steady_clock::now();
        std::size_t matches = 0;
        for (std::size_t i = 0; i < doc->info().pageCount; ++i) {
            auto text = doc->textPage(i);
            if (!text.has_value()) continue;
            matches += rivet::pdf::searchTextPage(**text, "Page", options).size();
        }
        report("search whole document (300 pages, cold)", msSince(watch));
        std::printf("  (matches: %zu)\n", matches);
    }

    // 7. Search restart (cancellation proxy): same run with warm text pages.
    {
        rivet::pdf::TextSearchOptions options;
        const auto watch = std::chrono::steady_clock::now();
        std::size_t matches = 0;
        for (std::size_t i = 0; i < doc->info().pageCount; ++i) {
            auto text = doc->textPage(i);
            if (!text.has_value()) continue;
            matches += rivet::pdf::searchTextPage(**text, "Page", options).size();
        }
        report("search whole document (warm text pages)", msSince(watch));
        std::printf("  (matches: %zu)\n", matches);
    }

    // 8. Thumbnail-scale raster.
    {
        const auto watch = std::chrono::steady_clock::now();
        auto bitmap = doc->renderPage(0, Rect{0.0, 0.0, 612.0, 792.0}, 0.53);
        report("thumbnail raster (whole page @0.53x)", msSince(watch));
    }

    std::printf("done\n");
    return 0;
}
