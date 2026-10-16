// SPDX-License-Identifier: MPL-2.0
#pragma once

// TeX-subset math typesetting for Markdown (ADR-0032): `$...$` / `$$...$$`
// source -> MathScene, a box of positioned glyph runs and rules that the
// Markdown painter and the PDF exporter both draw.
//
// Supported: sub/superscripts and primes; \frac \dfrac \tfrac \cfrac \binom;
// \sqrt[n]{}; big operators with limits (\sum \prod \coprod \int \iint \iiint
// \oint \bigcup \bigcap \bigoplus \bigotimes \bigvee \bigwedge, \limits,
// \nolimits) and named operators (\lim \max \min \sup \inf \det \gcd \Pr
// with limits; \sin \cos \log \ln \exp ... without; \operatorname[*]{});
// \left \right delimiters sized by content, \big \Big \bigg \Bigg (l/r/m);
// \text \textbf \textrm \mbox; \mathbf \boldsymbol \mathit \mathrm \mathsf
// \mathtt \mathbb \mathcal \mathscr; environments matrix pmatrix bmatrix
// Bmatrix vmatrix Vmatrix smallmatrix cases aligned align align* gathered
// split array; accents \hat \widehat \tilde \widetilde \bar \overline
// \underline \vec \overrightarrow \dot \ddot \check; Greek letters; common
// binary operators, relations, arrows and symbols; \not; spacing
// \, \: \; \! \quad \qquad; \displaystyle \textstyle \scriptstyle
// \scriptscriptstyle; \phantom; \color / \textcolor (colour ignored).
// Unknown commands render as their literal source in the Error colour.
//
// Layout follows TeX's math rules (The TeXbook, appendix G) with the four
// styles D/T/S/SS: inter-atom spacing, script shifts, fraction and radical
// clearances, operator limits, delimiter sizing; parameters in MathParams.
//
// Pure and thread-safe; may run on the async parse path.

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::markdown::math {

// Which face a run is drawn with. Math: the bundled math font (Noto Sans Math,
// PdfBundledFont::MathRegular): symbols, Greek, mathematical alphanumerics.
// Text / TextBold: the body text face (\text{}, \textbf{}, error literals).
enum class MathFont : std::uint8_t { Math, Text, TextBold };

enum class MathColor : std::uint8_t { Normal, Error };

struct MathGlyphMetrics {
    double width = 0.0;   // advance
    double ascent = 0.0;  // extent above the baseline
    double descent = 0.0; // extent below the baseline (negative when the ink is entirely above it)
};

// Measures one run of UTF-8 text set on a line in `font` at `size`. Must be
// pure and deterministic. ascent/descent should be the INK extents (glyph
// bounding boxes, as TeX uses); font-wide ascent/descent also work but
// typeset more loosely.
class IMathMeasurer {
public:
    virtual ~IMathMeasurer() = default;
    virtual MathGlyphMetrics measure(std::string_view utf8, MathFont font, double size) const = 0;
};

// Font parameters, in ems of the current style's size (TeX's fontdimens).
struct MathParams {
    double axisHeight = 0.25;
    double ruleThickness = 0.05;
    double xHeight = 0.45;
    double supShiftDisplay = 0.413;
    double supShiftText = 0.363;
    double supShiftCramped = 0.289;
    double subShift = 0.15;
    double subShiftWithSup = 0.247;
    double supDrop = 0.386;
    double subDrop = 0.05;
    double numShiftDisplay = 0.677;
    double numShiftText = 0.394;
    double denShiftDisplay = 0.686;
    double denShiftText = 0.345;
    double bigOpSpacing1 = 0.111;
    double bigOpSpacing2 = 0.167;
    double bigOpSpacing3 = 0.2;
    double bigOpSpacing4 = 0.6;
    double bigOpSpacing5 = 0.1;
    double delimiterFactor = 0.901;
    double delimiterShortfall = 0.5;
    double nullDelimiterSpace = 0.12;
    double scriptSpace = 0.05;
    double displayOperatorScale = 1.4; // big operator glyphs in display style
    double scriptScale = 0.7;
    double scriptScriptScale = 0.5;
    double maxDelimiterScale = 8.0;
};

struct MathLimits {
    std::size_t maxSourceBytes = 8 * 1024;
    std::size_t maxDepth = 32;    // group / argument nesting
    std::size_t maxNodes = 4096;  // parsed atoms
    std::size_t maxMatrixCells = 1024;
};

struct MathOptions {
    double fontSize = 15.0;
    MathParams params;
    MathLimits limits;
};

// One run of glyphs on a common baseline.
struct MathGlyphRun {
    std::string utf8;
    core::Point origin; // left end of the baseline; y relative to the formula baseline, down positive
    double width = 0.0;
    double size = 0.0;
    MathFont font = MathFont::Math;
    MathColor color = MathColor::Normal;
};

struct MathRule {
    core::Rect rect; // same coordinates as the runs
    MathColor color = MathColor::Normal;
};

// The formula box: x from 0 to width, the baseline at y = 0 (y grows down),
// ink between y = -ascent and y = +descent.
struct MathScene {
    std::vector<MathGlyphRun> runs;
    std::vector<MathRule> rules;
    double width = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
    bool hasErrors = false; // unknown commands or malformed input were rendered literally
};

// Typesets `tex` (the content between the dollar signs) in display (`$$`) or
// text (`$`) style. Unknown commands do not fail (they render literally);
// only the limits do (source too large, nesting too deep, too many atoms).
std::expected<MathScene, std::string> typesetMath(std::string_view tex, bool display, const IMathMeasurer& measurer,
                                                  const MathOptions& options = {});

} // namespace rivet::markdown::math
