// SPDX-License-Identifier: MPL-2.0
#include "markdown/MarkdownSlug.hpp"

#include "markdown/Utf8.hpp"

namespace rivet::markdown {

namespace {

char32_t toLower(char32_t c) {
    if (c < 0x80) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;               // Latin-1
    if (c >= 0x100 && c <= 0x17F) {                                        // Latin Extended-A (paired)
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c % 2 == 1) ? c + 1 : c;
        if (c == 0x130 || c == 0x138 || c == 0x149 || c == 0x178 || c == 0x17F) return c;
        return (c % 2 == 0) ? c + 1 : c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;             // Greek
    if (c >= 0x410 && c <= 0x42F) return c + 32;                           // Cyrillic А-Я
    if (c >= 0x400 && c <= 0x40F) return c + 80;                           // Cyrillic Ѐ-Џ
    if (c >= 0x460 && c <= 0x481 && c % 2 == 0) return c + 1;
    if (c >= 0x48A && c <= 0x4BF && c % 2 == 0) return c + 1;
    return c;
}

bool isWordCodePoint(char32_t c) {
    if (c < 0x80) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '_' || c == '-';
    if (c < 0xC0) return c == 0xAA || c == 0xB5 || c == 0xBA; // Latin-1 punctuation/symbols
    if (c == 0xD7 || c == 0xF7) return false;
    if (c >= 0x2000 && c <= 0x2BFF) return false;   // punctuation, symbols, arrows, math, dingbats
    if (c >= 0x2E00 && c <= 0x2E7F) return false;
    if (c >= 0x3000 && c <= 0x303F) return false;   // CJK punctuation
    if (c >= 0xFE00 && c <= 0xFE0F) return false;   // variation selectors
    if (c >= 0xFF00 && c <= 0xFF0F) return false;   // fullwidth punctuation
    if (c >= 0x1F000 && c <= 0x1FAFF) return false; // emoji
    if (c == 0xFEFF || c == 0xFFFD) return false;
    return true;
}

} // namespace

std::string makeSlug(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const utf8::Decoded d = utf8::decode(text, i);
        i += d.length;
        if (!d.valid) continue;
        const char32_t c = toLower(d.codePoint);
        if (c == ' ' || c == '\t' || c == 0xA0) {
            out.push_back('-');
        } else if (isWordCodePoint(c)) {
            utf8::append(out, c);
        }
    }
    if (out.empty()) out = "section";
    return out;
}

std::string SlugAllocator::allocate(std::string_view headingText) {
    std::string base = makeSlug(headingText);
    std::string candidate = base;
    for (unsigned n = 1; !used_.insert(candidate).second; ++n) {
        candidate = base + "-" + std::to_string(n);
    }
    return candidate;
}

} // namespace rivet::markdown
