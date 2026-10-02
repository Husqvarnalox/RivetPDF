// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Deterministic, backend-neutral line breaking for text blocks Rivet writes
// (ADR-0015 "Layout"). The backend supplies the advances of its actual font,
// so the on-screen result and the saved file use the same lines.

namespace rivet::pdf {

struct PdfTextLine {
    std::u32string text; // the line's code points (no trailing break spaces)
    double width = 0.0;  // sum of advances of `text`, points
};

struct PdfTextLayoutResult {
    std::vector<PdfTextLine> lines; // at least one (possibly empty) line
    bool hadInvalidUtf8 = false;    // ill-formed sequences became U+FFFD
};

// Advance of one code point at the block's font size, in points (>= 0).
using PdfAdvanceFunction = std::function<double(char32_t)>;

// Rules (all deterministic, no locale, no shaping):
//  - UTF-8 is decoded strictly; each ill-formed sequence becomes U+FFFD.
//  - "\r\n", "\r" and "\n" are hard line breaks; each produces a line, so
//    "a\n" yields ["a", ""] and "" yields [""].
//  - U+0009 (tab) is treated as U+0020. Other C0/C1 controls are dropped.
//  - wrapWidth <= 0: no wrapping (one line per hard-broken paragraph).
//  - Otherwise greedy wrapping: break opportunities are U+0020 runs; the
//    spaces at a break are dropped from both lines; leading spaces of a
//    paragraph are kept. A word wider than wrapWidth on its own line is
//    broken between code points, never before a combining mark
//    (U+0300-036F, U+1AB0-1AFF, U+1DC0-1DFF, U+20D0-20FF, U+FE20-FE2F) and
//    always at least one code point (plus its marks) per line.
//  - No hyphenation, kerning, bidi, justification or complex-script shaping
//    (documented limits, ADR-0015).
PdfTextLayoutResult layoutTextBlock(std::string_view utf8, double wrapWidth,
                                    const PdfAdvanceFunction& advance);

// Strict UTF-8 <-> UTF-32 helpers used by the layout and the backends.
// decodeUtf8 replaces each ill-formed sequence with U+FFFD and reports it.
std::u32string decodeUtf8(std::string_view utf8, bool* hadInvalid = nullptr);
std::string encodeUtf8(std::u32string_view text);

} // namespace rivet::pdf
