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

// --- Math face (ADR-0032) ------------------------------------------------------

RIVET_TEST(bundledMathFontDataNameAndCoverage) {
    const auto data = bundledFontData(PdfBundledFont::MathRegular);
    CHECK_GT(data.size(), std::size_t{10000});
    CHECK(data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00);
    CHECK(bundledFontDisplayName(PdfBundledFont::MathRegular) == "Noto Sans Math Regular");
    CHECK_EQ(kBundledFontCount, static_cast<std::size_t>(PdfBundledFont::MathRegular) + 1);
    // What the math engine emits: ASCII operators, Greek, big operators,
    // relations, arrows, radicals, mathematical italic/bold letters (astral
    // plane), double-struck capitals, accents.
    const char32_t used[] = {U'+',    U'=',    U'(',    U'[',    U'{',    U'|',    U'1',    0x03B1, 0x03A9,
                             0x03D5,  0x2211,  0x220F,  0x222B,  0x221A,  0x2264,  0x2265,  0x2260, 0x2248,
                             0x2192,  0x221E,  0x22C5,  0x00D7,  0x00B1,  0x2208,  0x2282,  0x2202, 0x2207,
                             0x1D44E, 0x1D467, 0x210E,  0x1D400, 0x1D6FC, 0x1D7CE, 0x211D,  0x2115, U'^',
                             U'~',    0x2032,  0x27E8,  0x27E9,  0x2016,  0x22EF,  0x2026,  0x2212, 0x1D49C};
    for (const char32_t c : used) CHECK(bundledFontCovers(PdfBundledFont::MathRegular, c));
    CHECK(!bundledFontCovers(PdfBundledFont::MathRegular, 0x4E2D));
    CHECK(!bundledFontCovers(PdfBundledFont::MathRegular, 0x0416)); // no Cyrillic
    CHECK(!bundledFontCovers(PdfBundledFont::MathRegular, 0x1F600));
    // Noto Sans Math has no spacing modifier accents (the engine uses ASCII
    // ^ and ~, rules and small operators instead).
    CHECK(!bundledFontCovers(PdfBundledFont::MathRegular, 0x02C6));
    // The text faces do not cover the math alphanumerics.
    CHECK(!bundledFontCovers(PdfBundledFont::SansRegular, 0x1D44E));
}
