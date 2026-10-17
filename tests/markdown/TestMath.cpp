// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "markdown/math/MathAst.hpp"
#include "markdown/math/MathDelimiters.hpp"
#include "markdown/math/MathScene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// TeX-subset math (ADR-0032): parser, box metrics of the layout, error
// handling, limits and the `$` money heuristic.

using namespace rivet::markdown::math;

namespace {

std::size_t codePoints(std::string_view s) {
    std::size_t n = 0;
    for (const char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
    }
    return n;
}

// Every code point is 0.5 em wide with ink from -0.2 em to 0.7 em, except a
// few with distinctive ink: the radical and parentheses are taller, '.' sits
// on the baseline, '^' and '~' float above it.
class FakeMathMeasurer final : public IMathMeasurer {
public:
    MathGlyphMetrics measure(std::string_view utf8, MathFont, double size) const override {
        MathGlyphMetrics m;
        m.width = static_cast<double>(codePoints(utf8)) * 0.5 * size;
        if (utf8 == "\xE2\x88\x9A" || utf8 == "(" || utf8 == ")" || utf8 == "[" || utf8 == "]" || utf8 == "{" ||
            utf8 == "}" || utf8 == "|") {
            m.ascent = 0.75 * size;
            m.descent = 0.25 * size;
        } else if (utf8 == "." || utf8 == "..") {
            m.ascent = 0.1 * size;
            m.descent = 0.0;
        } else if (utf8 == "^" || utf8 == "~") {
            m.ascent = 0.7 * size;
            m.descent = -0.5 * size;
        } else {
            m.ascent = 0.7 * size;
            m.descent = 0.2 * size;
        }
        return m;
    }
};

MathScene typeset(std::string_view tex, bool display = false, const MathOptions& options = {}) {
    FakeMathMeasurer m;
    auto scene = typesetMath(tex, display, m, options);
    if (!scene) throw std::runtime_error("typeset failed: " + scene.error());
    return *scene;
}

std::string u8(char32_t cp) {
    std::string s;
    if (cp < 0x80) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return s;
}

std::string italic(char c) { return u8(c == 'h' ? 0x210E : 0x1D44E + static_cast<char32_t>(c - 'a')); }

// The run containing `text` (as a whole run or as part of a merged run).
const MathGlyphRun& run(const MathScene& s, std::string_view text) {
    for (const MathGlyphRun& r : s.runs) {
        if (r.utf8 == text) return r;
    }
    for (const MathGlyphRun& r : s.runs) {
        if (r.utf8.find(text) != std::string::npos) return r;
    }
    throw std::runtime_error("no run");
}

bool hasRun(const MathScene& s, std::string_view text) {
    return std::any_of(s.runs.begin(), s.runs.end(),
                       [&](const MathGlyphRun& r) { return r.utf8.find(text) != std::string::npos; });
}

// x of `text` inside its (possibly merged) run.
double xOf(const MathScene& s, std::string_view text) {
    const MathGlyphRun& r = run(s, text);
    const std::size_t at = r.utf8.find(text);
    return r.origin.x + static_cast<double>(codePoints(std::string_view(r.utf8).substr(0, at))) * 0.5 * r.size;
}

constexpr double kSize = 15.0;
constexpr double kEps = 1e-9;

} // namespace

// ------------------------------------------------------------------- basics

RIVET_TEST(mathSingleLetterIsItalicMathAlphanumeric) {
    const MathScene s = typeset("x");
    CHECK_EQ(s.runs.size(), std::size_t{1});
    CHECK_EQ(s.runs[0].utf8, italic('x'));
    CHECK(s.runs[0].font == MathFont::Math);
    CHECK_EQ(s.runs[0].size, kSize);
    CHECK_NEAR(s.width, 0.5 * kSize, kEps);
    CHECK_NEAR(s.ascent, 0.7 * kSize, kEps);
    CHECK_NEAR(s.descent, 0.2 * kSize, kEps);
    CHECK(!s.hasErrors);
    // h is the Planck constant code point; digits stay upright.
    CHECK_EQ(typeset("h").runs[0].utf8, u8(0x210E));
    CHECK_EQ(typeset("2").runs[0].utf8, std::string("2"));
}

RIVET_TEST(mathAdjacentOrdinaryAtomsMergeIntoOneRun) {
    const MathScene s = typeset("abc");
    CHECK_EQ(s.runs.size(), std::size_t{1});
    CHECK_EQ(s.runs[0].utf8, italic('a') + italic('b') + italic('c'));
    CHECK_NEAR(s.runs[0].width, 1.5 * kSize, kEps);
}

RIVET_TEST(mathInterAtomSpacing) {
    const double mu = kSize / 18.0;
    // Binary: medium space on both sides.
    const MathScene bin = typeset("a+b");
    CHECK_NEAR(xOf(bin, "+") - 0.5 * kSize, 4.0 * mu, 1e-9);
    CHECK_NEAR(bin.width, 1.5 * kSize + 8.0 * mu, 1e-9);
    // Relation: thick space.
    const MathScene rel = typeset("a=b");
    CHECK_NEAR(rel.width, 1.5 * kSize + 10.0 * mu, 1e-9);
    // Unary minus is ordinary: no space; the minus sign is U+2212.
    const MathScene unary = typeset("-a");
    CHECK_NEAR(unary.width, 1.0 * kSize, 1e-9);
    CHECK(hasRun(unary, u8(0x2212)));
    // Binary before a relation becomes ordinary.
    const MathScene binRel = typeset("a+=b");
    CHECK_NEAR(binRel.width, 2.0 * kSize + 10.0 * mu, 1e-9);
    // Punctuation: thin space after.
    const MathScene punct = typeset("a,b");
    CHECK_NEAR(punct.width, 1.5 * kSize + 3.0 * mu, 1e-9);
    // Named operator then ordinary: thin space.
    const MathScene op = typeset("\\sin x");
    CHECK_NEAR(op.width, 4.0 * 0.5 * kSize + 3.0 * mu, 1e-9);
    // Explicit spaces.
    CHECK_NEAR(typeset("a\\quad b").width, 2.0 * kSize, 1e-9);
    CHECK_NEAR(typeset("a\\,b").width, 1.0 * kSize + 3.0 * mu, 1e-9);
    CHECK_NEAR(typeset("a\\!b").width, 1.0 * kSize - 3.0 * mu, 1e-9);
    // No medium/thick spaces in script style.
    const MathScene script = typeset("x^{a+b}");
    const MathGlyphRun& sup = run(script, italic('a'));
    CHECK_NEAR(xOf(script, "+") - sup.origin.x, 0.5 * sup.size, 1e-9);
}

RIVET_TEST(mathSuperscriptsAndSubscripts) {
    const MathScene sup = typeset("x^2");
    const MathGlyphRun& two = run(sup, "2");
    CHECK_NEAR(two.size, 0.7 * kSize, kEps);           // script size
    CHECK_LT(two.origin.y, 0.0);                        // raised
    CHECK_NEAR(-two.origin.y, 0.363 * kSize, 1e-9);     // text-style shift
    CHECK_GE(two.origin.x, 0.5 * kSize);                // after the base
    CHECK_GT(sup.ascent, 0.7 * kSize);

    const MathScene supD = typeset("x^2", true);
    CHECK_NEAR(-run(supD, "2").origin.y, 0.413 * kSize, 1e-9); // display shifts higher

    const MathScene sub = typeset("x_2");
    const MathGlyphRun& sub2 = run(sub, "2");
    CHECK_GT(sub2.origin.y, 0.0);
    CHECK_NEAR(sub2.origin.y, std::max(0.15 * kSize, 0.7 * 0.7 * kSize - 0.8 * 0.45 * kSize), 1e-9);
    CHECK_GT(sub.descent, 0.2 * kSize);

    // Both: the superscript is above the subscript with at least 4 rule
    // thicknesses between them.
    const MathScene both = typeset("x_1^2");
    const MathGlyphRun& s1 = run(both, "1");
    const MathGlyphRun& s2 = run(both, "2");
    const double supBottom = s2.origin.y + 0.2 * s2.size;
    const double subTop = s1.origin.y - 0.7 * s1.size;
    CHECK_GE(subTop - supBottom, 4.0 * 0.05 * kSize - 1e-9);
    CHECK_NEAR(s1.origin.x, s2.origin.x - 0.05 * kSize, 1e-9); // italic kern on the superscript only
}

RIVET_TEST(mathNestedScriptsShrink) {
    const MathScene s = typeset("x^{y^{z^{w}}}");
    CHECK_NEAR(run(s, italic('y')).size, 0.7 * kSize, kEps);
    CHECK_NEAR(run(s, italic('z')).size, 0.5 * kSize, kEps);
    CHECK_NEAR(run(s, italic('w')).size, 0.5 * kSize, kEps); // scriptscript is the floor
    CHECK_LT(run(s, italic('z')).origin.y, run(s, italic('y')).origin.y);
    // Primes are superscripts.
    const MathScene p = typeset("f''");
    CHECK(hasRun(p, u8(0x2033)));
    CHECK_LT(run(p, u8(0x2033)).origin.y, 0.0);
    const MathScene pp = typeset("f'^2");
    CHECK(hasRun(pp, u8(0x2032)));
    CHECK_NEAR(run(pp, "2").origin.y, run(pp, u8(0x2032)).origin.y, 1e-9);
}

RIVET_TEST(mathFractions) {
    const double axis = 0.25 * kSize;
    const MathScene d = typeset("\\frac{a}{b}", true);
    const MathGlyphRun& num = run(d, italic('a'));
    const MathGlyphRun& den = run(d, italic('b'));
    CHECK_EQ(num.size, kSize);    // display -> numerator in text style
    CHECK_LT(num.origin.y, -axis); // numerator raised above the axis
    CHECK_GT(den.origin.y, 0.0);   // denominator below the baseline
    CHECK_EQ(d.rules.size(), std::size_t{1});
    const auto& rule = d.rules[0].rect;
    CHECK_NEAR(rule.minY() + rule.size.height / 2.0, -axis, 1e-9); // bar on the axis
    CHECK_NEAR(rule.size.height, 0.05 * kSize, 1e-9);
    CHECK_NEAR(rule.size.width, 0.5 * kSize, 1e-9);
    // Clearance between numerator ink and the bar: at least 3 rule widths.
    CHECK_GE(rule.minY() - (num.origin.y + 0.2 * num.size), 3.0 * 0.05 * kSize - 1e-9);
    CHECK_GE((den.origin.y - 0.7 * den.size) - rule.maxY(), 3.0 * 0.05 * kSize - 1e-9);
    CHECK_NEAR(d.width, 0.5 * kSize + 2.0 * 0.12 * kSize, 1e-9);
    CHECK_GT(d.ascent, 0.7 * kSize);

    // Text style: smaller parts, smaller shifts.
    const MathScene t = typeset("\\frac{a}{b}");
    CHECK_NEAR(run(t, italic('a')).size, 0.7 * kSize, kEps);
    CHECK_LT(t.ascent, d.ascent);
    // \dfrac forces display, \tfrac text.
    CHECK_EQ(run(typeset("\\dfrac{a}{b}"), italic('a')).size, kSize);
    CHECK_NEAR(run(typeset("\\tfrac{a}{b}", true), italic('a')).size, 0.7 * kSize, kEps);
    // Wider numerator: the bar spans it, the denominator is centred.
    const MathScene w = typeset("\\frac{abcd}{b}", true);
    CHECK_NEAR(w.rules[0].rect.size.width, 2.0 * kSize, 1e-9);
    CHECK_NEAR(xOf(w, italic('b')) + 0.25 * kSize, w.rules[0].rect.minX() + kSize, 1e-9);
    // \binom: no bar, parentheses.
    const MathScene b = typeset("\\binom{n}{k}");
    CHECK(b.rules.empty());
    CHECK(hasRun(b, "("));
    CHECK(hasRun(b, ")"));
    // Nested fractions shrink.
    const MathScene n = typeset("\\frac{\\frac{a}{b}}{c}", true);
    CHECK_NEAR(run(n, italic('a')).size, 0.7 * kSize, kEps);
    CHECK_EQ(n.rules.size(), std::size_t{2});
}

RIVET_TEST(mathRadicals) {
    const MathScene s = typeset("\\sqrt{x}");
    CHECK(hasRun(s, u8(0x221A)));
    CHECK_EQ(s.rules.size(), std::size_t{1});
    const auto& bar = s.rules[0].rect;
    CHECK_LT(bar.maxY(), -0.7 * kSize); // above the body's ink
    CHECK_NEAR(bar.minX(), xOf(s, italic('x')), 1e-9);
    // The radical's ink top meets the bar.
    const MathGlyphRun& rad = run(s, u8(0x221A));
    CHECK_NEAR(rad.origin.y - 0.75 * rad.size, bar.minY(), 1e-9);
    // Taller bodies get a larger radical.
    const MathScene tall = typeset("\\sqrt{\\frac{a}{b}}", true);
    CHECK_GT(run(tall, u8(0x221A)).size, rad.size);
    // Index in scriptscript size, raised.
    const MathScene idx = typeset("\\sqrt[3]{x}");
    CHECK_NEAR(run(idx, "3").size, 0.5 * kSize, kEps);
    CHECK_LT(run(idx, "3").origin.y, 0.0);
    CHECK_LT(xOf(idx, "3"), xOf(idx, u8(0x221A)) + 1e-9);
}

RIVET_TEST(mathBigOperatorsAndLimits) {
    const std::string sum = u8(0x2211);
    // Display: limits centred under and over the enlarged operator.
    const MathScene d = typeset("\\sum_{i=1}^{n} x", true);
    const MathGlyphRun& op = run(d, sum);
    CHECK_NEAR(op.size, 1.4 * kSize, kEps);
    const MathGlyphRun& lower = run(d, "=");
    const MathGlyphRun& upper = run(d, italic('n'));
    CHECK_GT(lower.origin.y, 0.0);
    CHECK_LT(upper.origin.y, -0.7 * kSize);
    const double opCentre = op.origin.x + op.width / 2.0;
    CHECK_NEAR(upper.origin.x + upper.width / 2.0, opCentre, 1e-9);
    // Operator centred on the axis.
    CHECK_NEAR(op.origin.y + (0.2 - 0.7) / 2.0 * op.size, -0.25 * kSize, 1e-9);
    // Text style: scripts to the right, normal size.
    const MathScene t = typeset("\\sum_{i=1}^{n} i");
    CHECK_NEAR(run(t, sum).size, kSize, kEps);
    CHECK_GT(run(t, italic('n')).origin.x, run(t, sum).origin.x + run(t, sum).width - 1e-9);
    // \limits / \nolimits override the style.
    const MathScene tl = typeset("\\sum\\limits_{k} x");
    CHECK_GT(run(tl, italic('k')).origin.y, 0.3 * kSize);
    CHECK_LT(run(tl, italic('k')).origin.x, run(tl, sum).origin.x + run(tl, sum).width);
    const MathScene dn = typeset("\\sum\\nolimits_{k} x", true);
    CHECK_GE(run(dn, italic('k')).origin.x, run(dn, sum).origin.x + run(dn, sum).width - 1e-9);
    // Integrals take their scripts on the right even in display style.
    const MathScene integral = typeset("\\int_0^1 f", true);
    CHECK_GE(run(integral, "1").origin.x, run(integral, u8(0x222B)).origin.x + run(integral, u8(0x222B)).width - 1e-9);
    // \lim: a named operator with limits underneath in display style.
    const MathScene lim = typeset("\\lim_{x\\to 0} f", true);
    CHECK(hasRun(lim, "lim"));
    CHECK_GT(run(lim, italic('x')).origin.y, 0.0);
    CHECK_LT(run(lim, italic('x')).origin.x, run(lim, "lim").origin.x + run(lim, "lim").width);
    for (const char* name : {"\\prod", "\\coprod", "\\bigcup", "\\bigcap", "\\oint", "\\iint", "\\max", "\\min",
                             "\\sup", "\\inf", "\\det", "\\log", "\\exp", "\\operatorname{rank}",
                             "\\operatorname*{argmax}"}) {
        const MathScene s = typeset(std::string(name) + "_{a}^{b} x", true);
        CHECK(!s.hasErrors);
        CHECK_GT(s.width, 0.0);
    }
}

RIVET_TEST(mathDelimitersGrowWithContent) {
    const MathScene small = typeset("\\left( x \\right)");
    const MathScene frac = typeset("\\left( \\frac{a}{b} \\right)", true);
    const MathScene tall = typeset("\\left( \\frac{\\frac{a}{b}}{\\frac{c}{d}} \\right)", true);
    const double s1 = run(small, "(").size;
    const double s2 = run(frac, "(").size;
    const double s3 = run(tall, "(").size;
    CHECK_NEAR(s1, kSize, kEps); // never smaller than the base size
    CHECK_GT(s2, s1);
    CHECK_GT(s3, s2);
    // The delimiter covers (at least 90% of) the content around the axis.
    const MathGlyphRun& l = run(tall, "(");
    const double inkTop = -(l.origin.y - 0.75 * l.size);
    const double inkBottom = l.origin.y + 0.25 * l.size;
    CHECK_GE(inkTop + inkBottom, 0.9 * (tall.ascent + tall.descent) - 1e-9);
    // Both sides match; \right. is a null delimiter.
    CHECK_NEAR(run(tall, ")").size, s3, kEps);
    const MathScene oneSided = typeset("\\left\\{ x \\right.");
    CHECK(hasRun(oneSided, "{"));
    CHECK(!oneSided.hasErrors);
    for (const char* tex : {"\\left[ x \\right]", "\\left| x \\right|", "\\left\\| x \\right\\|",
                            "\\left\\langle x \\right\\rangle", "\\left\\lfloor x \\right\\rfloor",
                            "\\left\\lceil x \\right\\rceil", "\\left< x \\right>", "\\bigl( x \\bigr)",
                            "\\Big( x \\Big)", "\\bigg[ x \\bigg]", "\\Bigg\\{ x \\Bigg\\}"}) {
        const MathScene s = typeset(tex);
        CHECK(!s.hasErrors);
        CHECK(hasRun(s, italic('x')));
        CHECK_GT(s.width, 1.5 * kSize - 1e-9);
    }
    // \big sizes increase.
    CHECK_LT(run(typeset("\\big("), "(").size, run(typeset("\\Bigg("), "(").size);
}

RIVET_TEST(mathMatrices) {
    const MathScene p = typeset("\\begin{pmatrix} a & b \\\\ c & d \\end{pmatrix}");
    CHECK(!p.hasErrors);
    CHECK(hasRun(p, "("));
    CHECK(hasRun(p, ")"));
    const MathGlyphRun& a = run(p, italic('a'));
    const MathGlyphRun& b = run(p, italic('b'));
    const MathGlyphRun& c = run(p, italic('c'));
    CHECK_NEAR(a.origin.y, b.origin.y, kEps);
    CHECK_LT(a.origin.y, c.origin.y);
    CHECK_LT(a.origin.x, b.origin.x);
    CHECK_NEAR(a.origin.x, c.origin.x, kEps);
    // Centred on the axis.
    CHECK_NEAR((p.ascent - p.descent) / 2.0, 0.25 * kSize, 1e-9);
    CHECK_GT(run(p, "(").size, kSize); // delimiters span both rows
    // Other delimiter kinds.
    CHECK(hasRun(typeset("\\begin{bmatrix}1&2\\end{bmatrix}"), "["));
    CHECK(hasRun(typeset("\\begin{Bmatrix}1&2\\end{Bmatrix}"), "{"));
    CHECK(hasRun(typeset("\\begin{vmatrix}1&2\\end{vmatrix}"), "|"));
    CHECK(hasRun(typeset("\\begin{Vmatrix}1&2\\end{Vmatrix}"), u8(0x2016)));
    const MathScene plain = typeset("\\begin{matrix}1&2\\\\3&4\\end{matrix}");
    CHECK(!hasRun(plain, "("));
    CHECK_NEAR(run(typeset("\\begin{smallmatrix}1&2\\end{smallmatrix}"), "1").size, 0.7 * kSize, kEps);
    // Ragged rows are padded; a trailing \\ adds no row.
    const MathScene ragged = typeset("\\begin{matrix}1&2&3\\\\4\\\\\\end{matrix}");
    CHECK(!ragged.hasErrors);
    CHECK_LT(run(ragged, "1").origin.y, run(ragged, "4").origin.y);
    // cases: left brace only, left-aligned columns.
    const MathScene cases = typeset("f(x) = \\begin{cases} 1 & x > 0 \\\\ 100 & \\text{otherwise} \\end{cases}");
    CHECK(hasRun(cases, "{"));
    CHECK(!hasRun(cases, "}"));
    CHECK_NEAR(xOf(cases, "1"), xOf(cases, "100"), kEps);
    CHECK(hasRun(cases, "otherwise"));
    // aligned: the relations line up.
    const MathScene al = typeset("\\begin{aligned} a &= b + c \\\\ abc &= d \\end{aligned}", true);
    CHECK(!al.hasErrors);
    std::vector<double> eqs;
    for (const MathGlyphRun& r : al.runs) {
        if (r.utf8.find('=') != std::string::npos) eqs.push_back(r.origin.x);
    }
    CHECK_EQ(eqs.size(), std::size_t{2});
    if (eqs.size() == 2) CHECK_NEAR(eqs[0], eqs[1], kEps);
    // Thick space before the aligned relation.
    CHECK_NEAR(eqs.empty() ? 0.0 : eqs[0], 1.5 * kSize + 5.0 * kSize / 18.0, 1e-9);
    // array with a column spec, gathered.
    CHECK(!typeset("\\begin{array}{cc} 1 & 2 \\end{array}").hasErrors);
    CHECK(!typeset("\\begin{gathered} a \\\\ b \\end{gathered}", true).hasErrors);
}

RIVET_TEST(mathAccents) {
    const MathScene plain = typeset("x");
    const MathScene hat = typeset("\\hat{x}");
    CHECK(hasRun(hat, "^"));
    const MathGlyphRun& h = run(hat, "^");
    // The accent's ink bottom sits just above the body's ink.
    CHECK_NEAR(h.origin.y + (-0.5 * h.size), -(0.7 * kSize + 1.5 * 0.05 * kSize), 1e-9);
    CHECK_GT(hat.ascent, plain.ascent);
    CHECK_NEAR(hat.width, plain.width, kEps);
    // Italic bodies get the accent skewed right.
    CHECK_GT(h.origin.x + h.width / 2.0, plain.width / 2.0);
    const MathScene bar = typeset("\\bar{x}");
    CHECK_EQ(bar.rules.size(), std::size_t{1});
    CHECK_LT(bar.rules[0].rect.maxY(), -0.7 * kSize);
    const MathScene over = typeset("\\overline{abc}");
    CHECK_NEAR(over.rules[0].rect.size.width, 1.5 * kSize, kEps);
    const MathScene under = typeset("\\underline{ab}");
    CHECK_GT(under.rules[0].rect.minY(), 0.2 * kSize);
    CHECK_GT(under.descent, plain.descent);
    for (const char* tex : {"\\tilde{a}", "\\vec{v}", "\\dot{x}", "\\ddot{x}", "\\check{x}", "\\widehat{abc}",
                            "\\widetilde{xyz}", "\\overrightarrow{AB}"}) {
        const MathScene s = typeset(tex);
        CHECK(!s.hasErrors);
        CHECK_GT(s.ascent, plain.ascent);
        CHECK_GE(s.runs.size(), std::size_t{2});
    }
    // Wide hats grow with the body.
    CHECK_GT(run(typeset("\\widehat{abcdef}"), "^").size, run(typeset("\\hat{a}"), "^").size);
}

RIVET_TEST(mathFontsAndSymbols) {
    CHECK_EQ(run(typeset("\\alpha"), u8(0x1D6FC)).size, kSize);           // italic alpha
    CHECK(hasRun(typeset("\\Omega"), u8(0x03A9)));                        // upright capital
    CHECK(hasRun(typeset("\\varepsilon\\epsilon"), u8(0x1D700) + u8(0x1D716)));
    CHECK(hasRun(typeset("\\mathbf{x}"), u8(0x1D431)));
    CHECK(hasRun(typeset("\\mathbf{2}"), u8(0x1D7D0)));
    CHECK(hasRun(typeset("\\boldsymbol{x}"), u8(0x1D499)));
    CHECK(hasRun(typeset("\\mathbb{R}"), u8(0x211D)));
    CHECK(hasRun(typeset("\\mathbb{A}"), u8(0x1D538)));
    CHECK(hasRun(typeset("\\mathcal{L}"), u8(0x2112)));
    CHECK(hasRun(typeset("\\mathcal{A}"), u8(0x1D49C)));
    CHECK(hasRun(typeset("\\mathrm{d}x"), std::string("d")));
    CHECK(hasRun(typeset("\\mathit{x}"), italic('x')));
    CHECK(hasRun(typeset("\\sin x"), "sin"));
    for (const auto& [tex, cp] : std::vector<std::pair<std::string, char32_t>>{
             {"\\leq", 0x2264}, {"\\geq", 0x2265}, {"\\neq", 0x2260}, {"\\approx", 0x2248}, {"\\to", 0x2192},
             {"\\infty", 0x221E}, {"\\cdot", 0x22C5}, {"\\times", 0x00D7}, {"\\pm", 0x00B1}, {"\\in", 0x2208},
             {"\\subset", 0x2282}, {"\\Rightarrow", 0x21D2}, {"\\forall", 0x2200}, {"\\exists", 0x2203},
             {"\\nabla", 0x2207}, {"\\partial", 0x1D715}, {"\\ldots", 0x2026}, {"\\cdots", 0x22EF},
             {"\\not=", 0x2260}, {"\\not\\in", 0x2209}, {"\\langle", 0x27E8}, {"\\{", U'{'}, {"\\|", 0x2016}}) {
        const MathScene s = typeset("a" + tex + " b");
        CHECK(hasRun(s, u8(cp)));
        CHECK(!s.hasErrors);
    }
    // Unicode typed directly.
    const MathScene uni = typeset("\xCE\xB1 \xE2\x89\xA4 \xCE\xB2"); // α ≤ β
    CHECK(hasRun(uni, u8(0x1D6FC)));
    CHECK(hasRun(uni, u8(0x2264)));
    CHECK_NEAR(uni.width, 1.5 * kSize + 10.0 * kSize / 18.0, 1e-9);
}

RIVET_TEST(mathTextRuns) {
    const MathScene s = typeset("x \\text{ if } y > 0");
    const MathGlyphRun& t = run(s, " if ");
    CHECK(t.font == MathFont::Text);
    CHECK_EQ(t.size, kSize);
    CHECK(run(typeset("\\textbf{Bold}"), "Bold").font == MathFont::TextBold);
    // Escapes and nested braces inside text.
    CHECK(hasRun(typeset("\\text{a {b} \\$5}"), "a b $5"));
    // Text in scripts shrinks.
    CHECK_NEAR(run(typeset("x_{\\text{max}}"), "max").size, 0.7 * kSize, kEps);
}

RIVET_TEST(mathStylesAndPhantom) {
    CHECK_NEAR(run(typeset("\\scriptstyle x"), italic('x')).size, 0.7 * kSize, kEps);
    CHECK_NEAR(run(typeset("\\scriptscriptstyle x"), italic('x')).size, 0.5 * kSize, kEps);
    CHECK_EQ(run(typeset("\\displaystyle \\sum_a^b"), u8(0x2211)).size, 1.4 * kSize);
    const MathScene ph = typeset("a\\phantom{bbb}c");
    CHECK(!hasRun(ph, italic('b')));
    CHECK_NEAR(ph.width, 2.5 * kSize, kEps);
    // \color is accepted (colour ignored), \textcolor keeps its body.
    CHECK(!typeset("\\color{red} x").hasErrors);
    CHECK(hasRun(typeset("\\textcolor{blue}{y}"), italic('y')));
}

// ------------------------------------------------------------------- errors

RIVET_TEST(mathUnknownCommandsRenderLiterallyInErrorColour) {
    const MathScene s = typeset("\\foo + x");
    CHECK(s.hasErrors);
    const MathGlyphRun& lit = run(s, "\\foo");
    CHECK(lit.color == MathColor::Error);
    CHECK(lit.font == MathFont::Text);
    CHECK(hasRun(s, italic('x'))); // the rest still renders
    CHECK(run(s, italic('x')).color == MathColor::Normal);
    // Unknown environments, stray closers and missing pieces never fail.
    for (const char* tex : {"\\begin{tikzcd} a \\end{tikzcd}", "x}", "x^", "\\frac{a}", "\\frac", "\\sqrt",
                            "\\left( x", "x \\right)", "a & b", "\\end{matrix}", "{{x", "x^{2", "\\not\\foo",
                            "x^1^2", "x_1_2", "\\begin{pmatrix} a & b", "\\text{unterminated", "\\", "\\left",
                            "\\begin{matrix} a \\end{pmatrix}", "^2", "_"}) {
        FakeMathMeasurer m;
        const auto r = typesetMath(tex, false, m);
        CHECK(r.has_value());
        if (r) {
            CHECK(r->hasErrors);
            CHECK(std::isfinite(r->width) && std::isfinite(r->ascent) && std::isfinite(r->descent));
        }
    }
    // A stray closing brace is shown literally.
    CHECK(run(typeset("x}"), "}").color == MathColor::Error);
    // Well-formed input has no errors.
    CHECK(!typeset("\\frac{1}{2} + \\sqrt[3]{x^2} - \\left(\\sum_{i=0}^{n} a_i\\right)", true).hasErrors);
    // Empty input is an empty box.
    const MathScene empty = typeset("");
    CHECK(empty.runs.empty());
    CHECK_EQ(empty.width, 0.0);
}

RIVET_TEST(mathLimits) {
    FakeMathMeasurer m;
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "{";
    deep += "x";
    for (int i = 0; i < 40; ++i) deep += "}";
    CHECK(!typesetMath(deep, false, m).has_value());
    std::string ok;
    for (int i = 0; i < 20; ++i) ok += "{";
    ok += "x";
    for (int i = 0; i < 20; ++i) ok += "}";
    CHECK(typesetMath(ok, false, m).has_value());
    // Deeply nested fractions and scripts are bounded too.
    std::string fracs = "x";
    for (int i = 0; i < 40; ++i) fracs = "\\frac{" + fracs + "}{y}";
    CHECK(!typesetMath(fracs, false, m).has_value());
    MathOptions options;
    options.limits.maxSourceBytes = 10;
    CHECK(!typesetMath("abcdefghijk", false, m, options).has_value());
    options = {};
    options.limits.maxNodes = 10;
    CHECK(!typesetMath("abcdefghijklmnop", false, m, options).has_value());
    options = {};
    options.limits.maxMatrixCells = 3;
    CHECK(!typesetMath("\\begin{matrix}1&2\\\\3&4\\end{matrix}", false, m, options).has_value());
    // Default source limit.
    CHECK(!typesetMath(std::string(9000, 'x'), false, m).has_value());
    options = {};
    options.fontSize = 0.0;
    CHECK(!typesetMath("x", false, m, options).has_value());
}

RIVET_TEST(mathRobustAgainstGarbage) {
    FakeMathMeasurer m;
    const std::string alphabet = "\\{}^_&'$%~ ab12+=()[]|<>.,;:!?frac sqrt left right begin end text";
    std::uint32_t state = 99;
    for (int i = 0; i < 400; ++i) {
        std::string tex;
        const std::size_t len = 1 + (i % 40);
        for (std::size_t k = 0; k < len; ++k) {
            state = state * 1103515245u + 12345u;
            tex.push_back(alphabet[(state >> 16) % alphabet.size()]);
        }
        const auto r = typesetMath(tex, (i % 2) == 0, m);
        if (r) CHECK(std::isfinite(r->width) && r->width >= 0.0 && r->ascent >= 0.0 && r->descent >= 0.0);
    }
    // Invalid UTF-8 is replaced, never read out of bounds.
    CHECK(typesetMath("\xFF\xFE x \xC3", false, m).has_value());
}

RIVET_TEST(mathParserTree) {
    const auto parsed = parseMath("a_i^2 + \\frac{1}{x}");
    CHECK(parsed.has_value());
    if (!parsed) return;
    CHECK_EQ(parsed->list.size(), std::size_t{3});
    CHECK(parsed->list[0].kind == MathNode::Kind::Scripts);
    CHECK_GE(parsed->list[0].sub, 1);
    CHECK_GE(parsed->list[0].sup, 1);
    CHECK(parsed->list[1].cls == AtomClass::Bin);
    CHECK(parsed->list[2].kind == MathNode::Kind::Frac);
    const auto m = parseMath("\\begin{bmatrix}1&2&3\\\\4&5&6\\end{bmatrix}");
    CHECK(m.has_value());
    if (m) {
        CHECK(m->list[0].kind == MathNode::Kind::Matrix);
        CHECK(m->list[0].matrix == MatrixKind::Bracket);
        CHECK_EQ(m->list[0].rows, std::size_t{2});
        CHECK_EQ(m->list[0].columns, std::size_t{3});
        CHECK_EQ(m->list[0].children.size(), std::size_t{6});
    }
}

// ------------------------------------------------------------- `$` heuristic

RIVET_TEST(mathDollarHeuristic) {
    const auto spans = [](std::string_view text) {
        std::vector<std::string> out;
        for (const MathSpan& s : findMathSpans(text)) {
            out.push_back((s.display ? "D:" : "I:") + std::string(s.content(text)));
        }
        return out;
    };
    using V = std::vector<std::string>;
    CHECK(spans("Let $x$ be") == V{"I:x"});
    CHECK(spans("$a+b$ and $c$") == (V{"I:a+b", "I:c"}));
    CHECK(spans("It costs $5 and $10 today") == V{});
    CHECK(spans("between $5 and $10") == V{});
    CHECK(spans("pay $ 3 or $ 4") == V{});
    CHECK(spans("$x $ y") == V{});        // closing preceded by a space
    CHECK(spans("$x$5") == V{});          // closing followed by a digit
    CHECK(spans("$1+1$ is two") == V{"I:1+1"});
    CHECK(spans("\\$x$ escaped") == V{});
    CHECK(spans("$x\\$y$") == V{"I:x\\$y"});
    CHECK(spans("$$\\sum_i x_i$$") == V{"D:\\sum_i x_i"});
    CHECK(spans("a $$ x $$ b $y$") == (V{"D: x ", "I:y"}));
    CHECK(spans("$$$$") == V{});
    CHECK(spans("$$unterminated") == V{});
    CHECK(spans("$x\n\ny$") == V{});      // no blank line inside inline math
    CHECK(spans("$x\ny$") == V{"I:x\ny"});
    CHECK(spans("$5 then $x$") == V{"I:x"});
    CHECK(spans("") == V{});
    CHECK(spans("$") == V{});
    CHECK(spans("$$") == V{});

    const std::string_view t = "a $x$ b $ y$ c $z $ d $w$1";
    CHECK(isInlineMathSpan(t, 2, 4));
    CHECK(!isInlineMathSpan(t, 8, 11));   // opening followed by space
    CHECK(!isInlineMathSpan(t, 15, 18));  // closing preceded by space
    CHECK(!isInlineMathSpan(t, 22, 24));  // closing followed by a digit
    CHECK(!isInlineMathSpan(t, 2, 2));
    CHECK(!isInlineMathSpan(t, 4, 2));
    CHECK(!isInlineMathSpan(t, 0, 4));    // not a '$'
    CHECK(!isInlineMathSpan("$$", 0, 1)); // empty
    CHECK(!isInlineMathSpan(t, 2, 400));
}
