// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "pdf/BundledFontMetrics.hpp"
#include "pdf/BundledFonts.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

// PDFium-free tests of the portable bundled-font metrics (ADR-0023). Both
// build modes.

namespace {
using namespace rivet::pdf;
using rivet::test::utf32;

constexpr PdfBundledFont kAll[] = {PdfBundledFont::SansRegular,  PdfBundledFont::SansBold,
                                   PdfBundledFont::SerifRegular, PdfBundledFont::SerifBold,
                                   PdfBundledFont::MonoRegular,  PdfBundledFont::MonoBold};
} // namespace

RIVET_TEST(fontMetricsBasics) {
    for (const PdfBundledFont font : kAll) {
        const BundledFontMetrics& m = bundledFontMetrics(font);
        CHECK(m.valid());
        CHECK_EQ(m.unitsPerEm(), 2048.0);
        CHECK_GT(m.ascent(), 0.0);
        CHECK_LT(m.descent(), 0.0);
        CHECK_GE(m.lineGap(), 0.0);
        CHECK_GT(m.advance(U'A'), 0.0);
        CHECK(m.hasGlyph(U' '));
        CHECK_GT(m.advance(U' '), 0.0);
        // Same object on every call (parsed once).
        CHECK(&bundledFontMetrics(font) == &m);
    }
    // Monospace: every covered glyph has the same advance.
    const BundledFontMetrics& mono = bundledFontMetrics(PdfBundledFont::MonoRegular);
    CHECK_EQ(mono.advance(U'i'), mono.advance(U'W'));
    // Proportional: they differ.
    const BundledFontMetrics& sans = bundledFontMetrics(PdfBundledFont::SansRegular);
    CHECK_LT(sans.advance(U'i'), sans.advance(U'W'));
    // Arimo is metric-compatible with Arial: 'A' is 1366 units.
    CHECK_EQ(sans.advance(U'A'), 1366.0);
    CHECK_EQ(sans.pdfWidth(U'A'), 666.0); // floor(1366 * 1000 / 2048)
}

RIVET_TEST(fontMetricsAgreeWithParsingAFreshCopy) {
    for (const PdfBundledFont font : kAll) {
        const BundledFontMetrics fresh(bundledFontData(font));
        CHECK(fresh.valid());
        CHECK_EQ(fresh.advance(U'A'), bundledFontMetrics(font).advance(U'A'));
        CHECK_EQ(fresh.ascent(), bundledFontMetrics(font).ascent());
    }
}

RIVET_TEST(fontMetricsCoverageMatchesBundledTables) {
    // Latin, Latin-1, Latin Extended-A, Greek, Cyrillic, punctuation, and an
    // uncovered CJK character and emoji.
    const std::u32string sample =
        utf32("AZaz09!?.,;:()[]{}@#%&*+-=/\\\"'"
              "\xC3\xA9\xC3\xBC\xC3\x9F\xC3\xB1\xC5\x82\xC5\x91\xC4\x8D"
              "\xCE\x91\xCE\xA9\xCE\xB1\xCF\x89\xCE\xAC"
              "\xD0\x90\xD0\xAF\xD0\xB0\xD1\x8F\xD1\x91"
              "\xE4\xB8\xAD\xF0\x9F\x98\x80");
    for (const PdfBundledFont font : kAll) {
        const BundledFontMetrics& m = bundledFontMetrics(font);
        for (const char32_t cp : sample) {
            CHECK_EQ(m.hasGlyph(cp), bundledFontCovers(font, cp));
            CHECK_EQ(m.advance(cp) > 0.0, bundledFontCovers(font, cp));
            CHECK_EQ(m.pdfWidth(cp) > 0.0, bundledFontCovers(font, cp));
        }
        CHECK(!m.hasGlyph(0x4E2D));
        CHECK_EQ(m.advance(0x4E2D), 0.0);
    }
}

RIVET_TEST(fontMetricsTextWidth) {
    const BundledFontMetrics& m = bundledFontMetrics(PdfBundledFont::SerifBold);
    const double size = 11.0;
    const std::string a = "Hello, ";
    const std::string b = "\xD0\x9C\xD0\xB8\xD1\x80 \xCE\xBA\xCF\x8C\xCF\x83\xCE\xBC\xCE\xBF\xCF\x82";
    CHECK_NEAR(m.textWidth(a + b, size), m.textWidth(a, size) + m.textWidth(b, size), 1e-9);
    // PDF widths: advances truncated to integer thousandths of an em.
    CHECK_EQ(m.pdfWidth(U'A'), std::floor(m.advance(U'A') * 1000.0 / m.unitsPerEm()));
    CHECK_NEAR(m.textWidth("A", size), m.pdfWidth(U'A') * size / 1000.0, 1e-12);
    CHECK_NEAR(m.textWidth("A", size), m.advance(U'A') * size / m.unitsPerEm(), size / 1000.0);
    CHECK_NEAR(m.textWidth("AA", 22.0), 4.0 * m.textWidth("A", 11.0), 1e-9);
    CHECK_EQ(m.textWidth("", size), 0.0);
    // Uncovered code points count as 0.
    CHECK_NEAR(m.textWidth("A\xE4\xB8\xAD", size), m.textWidth("A", size), 1e-12);
}

RIVET_TEST(fontMetricsRejectMalformedPrograms) {
    const std::vector<std::uint8_t> empty;
    CHECK(!BundledFontMetrics(empty).valid());
    const auto data = bundledFontData(PdfBundledFont::SansRegular);
    // Every truncation must be handled without reading out of bounds.
    for (const std::size_t length : {std::size_t{4}, std::size_t{12}, std::size_t{100}, std::size_t{1000},
                                     data.size() / 2}) {
        const BundledFontMetrics m(data.first(length));
        CHECK_EQ(m.advance(U'A') >= 0.0, true);
        CHECK_EQ(m.textWidth("abc", 10.0) >= 0.0, true);
    }
    // Garbage with a plausible header.
    std::vector<std::uint8_t> garbage(data.begin(), data.begin() + 2048);
    for (std::size_t i = 12; i < garbage.size(); ++i) garbage[i] = static_cast<std::uint8_t>(i * 131u);
    const BundledFontMetrics m(garbage);
    CHECK(!m.valid());
    CHECK(!m.hasGlyph(U'A'));
}
