#include "RivetTest.h"

// PdfSystem.hpp first: it re-exports core::Result into rivet::pdf, which the
// frozen pdf/PdfEngine.hpp interface header refers to.
#include "pdf/PdfSystem.hpp"

#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfText.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef RIVET_PDF_TEXT_FIXTURE_DIR
// Fallback for builds outside CMake; the CMake build defines the absolute
// path (see tests/pdf/CMakeLists.txt).
#define RIVET_PDF_TEXT_FIXTURE_DIR "tests/pdf/fixtures"
#endif

// Text-extraction tests against the deterministic text fixtures in
// tests/pdf/fixtures/ (see that directory's README.md and make_fixtures.py).
//
// These run only in the PDFium-ON build (like TestPdfiumRender.cpp): the
// bodies return after the engine checks in a PDFium-OFF build. Every display
// -space expectation below was pinned empirically against the fixtures and is
// deterministic for the PDFium build in use; the rotation mapping follows the
// convention the rot90 render fixture established in Phase 1 (content at the
// user-space top-left lands in the display top-right for /Rotate 90).

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfTextPage;
using rivet::pdf::TextSearchOptions;
using rivet::pdf::TextSearchResult;

constexpr double kPositionEpsilon = 2.0; // display-space box tolerance, points

std::unique_ptr<PdfEngine> requirePdfiumEngine() {
    auto engine = rivet::pdf::createEngine();
    CHECK(engine != nullptr);
    if (!engine->isAvailable()) {
        CHECK_EQ(engine->backendName(), "none");
        return nullptr;
    }
    CHECK_EQ(engine->backendName(), "pdfium");
    return engine;
}

std::unique_ptr<PdfDocument> openFixture(PdfEngine& engine, const char* name) {
    auto opened = engine.openDocument(fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / name);
    CHECK(opened.has_value());
    if (!opened.has_value()) {
        return nullptr;
    }
    return std::move(opened.value());
}

// Extracts page 0, or null (after a failed CHECK) on error.
std::shared_ptr<const PdfTextPage> textPageOrBail(PdfDocument& doc) {
    auto page = doc.textPage(0);
    CHECK(page.has_value());
    if (!page.has_value()) {
        return nullptr;
    }
    return page.value();
}

std::size_t findChar(const PdfTextPage& page, char32_t unicode) {
    for (std::size_t i = 0; i < page.chars().size(); ++i) {
        if (page.chars()[i].unicode == unicode) {
            return i;
        }
    }
    return page.chars().size(); // not found
}

bool containsNonEmptyWithin(const PdfTextPage& page, const core::Rect& bounds) {
    for (const rivet::pdf::TextChar& ch : page.chars()) {
        if (ch.bounds.isEmpty()) {
            continue;
        }
        if (!bounds.containsRect(ch.bounds)) {
            return false;
        }
    }
    return true;
}

// Collapses every whitespace run into a single space (PDFium's extractor
// collapses duplicate spaces per line, so fixture-vs-text comparisons are
// whitespace-normalized).
std::string normalized(const std::string& text) {
    std::string out;
    bool inSpace = false;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            inSpace = true;
            continue;
        }
        if (inSpace && !out.empty()) {
            out.push_back(' ');
        }
        inSpace = false;
        out.push_back(c);
    }
    return out;
}

} // namespace

// text-basic.pdf: two lines, no rotation. Extraction returns both lines, the
// 'H' display box hugs the displayed baseline (user baseline y = 720 maps to
// display y = 792 - 720 = 72), every char box is inside the page, and the
// full-range rects merge into one rect per line.
RIVET_TEST(pdfiumTextBasic) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "text-basic.pdf");
    if (!doc) {
        return;
    }
    auto page = textPageOrBail(*doc);
    if (!page) {
        return;
    }

    CHECK_GT(page->charCount(), std::size_t{0});
    CHECK(page->text().find("Hello Rivet") != std::string::npos);
    CHECK(page->text().find("9.5") != std::string::npos);

    // The first char is the 'H' of "Hello Rivet" at 24 pt.
    const rivet::pdf::TextChar& first = page->chars().front();
    CHECK_EQ(first.unicode, static_cast<char32_t>(U'H'));
    CHECK_NEAR(first.fontSize, 24.0, 0.01);
    // Display baseline: y = 792 - 720 = 72. The 'H' box bottom (maxY in
    // y-down display space) sits on it; the box rises above it (smaller y).
    CHECK_NEAR(first.bounds.minX(), 72.0, kPositionEpsilon);
    CHECK_NEAR(first.bounds.maxY(), 72.0, kPositionEpsilon);
    CHECK_GE(first.bounds.minY(), 72.0 - 24.0); // within one em above baseline
    CHECK_LT(first.bounds.minY(), first.bounds.maxY());
    CHECK_LE(first.bounds.size.height, 24.0);

    // The '9' of the 9.5 pt tail: baseline user y = 650 -> display 142.
    const std::size_t nine = findChar(*page, static_cast<char32_t>(U'9'));
    CHECK_LT(nine, page->charCount());
    CHECK_NEAR(page->chars()[nine].fontSize, 9.5, 0.01);
    CHECK_NEAR(page->chars()[nine].bounds.maxY(), 142.0, kPositionEpsilon);

    // All char boxes land inside the displayed page (clamped).
    CHECK(containsNonEmptyWithin(*page, core::Rect{0.0, 0.0, 612.0, 792.0}));

    // One rect per line: at least two rows, first line above the second.
    const std::vector<core::Rect> rects = page->rectsForRange(0, static_cast<std::uint32_t>(page->charCount()));
    CHECK_GE(rects.size(), std::size_t{2});
    for (std::size_t i = 1; i < rects.size(); ++i) {
        CHECK_LT(rects[i - 1].maxY(), rects[i].minY());
    }
}

// text-cyrillic.pdf: the /Differences /uniXXXX encoding extracts exact
// Cyrillic code points, and Unicode case folding drives the search.
RIVET_TEST(pdfiumTextCyrillic) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "text-cyrillic.pdf");
    if (!doc) {
        return;
    }
    auto page = textPageOrBail(*doc);
    if (!page) {
        return;
    }

    CHECK(page->text().find("Привет") != std::string::npos);

    // "ривет" (lowercase) matches inside "Привет": exactly one hit.
    std::vector<TextSearchResult> hits =
        rivet::pdf::searchTextPage(*page, "ривет", TextSearchOptions{false, false});
    CHECK_EQ(hits.size(), std::size_t{1});
    CHECK_EQ(hits[0].count, 5u);
    CHECK_EQ(page->chars()[hits[0].startIndex].unicode, static_cast<char32_t>(0x0440)); // р

    // Uppercase "ПРИВЕТ" matches only case-insensitively.
    CHECK_EQ(rivet::pdf::searchTextPage(*page, "ПРИВЕТ", TextSearchOptions{false, false}).size(),
             std::size_t{1});
    CHECK(rivet::pdf::searchTextPage(*page, "ПРИВЕТ", TextSearchOptions{true, false}).empty());
}

// text-multiline.pdf: four lines -> four row rects; PDFium's generated \r\n
// line breaks have zero-area boxes; duplicate spaces are collapsed by the
// extractor, so the punctuation line is compared whitespace-normalized.
RIVET_TEST(pdfiumTextMultiline) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "text-multiline.pdf");
    if (!doc) {
        return;
    }
    auto page = textPageOrBail(*doc);
    if (!page) {
        return;
    }

    // Four lines -> exactly four rows (one rect per row over the full range).
    const std::vector<core::Rect> rects =
        page->rectsForRange(0, static_cast<std::uint32_t>(page->charCount()));
    CHECK_GE(rects.size(), std::size_t{3});
    CHECK_EQ(rects.size(), std::size_t{4});
    for (std::size_t i = 1; i < rects.size(); ++i) {
        CHECK_LT(rects[i - 1].maxY(), rects[i].minY());
    }

    // Spaces are preserved modulo the extractor's duplicate-space collapsing.
    const std::string normalizedText = normalized(page->text());
    CHECK(normalizedText.find("Hello, world! Two spaces.") != std::string::npos);

    // Every generated line break (CR or LF) has an empty (zero-area) box.
    std::size_t lineBreaks = 0;
    for (const rivet::pdf::TextChar& ch : page->chars()) {
        if (ch.unicode == 0x0D || ch.unicode == 0x0A) {
            ++lineBreaks;
            CHECK(ch.bounds.isEmpty());
        }
    }
    CHECK_GE(lineBreaks, std::size_t{1});
}

// text-rot90.pdf: with /Rotate 90 the user-space line at (72, 720) runs
// vertically near display x = 720 (user baseline y -> display x) starting at
// display y = 72 (user x -> display y), reading order walking downward. This
// pins the turn-1 mapping of the user->display transform (the same clockwise
// convention the rot90 render fixture pinned in Phase 1).
RIVET_TEST(pdfiumTextRot90) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "text-rot90.pdf");
    if (!doc) {
        return;
    }
    auto page = textPageOrBail(*doc);
    if (!page) {
        return;
    }

    // Displayed page is 792x612 (rotation swaps the dimensions).
    auto info = doc->pageInfo(0);
    CHECK(info.has_value());
    if (info.has_value()) {
        CHECK_EQ(info->rotation, core::PageRotation::Clockwise90);
        CHECK_NEAR(info->sizePoints.width, 792.0, 0.5);
        CHECK_NEAR(info->sizePoints.height, 612.0, 0.5);
    }

    CHECK(page->text().find("Rotated") != std::string::npos);
    CHECK_EQ(page->chars().front().unicode, static_cast<char32_t>(U'R'));
    const rivet::pdf::TextChar& first = page->chars().front();

    // Turn-1 mapping: displayX = userY - cropBottom = 720; displayY = userX -
    // cropLeft = 72. The 'R' box starts at that corner and extends one glyph
    // into the page (display x grows with the glyph's height above baseline,
    // display y with its advance along the line).
    CHECK_NEAR(first.bounds.minX(), 720.0, kPositionEpsilon);
    CHECK_NEAR(first.bounds.minY(), 72.0, kPositionEpsilon);
    CHECK_GT(first.bounds.maxX(), first.bounds.minX());
    CHECK_GT(first.bounds.maxY(), first.bounds.minY());
    CHECK_LE(first.bounds.maxX(), 720.0 + 20.0 + kPositionEpsilon);
    CHECK_LE(first.bounds.maxY(), 72.0 + 20.0 + kPositionEpsilon);

    // Row orientation: reading order walks DOWNWARD (y-centers strictly
    // increase with char index), with the line pinned near display x = 720.
    double previousY = -1.0;
    for (const rivet::pdf::TextChar& ch : page->chars()) {
        if (ch.bounds.isEmpty()) {
            continue;
        }
        CHECK_GT(ch.bounds.center().y, previousY);
        previousY = ch.bounds.center().y;
        CHECK_GT(ch.bounds.center().x, 720.0 - kPositionEpsilon);
        CHECK_LT(ch.bounds.center().x, 720.0 + 20.0 + kPositionEpsilon);
    }
    CHECK_GT(previousY, 72.0); // the line made vertical progress
}

// Range errors are typed, and the corrupted fixture never crashes extraction.
RIVET_TEST(pdfiumTextRejectsBadInput) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "text-basic.pdf");
    if (!doc) {
        return;
    }

    auto outOfRange = doc->textPage(1); // one-page document
    CHECK(!outOfRange.has_value());
    CHECK_EQ(outOfRange.error().code, core::ErrorCode::InvalidArgument);
    auto wayOut = doc->textPage(99);
    CHECK(!wayOut.has_value());
    CHECK_EQ(wayOut.error().code, core::ErrorCode::InvalidArgument);

    // corrupted.pdf: opening fails (InvalidDocument); if a PDFium build ever
    // accepted the garbage, extraction must still fail cleanly instead of
    // crashing.
    auto corrupted = engine->openDocument(fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / "corrupted.pdf");
    if (corrupted.has_value()) {
        auto page = corrupted.value()->textPage(0);
        CHECK(!page.has_value());
    } else {
        CHECK_EQ(corrupted.error().code, core::ErrorCode::InvalidDocument);
    }
}

// The extracted page is detached plain data: it stays valid and usable after
// the document handle is gone (no engine handles behind it).
RIVET_TEST(pdfiumTextPageOutlivesDocument) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    std::shared_ptr<const PdfTextPage> page;
    {
        auto doc = openFixture(*engine, "text-basic.pdf");
        if (!doc) {
            return;
        }
        page = textPageOrBail(*doc);
        if (!page) {
            return;
        }
    }
    CHECK_GT(page->charCount(), std::size_t{0});
    CHECK(page->text().find("Hello Rivet") != std::string::npos);
    // And it can still be searched after the document is closed.
    CHECK_EQ(rivet::pdf::searchTextPage(*page, "Rivet").size(), std::size_t{1});
}
