// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfTextLayout.hpp"

#include <cmath>
#include <cstdint>
#include <utility>

namespace rivet::pdf {

namespace {

constexpr char32_t kReplacement = 0xFFFD;

bool isCombiningMark(char32_t c) {
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) ||
           (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F);
}

bool isDroppedControl(char32_t c) {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F);
}

// One unit of a word that is never split: a code point and the combining
// marks that follow it.
struct Cluster {
    std::size_t begin = 0;
    std::size_t end = 0; // exclusive
    double width = 0.0;
};

// One paragraph (a hard-break-free run) laid out into `out`.
void layoutParagraph(const std::u32string& text, double wrapWidth, const PdfAdvanceFunction& advance,
                     std::vector<PdfTextLine>& out) {
    const std::size_t n = text.size();
    std::vector<double> adv(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double a = advance ? advance(text[i]) : 0.0;
        adv[i] = (std::isfinite(a) && a > 0.0) ? a : 0.0;
    }
    auto lineOf = [&](std::size_t from, std::size_t to) {
        PdfTextLine line;
        line.text = text.substr(from, to - from);
        for (std::size_t i = from; i < to; ++i) line.width += adv[i];
        return line;
    };
    auto sum = [&](std::size_t from, std::size_t to) {
        double w = 0.0;
        for (std::size_t i = from; i < to; ++i) w += adv[i];
        return w;
    };

    if (wrapWidth <= 0.0 || !std::isfinite(wrapWidth)) {
        out.push_back(lineOf(0, n));
        return;
    }

    // The current line is the half-open range [lineBegin, lineEnd) of `text`
    // (spaces between its words are part of the range; spaces at a break are
    // not, so the range never spans a dropped run).
    std::size_t lineBegin = 0;
    std::size_t lineEnd = 0;
    double lineWidth = 0.0;
    bool lineHasWord = false; // beyond leading spaces

    auto flush = [&]() {
        out.push_back(lineOf(lineBegin, lineEnd));
        lineWidth = 0.0;
        lineHasWord = false;
    };

    // Leading spaces of the paragraph are kept.
    std::size_t pos = 0;
    while (pos < n && text[pos] == U' ') ++pos;
    lineBegin = 0;
    lineEnd = pos;
    lineWidth = sum(0, pos);

    // Places the cluster-wise break of a word that does not fit on a line of
    // its own; the line `[lineBegin, lineEnd)` must be empty of words.
    auto placeLongWord = [&](std::size_t wordBegin, std::size_t wordEnd) {
        std::vector<Cluster> clusters;
        for (std::size_t i = wordBegin; i < wordEnd;) {
            Cluster cluster;
            cluster.begin = i;
            cluster.width = adv[i];
            ++i;
            while (i < wordEnd && isCombiningMark(text[i])) {
                cluster.width += adv[i];
                ++i;
            }
            cluster.end = i;
            clusters.push_back(cluster);
        }
        // `text` positions are contiguous, so the line grows by extending
        // lineEnd; a fresh line starts at the cluster's begin.
        bool first = true;
        for (const Cluster& cluster : clusters) {
            if (first) {
                // At least one cluster per line: the first is forced onto
                // the current line (which holds at most leading spaces).
                if (!lineHasWord) {
                    lineEnd = cluster.end;
                    lineWidth += cluster.width;
                    lineHasWord = true;
                }
                first = false;
                continue;
            }
            if (lineWidth + cluster.width <= wrapWidth) {
                lineEnd = cluster.end;
                lineWidth += cluster.width;
            } else {
                flush();
                lineBegin = cluster.begin;
                lineEnd = cluster.end;
                lineWidth = cluster.width;
                lineHasWord = true;
            }
        }
    };

    while (pos < n) {
        // pos is at the start of a word; read it, then the space run after it.
        const std::size_t wordBegin = pos;
        while (pos < n && text[pos] != U' ') ++pos;
        const std::size_t wordEnd = pos;
        const std::size_t spaceBegin = pos;
        while (pos < n && text[pos] == U' ') ++pos;
        const std::size_t spaceEnd = pos;
        const double wordWidth = sum(wordBegin, wordEnd);

        if (!lineHasWord) {
            // Leading spaces (if any) then this word.
            if (lineWidth + wordWidth <= wrapWidth) {
                lineEnd = wordEnd;
                lineWidth += wordWidth;
                lineHasWord = true;
            } else if (lineEnd > lineBegin) {
                // Leading spaces plus a word that does not fit: the word
                // goes on its own line when it fits there, else is broken
                // right after the leading spaces. Keep the spaces (kept by
                // rule) and let the cluster forcing below start the line.
                if (wordWidth <= wrapWidth) {
                    // Spaces alone would form an empty-looking line; start
                    // the word on the same line only if forced. Prefer the
                    // spaces-only line to honour "leading spaces are kept".
                    flush();
                    lineBegin = wordBegin;
                    lineEnd = wordEnd;
                    lineWidth = wordWidth;
                    lineHasWord = true;
                } else {
                    placeLongWord(wordBegin, wordEnd);
                }
            } else if (wordWidth <= wrapWidth) {
                lineBegin = wordBegin;
                lineEnd = wordEnd;
                lineWidth = wordWidth;
                lineHasWord = true;
            } else {
                lineBegin = wordBegin;
                lineEnd = wordBegin;
                lineWidth = 0.0;
                placeLongWord(wordBegin, wordEnd);
            }
        } else {
            const double gapWidth = sum(lineEnd, wordBegin);
            if (lineWidth + gapWidth + wordWidth <= wrapWidth) {
                lineEnd = wordEnd;
                lineWidth += gapWidth + wordWidth;
            } else {
                // Break: the gap spaces are dropped from both lines.
                flush();
                lineBegin = wordBegin;
                lineEnd = wordBegin;
                lineWidth = 0.0;
                lineHasWord = false;
                if (wordWidth <= wrapWidth) {
                    lineEnd = wordEnd;
                    lineWidth = wordWidth;
                    lineHasWord = true;
                } else {
                    placeLongWord(wordBegin, wordEnd);
                }
            }
        }

        if (pos >= n) {
            // Trailing spaces of the paragraph are not at a break: kept.
            if (spaceEnd > spaceBegin) {
                lineEnd = spaceEnd;
                lineWidth += sum(spaceBegin, spaceEnd);
            }
        }
    }
    flush();
}

} // namespace

std::u32string decodeUtf8(std::string_view utf8, bool* hadInvalid) {
    // WHATWG-style decoder: each maximal ill-formed subsequence yields one
    // U+FFFD; surrogates, overlongs and values above U+10FFFF are rejected by
    // the per-lead-byte continuation ranges.
    std::u32string out;
    out.reserve(utf8.size());
    bool invalid = false;
    std::size_t needed = 0;
    std::uint32_t cp = 0;
    std::uint8_t lower = 0x80;
    std::uint8_t upper = 0xBF;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto byte = static_cast<std::uint8_t>(utf8[i]);
        if (needed == 0) {
            ++i;
            if (byte < 0x80) {
                out.push_back(byte);
            } else if (byte >= 0xC2 && byte <= 0xDF) {
                needed = 1;
                cp = byte & 0x1FU;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                if (byte == 0xE0) lower = 0xA0;
                if (byte == 0xED) upper = 0x9F;
                needed = 2;
                cp = byte & 0x0FU;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                if (byte == 0xF0) lower = 0x90;
                if (byte == 0xF4) upper = 0x8F;
                needed = 3;
                cp = byte & 0x07U;
            } else {
                out.push_back(kReplacement);
                invalid = true;
            }
            continue;
        }
        if (byte < lower || byte > upper) {
            // Ill-formed: emit one U+FFFD and reprocess this byte afresh.
            out.push_back(kReplacement);
            invalid = true;
            needed = 0;
            cp = 0;
            lower = 0x80;
            upper = 0xBF;
            continue;
        }
        lower = 0x80;
        upper = 0xBF;
        cp = (cp << 6) | (byte & 0x3FU);
        ++i;
        if (--needed == 0) {
            out.push_back(static_cast<char32_t>(cp));
            cp = 0;
        }
    }
    if (needed != 0) {
        out.push_back(kReplacement); // truncated sequence at the end
        invalid = true;
    }
    if (hadInvalid != nullptr) *hadInvalid = invalid;
    return out;
}

std::string encodeUtf8(std::u32string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char32_t c : text) {
        if (c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) c = kReplacement;
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

PdfTextLayoutResult layoutTextBlock(std::string_view utf8, double wrapWidth, const PdfAdvanceFunction& advance) {
    PdfTextLayoutResult result;
    const std::u32string decoded = decodeUtf8(utf8, &result.hadInvalidUtf8);

    std::u32string paragraph;
    auto endParagraph = [&]() {
        layoutParagraph(paragraph, wrapWidth, advance, result.lines);
        paragraph.clear();
    };
    for (std::size_t i = 0; i < decoded.size(); ++i) {
        const char32_t c = decoded[i];
        if (c == U'\r') {
            if (i + 1 < decoded.size() && decoded[i + 1] == U'\n') ++i;
            endParagraph();
        } else if (c == U'\n') {
            endParagraph();
        } else if (c == U'\t') {
            paragraph.push_back(U' ');
        } else if (isDroppedControl(c)) {
            continue;
        } else {
            paragraph.push_back(c);
        }
    }
    endParagraph();
    return result;
}

} // namespace rivet::pdf
