// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownPaintMeasurer.hpp"

#include <cmath>

namespace rivet::app {

namespace {

// Decodes one code point at `i` (lenient: invalid bytes decode as themselves).
char32_t decodeAt(std::string_view s, std::size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    std::size_t len = 1;
    char32_t cp = b0;
    if (b0 >= 0xF0) {
        len = 4;
        cp = b0 & 0x07u;
    } else if (b0 >= 0xE0) {
        len = 3;
        cp = b0 & 0x0Fu;
    } else if (b0 >= 0xC0) {
        len = 2;
        cp = b0 & 0x1Fu;
    }
    if (i + len > s.size()) {
        ++i;
        return b0;
    }
    for (std::size_t k = 1; k < len; ++k) {
        const auto b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0u) != 0x80u) {
            ++i;
            return b0;
        }
        cp = (cp << 6) | (b & 0x3Fu);
    }
    i += len;
    return cp;
}

} // namespace

ui::Font MarkdownPaintMeasurer::fontFor(const markdown::TextStyle& style) {
    using markdown::TextKind;
    ui::Font font;
    font.size = style.size;
    const bool heading = style.kind >= TextKind::H1 && style.kind <= TextKind::H6;
    font.weight = (style.bold || heading) ? ui::Font::Weight::Bold : ui::Font::Weight::Regular;
    font.italic = style.italic;
    font.monospace = style.monospace;
    return font;
}

std::uint64_t MarkdownPaintMeasurer::styleKey(const markdown::TextStyle& style) {
    const auto size = static_cast<std::uint64_t>(std::llround(style.size * 64.0)) & 0xFFFFFFFFu;
    const ui::Font f = fontFor(style);
    return size | (static_cast<std::uint64_t>(f.weight == ui::Font::Weight::Bold) << 32) |
           (static_cast<std::uint64_t>(f.italic) << 33) | (static_cast<std::uint64_t>(f.monospace) << 34);
}

MarkdownPaintMeasurer::StyleData& MarkdownPaintMeasurer::dataFor(const markdown::TextStyle& style) const {
    StyleData& data = styles_[styleKey(style)];
    if (data.lineHeight <= 0.0) data.lineHeight = style.size * 1.2;
    return data;
}

markdown::TextMetrics MarkdownPaintMeasurer::measure(std::string_view utf8,
                                                     const markdown::TextStyle& style) const {
    StyleData& data = dataFor(style);
    markdown::TextMetrics m;
    if (context_ != nullptr) {
        const core::Size size = context_->measureText(utf8.empty() ? std::string_view{"M"} : utf8, fontFor(style));
        if (size.height > 0.0) data.lineHeight = size.height;
        m.width = utf8.empty() ? 0.0 : size.width;
    } else {
        for (std::size_t i = 0; i < utf8.size();) {
            const char32_t cp = decodeAt(utf8, i);
            const auto it = data.advances.find(cp);
            m.width += it != data.advances.end() ? it->second : style.size * 0.55;
        }
    }
    m.ascent = data.lineHeight * 0.8;
    m.descent = data.lineHeight * 0.2;
    return m;
}

void MarkdownPaintMeasurer::warm(std::string_view utf8, const markdown::TextStyle& style) const {
    if (context_ == nullptr) return;
    StyleData& data = dataFor(style);
    const ui::Font font = fontFor(style);
    char buf[5];
    for (std::size_t i = 0; i < utf8.size();) {
        const std::size_t start = i;
        const char32_t cp = decodeAt(utf8, i);
        if (data.advances.contains(cp)) continue;
        const std::size_t len = i - start;
        for (std::size_t k = 0; k < len; ++k) buf[k] = utf8[start + k];
        data.advances.emplace(cp, context_->measureText(std::string_view{buf, len}, font).width);
    }
}

void MarkdownPaintMeasurer::clear() { styles_.clear(); }

} // namespace rivet::app
