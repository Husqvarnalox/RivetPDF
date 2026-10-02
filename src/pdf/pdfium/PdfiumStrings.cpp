// SPDX-License-Identifier: MPL-2.0

#include "PdfiumStrings.h"

namespace rivet::pdf::internal {

// Converts UTF-16LE bytes (as returned by FPDF_GetMetaText) to UTF-8.
// Stops at the terminating NUL code unit, skips a leading BOM, decodes
// surrogate pairs and emits U+FFFD for malformed sequences. No <codecvt>.
std::string utf16leToUtf8(const std::uint8_t* bytes, std::size_t byteLength) {
    std::string out;
    out.reserve(byteLength); // usually an over-estimate; avoids reallocation

    const std::size_t units = byteLength / 2;
    auto unitAt = [&](std::size_t index) -> std::uint16_t {
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[index * 2]) |
                                          (static_cast<std::uint16_t>(bytes[index * 2 + 1]) << 8));
    };

    auto appendCodePoint = [&out](std::uint32_t codePoint) {
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
    };

    std::size_t i = 0;
    if (units > 0 && unitAt(0) == 0xFEFFu) {
        i = 1; // byte-order mark
    }

    while (i < units) {
        const std::uint16_t unit = unitAt(i);
        if (unit == 0) {
            break; // terminator
        }
        if (unit >= 0xD800u && unit <= 0xDBFFu) { // high surrogate
            if (i + 1 < units && unitAt(i + 1) >= 0xDC00u && unitAt(i + 1) <= 0xDFFFu) {
                appendCodePoint(0x10000u + ((static_cast<std::uint32_t>(unit) - 0xD800u) << 10) +
                                (static_cast<std::uint32_t>(unitAt(i + 1)) - 0xDC00u));
                i += 2;
            } else {
                appendCodePoint(0xFFFDu); // unpaired high surrogate
                i += 1;
            }
            continue;
        }
        if (unit >= 0xDC00u && unit <= 0xDFFFu) { // unpaired low surrogate
            appendCodePoint(0xFFFDu);
            i += 1;
            continue;
        }
        appendCodePoint(unit);
        i += 1;
    }
    return out;
}

std::vector<std::uint8_t> utf8ToUtf16le(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 2 + 2);
    auto put = [&out](std::uint32_t unit) {
        out.push_back(static_cast<std::uint8_t>(unit & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((unit >> 8) & 0xFFu));
    };
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = 0xFFFDu;
        std::size_t extra = 0;
        if (b0 < 0x80u) {
            cp = b0;
        } else if (b0 >= 0xC2u && b0 <= 0xDFu) {
            cp = b0 & 0x1Fu;
            extra = 1;
        } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
            cp = b0 & 0x0Fu;
            extra = 2;
        } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
            cp = b0 & 0x07u;
            extra = 3;
        }
        bool ok = extra == 0 ? b0 < 0x80u : i + extra < n;
        if (extra != 0 && ok) {
            for (std::size_t k = 1; k <= extra; ++k) {
                const auto bk = static_cast<unsigned char>(text[i + k]);
                if ((bk & 0xC0u) != 0x80u) {
                    ok = false;
                    break;
                }
                cp = (cp << 6) | (bk & 0x3Fu);
            }
        }
        if (!ok || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
            put(0xFFFDu);
            i += 1;
            continue;
        }
        i += extra + 1;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            put(0xD800u + (cp >> 10));
            put(0xDC00u + (cp & 0x3FFu));
        } else {
            put(cp);
        }
    }
    put(0); // terminator
    return out;
}

} // namespace rivet::pdf::internal
