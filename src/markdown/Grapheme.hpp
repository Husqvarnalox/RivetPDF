// SPDX-License-Identifier: MPL-2.0
#pragma once

// Approximate extended-grapheme-cluster boundaries for caret movement and
// deletion (one "character" as the user sees it). Covers what text editing
// meets in practice: combining marks, variation selectors, emoji modifiers
// (skin tones), ZWJ sequences, tag sequences and regional-indicator flag
// pairs. Not a full UAX #29 implementation (no Hangul jamo sequences or
// Indic conjunct rules): those still break between code points, which is
// safe, merely finer than ideal. Pure; tolerant of invalid UTF-8.

#include "markdown/Utf8.hpp"

#include <cstddef>
#include <string_view>

namespace rivet::markdown::grapheme {

// True for code points that never start a cluster (they extend the previous one).
constexpr bool isExtend(char32_t cp) {
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) || (cp >= 0x0591 && cp <= 0x05BD) ||
           cp == 0x05BF || cp == 0x05C1 || cp == 0x05C2 || cp == 0x05C4 || cp == 0x05C5 || cp == 0x05C7 ||
           (cp >= 0x0610 && cp <= 0x061A) || (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
           (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) || cp == 0x06E7 || cp == 0x06E8 ||
           (cp >= 0x06EA && cp <= 0x06ED) || (cp >= 0x0900 && cp <= 0x0903) || (cp >= 0x093A && cp <= 0x094F) ||
           (cp >= 0x0951 && cp <= 0x0957) || cp == 0x0962 || cp == 0x0963 || (cp >= 0x0981 && cp <= 0x0983) ||
           (cp >= 0x09BC && cp <= 0x09D7) || (cp >= 0x0E31 && cp <= 0x0E3A && cp != 0x0E32 && cp != 0x0E33) ||
           (cp >= 0x0E47 && cp <= 0x0E4E) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           cp == 0x200C || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0x302A && cp <= 0x302F) || cp == 0x3099 ||
           cp == 0x309A || (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) ||
           (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F) || (cp >= 0xE0100 && cp <= 0xE01EF);
}

constexpr bool isRegionalIndicator(char32_t cp) { return cp >= 0x1F1E6 && cp <= 0x1F1FF; }
inline constexpr char32_t kZwj = 0x200D;

// Smallest cluster boundary > i (s.size() at the end). `i` must be a boundary.
inline std::size_t next(std::string_view s, std::size_t i) {
    if (i >= s.size()) return s.size();
    utf8::Decoded d = utf8::decode(s, i);
    const char32_t first = d.codePoint;
    std::size_t j = i + d.length;
    bool afterZwj = first == kZwj;
    bool pairedRi = false;
    while (j < s.size()) {
        d = utf8::decode(s, j);
        const char32_t cp = d.codePoint;
        if (isExtend(cp) || cp == kZwj) {
            afterZwj = cp == kZwj;
            j += d.length;
            continue;
        }
        if (afterZwj && d.valid && cp >= 0x80) { // ZWJ sequence: the next pictograph joins
            afterZwj = false;
            j += d.length;
            continue;
        }
        if (isRegionalIndicator(first) && isRegionalIndicator(cp) && !pairedRi && j == i + 4) {
            pairedRi = true;
            j += d.length;
            continue;
        }
        break;
    }
    return j;
}

// Largest cluster boundary < i (0 at the start).
inline std::size_t previous(std::string_view s, std::size_t i) {
    if (i == 0 || s.empty()) return 0;
    if (i > s.size()) i = s.size();
    // Back up to a code point that certainly starts a cluster, then walk forward.
    std::size_t start = utf8::snapToBoundary(s, i - 1);
    for (int guard = 0; start > 0 && guard < 64; ++guard) {
        const char32_t cp = utf8::decode(s, start).codePoint;
        const std::size_t before = utf8::snapToBoundary(s, start - 1);
        const char32_t prev = utf8::decode(s, before).codePoint;
        if (!isExtend(cp) && cp != kZwj && prev != kZwj && !isRegionalIndicator(cp)) break;
        start = before;
    }
    std::size_t last = start;
    for (std::size_t b = start; b < i;) {
        last = b;
        b = next(s, b);
    }
    return last;
}

// Whether `i` is a cluster boundary of `s` (0 and s.size() always are).
inline bool isBoundary(std::string_view s, std::size_t i) {
    if (i == 0 || i >= s.size()) return true;
    if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) return false;
    return next(s, previous(s, i)) == i;
}

} // namespace rivet::markdown::grapheme
