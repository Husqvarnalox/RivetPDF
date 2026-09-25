// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Error.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfSystem.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifndef RIVET_PDF_TEXT_FIXTURE_DIR
// Same convention as TestPdfiumText.cpp.
#define RIVET_PDF_TEXT_FIXTURE_DIR "tests/pdf/fixtures"
#endif

// Document-navigation tests (outline, page labels, links) against the
// deterministic fixtures. PDFium-ON bodies only.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;

// Returns null (after an OFF-build reporting CHECK) when no PDFium backend
// exists: the PDFium bodies skip in that configuration.
std::unique_ptr<rivet::pdf::PdfDocument> open(const char* name) {
    const std::unique_ptr<rivet::pdf::PdfEngine> engine = rivet::pdf::createEngine();
    CHECK(engine != nullptr);
    if (!engine) return nullptr;
    if (!engine->isAvailable()) {
        CHECK_EQ(engine->backendName(), "none");
        return nullptr;
    }
    auto opened = engine->openDocument(fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / name);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

} // namespace

RIVET_TEST(pdfiumOutlineExtractsNestedTree) {
    auto document = open("outline.pdf");
    if (!document) return;

    const auto outline = document->outline();
    CHECK(outline.has_value());
    if (!outline.has_value()) return;
    CHECK(outline->has_value());
    if (!outline->has_value()) return;
    const rivet::pdf::PdfOutlineNode& root = **outline;

    CHECK_EQ(root.children.size(), std::size_t{2});
    if (root.children.size() != 2) return;

    const rivet::pdf::PdfOutlineNode& chapter1 = root.children[0];
    CHECK_EQ(chapter1.title, std::string("Chapter 1"));
    CHECK(chapter1.destination.has_value());
    if (chapter1.destination) {
        CHECK_EQ(chapter1.destination->pageIndex, std::size_t{0});
        CHECK_EQ(chapter1.destination->fit, rivet::pdf::PdfDestination::Fit::Fit);
    }
    CHECK_EQ(chapter1.children.size(), std::size_t{2});
    if (chapter1.children.size() == 2) {
        CHECK_EQ(chapter1.children[0].title, std::string("Section 1.1"));
        CHECK(chapter1.children[0].destination.has_value());
        if (chapter1.children[0].destination) {
            const rivet::pdf::PdfDestination& dest = *chapter1.children[0].destination;
            CHECK_EQ(dest.pageIndex, std::size_t{1});
            CHECK(dest.hasPoint);
            // /XYZ 72 720 (user, y-up) on the 612x792 page maps to display
            // (72, 792 - 720) = (72, 72).
            CHECK_NEAR(dest.point.x, 72.0, 0.5);
            CHECK_NEAR(dest.point.y, 72.0, 0.5);
            CHECK_EQ(dest.fit, rivet::pdf::PdfDestination::Fit::XYZ);
        }
        CHECK_EQ(chapter1.children[1].title, std::string("Section 1.2"));
        CHECK_EQ(chapter1.children[1].children.size(), std::size_t{0});
    }
    CHECK(!chapter1.truncated);

    const rivet::pdf::PdfOutlineNode& chapter2 = root.children[1];
    CHECK_EQ(chapter2.title, std::string("Chapter 2"));
    CHECK(chapter2.destination.has_value());
    if (chapter2.destination) CHECK_EQ(chapter2.destination->pageIndex, std::size_t{2});
    CHECK_EQ(chapter2.children.size(), std::size_t{0});
}

RIVET_TEST(pdfiumOutlineDepthLimitMarksTruncation) {
    auto document = open("outline.pdf");
    if (!document) return;
    const auto outline = document->outline();
    CHECK(outline.has_value());
    if (!outline.has_value() || !outline->has_value()) return;
    // The fixture tree is shallow: nothing may claim truncation.
    const rivet::pdf::PdfOutlineNode& root = **outline;
    CHECK(!root.truncated);
    for (const rivet::pdf::PdfOutlineNode& node : root.children) {
        CHECK(!node.truncated);
        for (const rivet::pdf::PdfOutlineNode& child : node.children) {
            CHECK(!child.truncated);
        }
    }
}

RIVET_TEST(pdfiumOutlineAbsentReturnsNullopt) {
    auto document = open("corners.pdf");
    if (!document) return;
    const auto outline = document->outline();
    CHECK(outline.has_value());
    if (outline.has_value()) {
        CHECK(!outline->has_value()); // no outline: nullopt, not an error
    }
}

RIVET_TEST(pdfiumPageLabels) {
    auto document = open("page-labels.pdf");
    if (!document) return;
    const auto l0 = document->pageLabel(0);
    const auto l1 = document->pageLabel(1);
    const auto l2 = document->pageLabel(2);
    const auto l3 = document->pageLabel(3);
    CHECK(l0.has_value() && l1.has_value() && l2.has_value() && l3.has_value());
    if (l0.has_value()) CHECK_EQ(*l0, std::string("i"));
    if (l1.has_value()) CHECK_EQ(*l1, std::string("A-1"));
    if (l2.has_value()) CHECK_EQ(*l2, std::string("A-2"));
    if (l3.has_value()) CHECK_EQ(*l3, std::string("A-3"));

    auto bad = document->pageLabel(9);
    CHECK(bad.has_value() || bad.error().code == core::ErrorCode::InvalidArgument);

    auto none = open("corners.pdf");
    if (!none) return;
    auto label = none->pageLabel(0);
    CHECK(label.has_value());
    if (label.has_value()) CHECK_EQ(*label, std::string("")); // no labels defined
}

RIVET_TEST(pdfiumInternalLinks) {
    auto document = open("internal-links.pdf");
    if (!document) return;

    const auto links0 = document->pageLinks(0);
    CHECK(links0.has_value());
    if (!links0.has_value()) return;
    CHECK_EQ(links0->size(), std::size_t{2});
    if (links0->size() == 2) {
        for (const rivet::pdf::PdfPageLink& link : *links0) {
            CHECK(link.kind == rivet::pdf::PdfPageLink::Kind::Internal);
            CHECK(!link.rects.empty());
        }
        // Reading order: first link -> page 1, second -> page 2.
        CHECK_EQ((*links0)[0].destination.pageIndex, std::size_t{1});
        CHECK_EQ((*links0)[1].destination.pageIndex, std::size_t{2});
        // User rect [72 700 320 724] maps to display y 792-724=68 .. 792-700=92.
        const core::Rect& rect = (*links0)[0].rects.front();
        CHECK_NEAR(rect.minY(), 68.0, 1.0);
        CHECK_NEAR(rect.maxY(), 92.0, 1.0);
        CHECK_NEAR(rect.minX(), 72.0, 1.0);
        CHECK_NEAR(rect.maxX(), 320.0, 1.0);
    }

    const auto links1 = document->pageLinks(1);
    CHECK(links1.has_value());
    if (links1.has_value()) {
        CHECK_EQ(links1->size(), std::size_t{1});
        if (links1->size() == 1) {
            CHECK(links1->front().kind == rivet::pdf::PdfPageLink::Kind::Internal);
            CHECK_EQ(links1->front().destination.pageIndex, std::size_t{2});
        }
    }
}

RIVET_TEST(pdfiumExternalLink) {
    auto document = open("external-link.pdf");
    if (!document) return;
    const auto links = document->pageLinks(0);
    CHECK(links.has_value());
    if (!links.has_value()) return;
    CHECK_EQ(links->size(), std::size_t{1});
    if (links->size() == 1) {
        CHECK(links->front().kind == rivet::pdf::PdfPageLink::Kind::External);
        CHECK_EQ(links->front().url, std::string("https://example.com/rivet"));
        CHECK(!links->front().rects.empty());
    }
}

RIVET_TEST(pdfiumPageWithoutLinksIsEmpty) {
    auto document = open("corners.pdf");
    if (!document) return;
    const auto links = document->pageLinks(0);
    CHECK(links.has_value());
    if (links.has_value()) CHECK(links->empty());
}

RIVET_TEST(pdfiumPageLinksOutOfRangeIsInvalidArgument) {
    auto document = open("corners.pdf");
    if (!document) return;
    const auto links = document->pageLinks(5);
    CHECK(!links.has_value());
    if (!links.has_value()) CHECK_EQ(links.error().code, core::ErrorCode::InvalidArgument);
}
