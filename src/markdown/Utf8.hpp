// SPDX-License-Identifier: MPL-2.0
#pragma once

// Small UTF-8 helpers shared by the Markdown parser, slugger and layout.
// Decoding is total: malformed input never throws or reads out of bounds.

#include <cstddef>
#include <string>
#include <string_view>

namespace rivet::markdown::utf8 {

inline constexpr char32_t kReplacement = 0xFFFD;

struct Decoded {
    char32_t codePoint = kReplacement;
    std::size_t length = 1; // bytes consumed (>= 1 when input is non-empty)
    bool valid = true;
};

// Decodes one code point at s[i] (i < s.size()). Overlong forms, surrogates
// and values above U+10FFFF are rejected as invalid (consumes 1 byte, U+FFFD).
inline Decoded decode(std::string_view s, std::size_t i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80) return {b0, 1, true};
    std::size_t need = 0;
    char32_t cp = 0;
    char32_t minimum = 0;
    if ((b0 & 0xE0) == 0xC0) { need = 1; cp = b0 & 0x1Fu; minimum = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { need = 2; cp = b0 & 0x0Fu; minimum = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { need = 3; cp = b0 & 0x07u; minimum = 0x10000; }
    else return {kReplacement, 1, false};
    if (i + need >= s.size()) return {kReplacement, 1, false};
    for (std::size_t k = 1; k <= need; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) return {kReplacement, 1, false};
        cp = (cp << 6) | (b & 0x3Fu);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return {kReplacement, 1, false};
    return {cp, need + 1, true};
}

inline void append(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = kReplacement;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Snaps `offset` down to the start of the code point containing it.
inline std::size_t snapToBoundary(std::string_view s, std::size_t offset) {
    if (offset >= s.size()) return s.size();
    std::size_t i = offset;
    std::size_t steps = 0;
    while (i > 0 && steps < 3 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
        --i;
        ++steps;
    }
    return i;
}

// Returns `s` with every invalid sequence replaced by U+FFFD. `changed`, when
// non-null, is set if anything was replaced.
inline std::string sanitize(std::string_view s, bool* changed = nullptr) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const Decoded d = decode(s, i);
        if (d.valid) {
            out.append(s.substr(i, d.length));
        } else {
            append(out, kReplacement);
            if (changed) *changed = true;
        }
        i += d.length;
    }
    return out;
}

} // namespace rivet::markdown::utf8
