#include "RivetTest.h"

// PdfSystem.hpp first: it re-exports core::Result into rivet::pdf, which the
// frozen pdf/PdfEngine.hpp interface header refers to.
#include "pdf/PdfSystem.hpp"

#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#ifndef RIVET_PDF_RENDER_FIXTURE_DIR
// Fallback for builds outside CMake; the CMake build defines the absolute
// path (see tests/pdf/CMakeLists.txt).
#define RIVET_PDF_RENDER_FIXTURE_DIR "tests/pdf/fixtures"
#endif

// Rendering-contract tests against the deterministic geometry fixtures in
// tests/pdf/fixtures/ (see that directory's README.md and make_fixtures.py).
//
// Every fixture is a US-Letter page (MediaBox 612x792) whose displayed
// top-left quadrant is a large solid black rectangle and whose other three
// display quadrants each carry a small 20x20-point black square. Orientation
// is proven by asymmetry: the big quadrant must land where /Rotate says it
// does, and the small marks must land one per remaining quadrant / per tile.
//
// All tests runtime-branch so the suite passes in both build modes: with
// RIVET_WITH_PDFIUM=OFF the null backend cannot open documents, so the
// bodies return after the engine checks (same pattern as TestPdfSystem.cpp).

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using rivet::core::Bitmap;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;

// Page sizes come back as floats; dimensions are exact integers here.
constexpr double kSizeEpsilon = 0.5;

// Unrotated display-space geometry of corners.pdf (points, y-down).
constexpr double kPageW = 612.0;
constexpr double kPageH = 792.0;
constexpr double kQuadW = 306.0; // half of 612
constexpr double kQuadH = 396.0; // half of 792

constexpr std::size_t kMarkMinPixels = 100; // a 20x20 mark fills ~400 px
constexpr std::size_t kBigQuadMinPixels =
    static_cast<std::size_t>(kQuadW * kQuadH / 2); // >50% of 306x396

// Returns the engine, or null in a PDFium-OFF build (after asserting the
// null-backend contract). Callers `return;` on null to skip the body.
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

// Opens a fixture from tests/pdf/fixtures; null (after a failed CHECK) on
// error, so callers can bail out.
std::unique_ptr<PdfDocument> openFixture(PdfEngine& engine, const char* name) {
    auto opened = engine.openDocument(fs::path(RIVET_PDF_RENDER_FIXTURE_DIR) / name);
    CHECK(opened.has_value());
    if (!opened.has_value()) {
        return nullptr;
    }
    return std::move(opened.value());
}

// Renders a display-space rect, or null (after a failed CHECK) on error.
std::unique_ptr<Bitmap> renderOrBail(PdfDocument& doc, const core::Rect& rect, double scale) {
    auto rendered = doc.renderPage(0, rect, scale);
    CHECK(rendered.has_value());
    if (!rendered.has_value()) {
        return nullptr;
    }
    return std::make_unique<Bitmap>(std::move(rendered.value()));
}

// Number of pixels in the device-pixel region whose B, G or R channel is
// below 250 (i.e. anything not essentially white; alpha is ignored - the
// renderer fills an opaque background, so it is always 255 today).
std::size_t countNonWhite(const Bitmap& bitmap, std::uint32_t px, std::uint32_t py,
                          std::uint32_t pw, std::uint32_t ph) {
    const std::uint32_t width = bitmap.width();
    const std::uint32_t height = bitmap.height();
    if (px >= width || py >= height) {
        return 0;
    }
    const std::uint32_t clippedW = std::min(pw, width - px);
    const std::uint32_t clippedH = std::min(ph, height - py);
    const std::size_t stride = bitmap.stride();
    const auto* base = reinterpret_cast<const std::uint8_t*>(bitmap.data());

    std::size_t count = 0;
    for (std::uint32_t y = 0; y < clippedH; ++y) {
        const auto* row = base + (static_cast<std::size_t>(py) + y) * stride +
                          static_cast<std::size_t>(px) * 4;
        for (std::uint32_t x = 0; x < clippedW; ++x) {
            if (row[x * 4 + 0] < 250 || row[x * 4 + 1] < 250 || row[x * 4 + 2] < 250) {
                ++count;
            }
        }
    }
    return count;
}

std::size_t countNonWhiteWhole(const Bitmap& bitmap) {
    return countNonWhite(bitmap, 0, 0, bitmap.width(), bitmap.height());
}

struct QuadrantCounts {
    std::size_t topLeft = 0;
    std::size_t topRight = 0;
    std::size_t bottomLeft = 0;
    std::size_t bottomRight = 0;
};

// Non-white counts of the four displayed-page quadrants (bitmap split at the
// middle of each axis).
QuadrantCounts quadrantNonWhite(const Bitmap& bitmap) {
    const std::uint32_t halfW = bitmap.width() / 2;
    const std::uint32_t halfH = bitmap.height() / 2;
    QuadrantCounts counts;
    counts.topLeft = countNonWhite(bitmap, 0, 0, halfW, halfH);
    counts.topRight = countNonWhite(bitmap, halfW, 0, halfW, halfH);
    counts.bottomLeft = countNonWhite(bitmap, 0, halfH, halfW, halfH);
    counts.bottomRight = countNonWhite(bitmap, halfW, halfH, halfW, halfH);
    return counts;
}

// Shared assertions for one full-page corner render: exactly one quadrant is
// dark-heavy (the large rectangle), the other three carry a small mark, and
// the dark quadrant dominates by an order of magnitude.
void checkCornerPattern(const QuadrantCounts& counts, const char* darkQuadrant) {
    const std::string dark = darkQuadrant;
    CHECK_GT(counts.topRight, kMarkMinPixels);
    CHECK_GT(counts.bottomLeft, kMarkMinPixels);
    CHECK_GT(counts.bottomRight, kMarkMinPixels);
    if (dark == "top-left") {
        CHECK_GT(counts.topLeft, kBigQuadMinPixels);
        CHECK_GT(counts.topLeft, counts.topRight * 10);
        CHECK_GT(counts.topLeft, counts.bottomLeft * 10);
    } else if (dark == "top-right") {
        CHECK_GT(counts.topRight, kBigQuadMinPixels);
        CHECK_GT(counts.topRight, counts.topLeft * 10);
        CHECK_GT(counts.topRight, counts.bottomRight * 10);
    } else if (dark == "bottom-left") {
        CHECK_GT(counts.bottomLeft, kBigQuadMinPixels);
        CHECK_GT(counts.bottomLeft, counts.topLeft * 10);
        CHECK_GT(counts.bottomLeft, counts.bottomRight * 10);
    } else { // "bottom-right"
        CHECK_GT(counts.bottomRight, kBigQuadMinPixels);
        CHECK_GT(counts.bottomRight, counts.topLeft * 10);
        CHECK_GT(counts.bottomRight, counts.bottomLeft * 10);
    }
}

} // namespace

// Document-level contract: missing and malformed files are reported as typed
// errors (never crash), and each rotation fixture reports its post-rotation
// display size (FPDF_GetPageWidthF/HeightF swap width/height for /Rotate 90
// and 270) and quarter-turn.
RIVET_TEST(pdfiumRenderOpenAndPageInfo) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }

    auto missing = engine->openDocument(fs::path(RIVET_PDF_RENDER_FIXTURE_DIR) / "does_not_exist.pdf");
    CHECK(!missing.has_value());
    CHECK_EQ(missing.error().code, core::ErrorCode::NotFound);

    auto corrupted = engine->openDocument(fs::path(RIVET_PDF_RENDER_FIXTURE_DIR) / "corrupted.pdf");
    CHECK(!corrupted.has_value());
    CHECK_EQ(corrupted.error().code, core::ErrorCode::InvalidDocument);

    struct Expectation {
        const char* file;
        double width;
        double height;
        core::PageRotation rotation;
    };
    const Expectation expectations[] = {
        {"corners.pdf", 612.0, 792.0, core::PageRotation::None},
        {"rot90.pdf", 792.0, 612.0, core::PageRotation::Clockwise90},
        {"rot180.pdf", 612.0, 792.0, core::PageRotation::Clockwise180},
        {"rot270.pdf", 792.0, 612.0, core::PageRotation::Clockwise270},
    };

    for (const Expectation& expected : expectations) {
        auto doc = openFixture(*engine, expected.file);
        if (!doc) {
            return;
        }
        CHECK_EQ(doc->info().pageCount, std::size_t{1});
        CHECK_EQ(doc->info().isEncrypted, false);

        auto page = doc->pageInfo(0);
        CHECK(page.has_value());
        if (!page.has_value()) {
            return;
        }
        CHECK_NEAR(page->sizePoints.width, expected.width, kSizeEpsilon);
        CHECK_NEAR(page->sizePoints.height, expected.height, kSizeEpsilon);
        CHECK_EQ(page->rotation, expected.rotation);
    }
}

// corners.pdf full page at 1x: the top-left quadrant is dark-heavy, the other
// three carry their small marks, and the whole page is far from blank.
RIVET_TEST(pdfiumRenderCornersOrientation) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "corners.pdf");
    if (!doc) {
        return;
    }

    auto full = renderOrBail(*doc, core::Rect{0.0, 0.0, kPageW, kPageH}, 1.0);
    if (!full) {
        return;
    }
    CHECK_EQ(full->width(), 612u);
    CHECK_EQ(full->height(), 792u);

    const QuadrantCounts counts = quadrantNonWhite(*full);
    checkCornerPattern(counts, "top-left");

    // Non-blank page overall: big rectangle + three marks dwarf this bound.
    CHECK_GT(countNonWhiteWhole(*full), std::size_t{1000});
}

// /Rotate is clockwise (PDF spec): the large black quadrant moves from
// top-left to top-right (90), bottom-right (180) and bottom-left (270), with
// exactly one small mark left in each of the other three quadrants.
RIVET_TEST(pdfiumRenderRotationMovesQuadrantClockwise) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }

    struct Case {
        const char* file;
        const char* darkQuadrant; // where the big rectangle must land
    };
    const Case cases[] = {
        {"rot90.pdf", "top-right"},
        {"rot180.pdf", "bottom-right"},
        {"rot270.pdf", "bottom-left"},
    };

    for (const Case& testCase : cases) {
        auto doc = openFixture(*engine, testCase.file);
        if (!doc) {
            return;
        }
        const auto page = doc->pageInfo(0);
        CHECK(page.has_value());
        if (!page.has_value()) {
            return;
        }
        // Render the full displayed page at 1x.
        auto full = renderOrBail(*doc, core::Rect{0.0, 0.0, page->sizePoints.width,
                                                  page->sizePoints.height},
                                 1.0);
        if (!full) {
            return;
        }
        // /Rotate 90 and 270 swap the displayed dimensions.
        const bool swapped = page->rotation == core::PageRotation::Clockwise90 ||
                             page->rotation == core::PageRotation::Clockwise270;
        CHECK_EQ(full->width(), swapped ? 792u : 612u);
        CHECK_EQ(full->height(), swapped ? 612u : 792u);

        checkCornerPattern(quadrantNonWhite(*full), testCase.darkQuadrant);
    }
}

// Tile grid over corners.pdf at 1x: 612x792 points with 512-px tiles gives
// tilesX = ceil(612/512) = 2, tilesY = ceil(792/512) = 2; edge tiles are
// clipped to the page bounds (tile cell intersect page rect), exactly as
// PdfViewport requests them. Each clipped tile must come back with
// ceil(clipSize * scale) dimensions and contain its own region's mark.
RIVET_TEST(pdfiumRenderTileGrid) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "corners.pdf");
    if (!doc) {
        return;
    }

    struct Tile {
        double x;
        double y;
        double w;
        double h;
        std::uint32_t expectedW;
        std::uint32_t expectedH;
        bool darkHeavy;
    };
    // tile(0,0): [0,512]x[0,512] - holds most of the big black quadrant.
    // tile(1,0): [512,612]x[0,512] - the top-right quadrant's mark only.
    // tile(0,1): [0,512]x[512,792] - the bottom-left quadrant's mark only.
    // tile(1,1): [512,612]x[512,792] - the bottom-right quadrant's mark only.
    const Tile tiles[] = {
        {0.0, 0.0, 512.0, 512.0, 512u, 512u, true},
        {512.0, 0.0, 100.0, 512.0, 100u, 512u, false},
        {0.0, 512.0, 512.0, 280.0, 512u, 280u, false},
        {512.0, 512.0, 100.0, 280.0, 100u, 280u, false},
    };

    std::size_t darkTileCount = 0;
    std::size_t markTileCount = 0;
    for (const Tile& tile : tiles) {
        auto bitmap = renderOrBail(*doc, core::Rect{tile.x, tile.y, tile.w, tile.h}, 1.0);
        if (!bitmap) {
            return;
        }
        CHECK_EQ(bitmap->width(), tile.expectedW);
        CHECK_EQ(bitmap->height(), tile.expectedH);

        const std::size_t nonWhite = countNonWhiteWhole(*bitmap);
        if (tile.darkHeavy) {
            // The big quadrant alone is ~121k px of this 512x512 tile.
            CHECK_GT(nonWhite, tile.expectedW * tile.expectedH / 4);
            CHECK_GT(nonWhite, std::size_t{10000});
            ++darkTileCount;
        } else {
            CHECK_GE(nonWhite, kMarkMinPixels);
            ++markTileCount;
        }
    }
    CHECK_EQ(darkTileCount, std::size_t{1});
    CHECK_EQ(markTileCount, std::size_t{3});
}

// A rect smaller than one tile at a non-integer-friendly scale: dimensions
// are ceil(size * scale) and content lands in the bitmap (the rect sits
// inside the black quadrant, so the output must be far from white).
RIVET_TEST(pdfiumRenderSubTile) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "corners.pdf");
    if (!doc) {
        return;
    }

    auto tile = renderOrBail(*doc, core::Rect{100.0, 100.0, 100.0, 100.0}, 2.0);
    if (!tile) {
        return;
    }
    CHECK_EQ(tile->width(), 200u);
    CHECK_EQ(tile->height(), 200u);
    CHECK_GT(countNonWhiteWhole(*tile), std::size_t{1000});

    // Fractional geometry: dimensions round up, never down.
    auto fractional = renderOrBail(*doc, core::Rect{300.5, 400.25, 50.5, 33.25}, 2.0);
    if (!fractional) {
        return;
    }
    CHECK_EQ(fractional->width(), 101u);  // ceil(50.5 * 2)
    CHECK_EQ(fractional->height(), 67u);  // ceil(33.25 * 2)
}

// Caller bugs are rejected as InvalidArgument: rects exceeding the page
// bounds, non-positive / non-finite / over-cap scales and bad page indices.
RIVET_TEST(pdfiumRenderRejectsInvalidArguments) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "corners.pdf");
    if (!doc) {
        return;
    }
    const core::Rect unit{0.0, 0.0, 10.0, 10.0};

    const core::Rect outOfBounds[] = {
        core::Rect{600.0, 780.0, 20.0, 20.0},  // exceeds x and y bounds
        core::Rect{0.0, 790.0, 100.0, 100.0},  // exceeds y bound
        core::Rect{0.0, 0.0, 100000.0, 1.0},   // wildly out of bounds
    };
    for (const core::Rect& rect : outOfBounds) {
        auto rejected = doc->renderPage(0, rect, 1.0);
        CHECK(!rejected.has_value());
        CHECK_EQ(rejected.error().code, core::ErrorCode::InvalidArgument);
    }

    const double badScales[] = {0.0, -1.0, std::nan(""), 1.0e308 * 10.0, 512.0 * 2.0};
    for (const double scale : badScales) {
        auto rejected = doc->renderPage(0, unit, scale);
        CHECK(!rejected.has_value());
        CHECK_EQ(rejected.error().code, core::ErrorCode::InvalidArgument);
    }

    auto badIndex = doc->renderPage(99, unit, 1.0);
    CHECK(!badIndex.has_value());
    CHECK_EQ(badIndex.error().code, core::ErrorCode::InvalidArgument);
}

// Physical scale is honored exactly and rendering is deterministic: doubling
// the scale doubles the pixel dimensions, and re-rendering the same rect at
// the same scale reproduces identical dimensions and content.
RIVET_TEST(pdfiumRenderScaleAndDeterminism) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "corners.pdf");
    if (!doc) {
        return;
    }
    const core::Rect full{0.0, 0.0, kPageW, kPageH};

    auto oneX = renderOrBail(*doc, full, 1.0);
    auto twoX = renderOrBail(*doc, full, 2.0);
    if (!oneX || !twoX) {
        return;
    }
    CHECK_EQ(oneX->width(), 612u);
    CHECK_EQ(oneX->height(), 792u);
    CHECK_EQ(twoX->width(), 1224u);
    CHECK_EQ(twoX->height(), 1584u);
    CHECK_GT(countNonWhiteWhole(*twoX), countNonWhiteWhole(*oneX) / 2);

    auto repeat = renderOrBail(*doc, full, 2.0);
    if (!repeat) {
        return;
    }
    CHECK_EQ(repeat->width(), twoX->width());
    CHECK_EQ(repeat->height(), twoX->height());
    CHECK_EQ(countNonWhiteWhole(*repeat), countNonWhiteWhole(*twoX));
}

// Straight-alpha contract: alpha.pdf fills a rectangle through an ExtGState
// with /ca 0.5 over the opaque white background, so its interior blends to a
// deterministic mid-gray (not white, not black) in every channel.
RIVET_TEST(pdfiumRenderAlphaBlendIsMidGray) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "alpha.pdf");
    if (!doc) {
        return;
    }

    auto full = renderOrBail(*doc, core::Rect{0.0, 0.0, kPageW, kPageH}, 1.0);
    if (!full) {
        return;
    }

    // Center of the alpha rectangle (display x 100..512, y 200..400): a
    // uniformly blended interior. Count pixels in the mid-gray window.
    const auto* base = reinterpret_cast<const std::uint8_t*>(full->data());
    const std::size_t stride = full->stride();
    std::size_t midGray = 0;
    const std::uint32_t sampled = 40;
    for (std::uint32_t y = 0; y < sampled; ++y) {
        for (std::uint32_t x = 0; x < sampled; ++x) {
            const auto* pixel = base + (280u + y) * stride + (296u + x) * 4;
            const bool channelsMatch = pixel[0] == pixel[1] && pixel[1] == pixel[2];
            const bool inWindow = pixel[1] >= 100 && pixel[1] <= 160;
            if (channelsMatch && inWindow) {
                ++midGray;
            }
        }
    }
    // Anti-aliasing cannot reach this interior; essentially all pixels must
    // be the blend result.
    CHECK_GT(midGray, sampled * sampled * 9 / 10);
    // The blend is over an opaque background: alpha stays 255.
    const auto* center = base + 300u * stride + 316u * 4;
    CHECK_EQ(center[3], 255u);
}

// Print banding contract (editor::PrintSpooler): horizontal strips whose
// point rects start at row / density render to the SAME pixels as the full
// page - bands are at least the planned row count tall (at most one extra
// row from ceil rounding) and stitch without seams or offsets.
RIVET_TEST(pdfiumRenderPrintBandsStitchToFullPage) {
    auto engine = requirePdfiumEngine();
    if (!engine) {
        return;
    }
    auto doc = openFixture(*engine, "rot90.pdf"); // landscape, asymmetric
    if (!doc) {
        return;
    }
    const double density = 150.0 / 72.0;
    const double pageW = 792.0;
    const double pageH = 612.0;
    auto full = renderOrBail(*doc, core::Rect{0.0, 0.0, pageW, pageH}, density);
    if (!full) {
        return;
    }
    const std::uint32_t rowsPerBand = 97; // deliberately not a divisor
    const std::uint32_t totalRows = full->height();
    std::size_t differing = 0;
    for (std::uint32_t row = 0; row < totalRows; row += rowsPerBand) {
        const std::uint32_t endRow = std::min(row + rowsPerBand, totalRows);
        const double top = row / density;
        const double bottom = endRow == totalRows ? pageH : endRow / density;
        auto band = renderOrBail(*doc, core::Rect{0.0, top, pageW, bottom - top}, density);
        if (!band) {
            return;
        }
        CHECK_EQ(band->width(), full->width());
        CHECK_GE(band->height(), endRow - row);
        CHECK_LE(band->height(), endRow - row + 1);
        const auto* bandBytes = reinterpret_cast<const std::uint8_t*>(band->data());
        const auto* fullBytes = reinterpret_cast<const std::uint8_t*>(full->data());
        for (std::uint32_t y = 0; y < endRow - row; ++y) {
            for (std::size_t x = 0; x < std::size_t{full->width()} * 4; ++x) {
                const int a = bandBytes[y * band->stride() + x];
                const int b = fullBytes[(row + y) * full->stride() + x];
                if (std::abs(a - b) > 8) {
                    ++differing;
                }
            }
        }
    }
    CHECK_EQ(differing, std::size_t{0});
}
