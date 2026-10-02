// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "pdf/BundledFonts.hpp"

// PDFium-free tests of the bundled-font tables (data, names, coverage,
// fallback selection). Both build modes.

namespace {
using namespace rivet::pdf;
using rivet::test::utf32;
} // namespace

// --- Bundled fonts -----------------------------------------------------------

RIVET_TEST(bundledFontsDataAndNames) {
    const PdfBundledFont all[] = {PdfBundledFont::SansRegular,  PdfBundledFont::SansBold,
                                  PdfBundledFont::SerifRegular, PdfBundledFont::SerifBold,
                                  PdfBundledFont::MonoRegular,  PdfBundledFont::MonoBold};
    for (const PdfBundledFont font : all) {
        const auto data = bundledFontData(font);
        CHECK_GT(data.size(), std::size_t{10000});
        // TrueType: sfnt version 0x00010000.
        CHECK(data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00);
        CHECK(!bundledFontDisplayName(font).empty());
    }
    CHECK(bundledFontDisplayName(PdfBundledFont::SansBold) == "Arimo Bold");
    CHECK(bundledFontDisplayName(PdfBundledFont::SerifRegular) == "Tinos Regular");
    CHECK(bundledFontDisplayName(PdfBundledFont::MonoRegular) == "Cousine Regular");
}

RIVET_TEST(bundledFontsCoverage) {
    const PdfBundledFont all[] = {PdfBundledFont::SansRegular,  PdfBundledFont::SansBold,
                                  PdfBundledFont::SerifRegular, PdfBundledFont::SerifBold,
                                  PdfBundledFont::MonoRegular,  PdfBundledFont::MonoBold};
    for (const PdfBundledFont font : all) {
        for (const char32_t c : utf32("Hello, World! 0123456789")) CHECK(bundledFontCovers(font, c));
        // Latin-1 and Latin Extended-A.
        for (const char32_t c : utf32("\xC3\xA9\xC3\xBC\xC3\x9F\xC3\xB1\xC5\x82\xC5\x91\xC4\x8D")) {
            CHECK(bundledFontCovers(font, c));
        }
        // Cyrillic and Greek.
        for (const char32_t c : utf32("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82")) {
            CHECK(bundledFontCovers(font, c));
        }
        for (const char32_t c : utf32("\xCE\xB1\xCE\xB2\xCE\xB3\xCE\xA9")) CHECK(bundledFontCovers(font, c));
        // Spaces and line breaks are always accepted.
        CHECK(bundledFontCovers(font, U' '));
        CHECK(bundledFontCovers(font, U'\n'));
        CHECK(bundledFontCovers(font, U'\r'));
        CHECK(bundledFontCovers(font, U'\t'));
        // CJK, emoji, Arabic, Hebrew, Devanagari, private use and out-of-range are not.
        CHECK(!bundledFontCovers(font, 0x4E2D));
        CHECK(!bundledFontCovers(font, 0x3042));
        CHECK(!bundledFontCovers(font, 0x1F600));
        CHECK(!bundledFontCovers(font, 0x0627));
        CHECK(!bundledFontCovers(font, 0x05D0));
        CHECK(!bundledFontCovers(font, 0x0905));
        CHECK(!bundledFontCovers(font, 0xE000));
        CHECK(!bundledFontCovers(font, 0x110000));
        CHECK(!bundledFontCovers(font, 0x0000));
        CHECK(!bundledFontCovers(font, 0x007F));
    }
}

RIVET_TEST(bundledFontsFallbackFor) {
    PdfFontInfo info;
    CHECK(fallbackFor(info) == PdfBundledFont::SansRegular);
    info.bold = true;
    CHECK(fallbackFor(info) == PdfBundledFont::SansBold);
    info.serif = true;
    CHECK(fallbackFor(info) == PdfBundledFont::SerifBold);
    info.bold = false;
    CHECK(fallbackFor(info) == PdfBundledFont::SerifRegular);
    info.monospace = true; // monospace wins over serif
    CHECK(fallbackFor(info) == PdfBundledFont::MonoRegular);
    info.bold = true;
    CHECK(fallbackFor(info) == PdfBundledFont::MonoBold);
    // Italic does not select a face (no italic faces are bundled).
    PdfFontInfo italic;
    italic.italic = true;
    CHECK(fallbackFor(italic) == PdfBundledFont::SansRegular);
}
