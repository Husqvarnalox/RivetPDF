#include "RivetTest.h"

// PdfSystem.hpp first: it re-exports core::Result into rivet::pdf, which the
// frozen pdf/PdfEngine.hpp interface header refers to.
#include "pdf/PdfSystem.hpp"

#include "core/Error.hpp"
#include "pdf/PdfEngine.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>

#ifndef RIVET_PDF_FIXTURE_DIR
// Fallback for builds outside CMake; the CMake build defines the absolute
// fixtures path (see tests/pdf/CMakeLists.txt).
#define RIVET_PDF_FIXTURE_DIR "tests/fixtures"
#endif

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;

constexpr double kSizeEpsilon = 0.5; // page sizes come back as floats

} // namespace

// One end-to-end flow over the engine factory contract. Runtime-branching so
// it passes in both build modes: RIVET_WITH_PDFIUM=OFF exercises the null
// backend contract; ON exercises the full PDFium path against the
// deterministic Letter-size fixture.
RIVET_TEST(pdfSystemContract) {
    const fs::path fixtureDir = RIVET_PDF_FIXTURE_DIR;

    auto engine = rivet::pdf::createEngine();
    CHECK(engine != nullptr);
    CHECK(!engine->backendName().empty());

    if (!engine->isAvailable()) {
        // Null backend: no document can ever be opened, and the error is
        // NotAvailable. All PdfDocument methods are unreachable here.
        CHECK_EQ(engine->backendName(), "none");
        auto doc = engine->openDocument(fixtureDir / "hello.pdf");
        CHECK(!doc.has_value());
        CHECK_EQ(doc.error().code, core::ErrorCode::NotAvailable);
        return;
    }

    CHECK_EQ(engine->backendName(), "pdfium");

    auto missing = engine->openDocument(fixtureDir / "does_not_exist.pdf");
    CHECK(!missing.has_value());
    CHECK_EQ(missing.error().code, core::ErrorCode::NotFound);

    auto opened = engine->openDocument(fixtureDir / "hello.pdf");
    CHECK(opened.has_value());
    if (!opened.has_value()) {
        return;
    }
    rivet::pdf::PdfDocument& doc = **opened;

    const rivet::pdf::PdfDocumentInfo& info = doc.info();
    CHECK_EQ(info.pageCount, std::size_t{1});
    CHECK_EQ(info.isEncrypted, false);
    CHECK(info.title.empty()); // fixture carries no title metadata

    auto page = doc.pageInfo(0);
    CHECK(page.has_value());
    if (!page.has_value()) {
        return;
    }
    CHECK_EQ(page->index, std::size_t{0});
    CHECK_NEAR(page->sizePoints.width, 612.0, kSizeEpsilon);
    CHECK_NEAR(page->sizePoints.height, 792.0, kSizeEpsilon);
    CHECK_EQ(page->rotation, core::PageRotation::None);

    auto badIndex = doc.pageInfo(99);
    CHECK(!badIndex.has_value());
    CHECK_EQ(badIndex.error().code, core::ErrorCode::InvalidArgument);

    // Full page at 1x.
    auto full =
        doc.renderPage(0, core::Rect{core::Point{0.0, 0.0}, core::Size{612.0, 792.0}}, 1.0);
    CHECK(full.has_value());
    if (full.has_value()) {
        CHECK_EQ(full->width(), 612u);
        CHECK_EQ(full->height(), 792u);
        CHECK(full->isValid());
    }

    // Sub-tile at 2x.
    auto tile =
        doc.renderPage(0, core::Rect{core::Point{0.0, 0.0}, core::Size{100.0, 100.0}}, 2.0);
    CHECK(tile.has_value());
    if (tile.has_value()) {
        CHECK_EQ(tile->width(), 200u);
        CHECK_EQ(tile->height(), 200u);
    }

    // A rectangle outside the page bounds is rejected.
    auto reject =
        doc.renderPage(0, core::Rect{core::Point{0.0, 0.0}, core::Size{100000.0, 100000.0}}, 1.0);
    CHECK(!reject.has_value());
    CHECK_EQ(reject.error().code, core::ErrorCode::InvalidArgument);

    // A non-positive scale is rejected.
    auto zeroScale =
        doc.renderPage(0, core::Rect{core::Point{0.0, 0.0}, core::Size{100.0, 100.0}}, 0.0);
    CHECK(!zeroScale.has_value());
    CHECK_EQ(zeroScale.error().code, core::ErrorCode::InvalidArgument);
}
