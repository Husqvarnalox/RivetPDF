// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace rivet::ui::utf8 {

// Small UTF-8 helpers shared by the text widgets. Indices are byte offsets;
// "boundary" means a code-point start (or text.size()). All helpers tolerate
// malformed input (stray continuation bytes are treated as part of the
// preceding sequence) and never read out of range.

inline bool isContinuationByte(char byte) {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

// Largest boundary j < byteIndex (0 when byteIndex is 0 or the text is empty).
inline std::size_t previousBoundary(std::string_view s, std::size_t byteIndex) {
    if (s.empty()) return 0;
    std::size_t index = std::min(byteIndex, s.size());
    if (index == 0) return 0;
    --index;
    while (index > 0 && isContinuationByte(s[index])) --index;
    return index;
}

// Smallest boundary j > byteIndex (s.size() at the end).
inline std::size_t nextBoundary(std::string_view s, std::size_t byteIndex) {
    const std::size_t size = s.size();
    if (byteIndex >= size) return size;
    std::size_t next = byteIndex + 1;
    while (next < size && isContinuationByte(s[next])) ++next;
    return next;
}

// Largest boundary <= byteIndex: the safe cut point for truncating `s` to at
// most `byteIndex` bytes without splitting a code point.
inline std::size_t floorBoundary(std::string_view s, std::size_t byteIndex) {
    if (byteIndex >= s.size()) return s.size();
    while (byteIndex > 0 && isContinuationByte(s[byteIndex])) --byteIndex;
    return byteIndex;
}

// Decodes the code point starting at boundary `byteIndex`. Malformed or
// truncated sequences decode to U+FFFD.
inline char32_t decodeAt(std::string_view s, std::size_t byteIndex) {
    if (byteIndex >= s.size()) return 0;
    const auto lead = static_cast<unsigned char>(s[byteIndex]);
    if (lead < 0x80u) return lead;
    const std::size_t end = nextBoundary(s, byteIndex);
    const std::size_t length = end - byteIndex;
    std::size_t expected = 0;
    char32_t codePoint = 0;
    if ((lead & 0xE0u) == 0xC0u) {
        expected = 2;
        codePoint = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
        expected = 3;
        codePoint = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
        expected = 4;
        codePoint = lead & 0x07u;
    } else {
        return 0xFFFDu;
    }
    if (length != expected) return 0xFFFDu;
    for (std::size_t k = 1; k < length; ++k) {
        codePoint = (codePoint << 6) | (static_cast<unsigned char>(s[byteIndex + k]) & 0x3Fu);
    }
    return codePoint;
}

inline void appendCodePoint(std::string& out, char32_t codePoint) {
    if (codePoint <= 0x7Fu) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

} // namespace rivet::ui::utf8
