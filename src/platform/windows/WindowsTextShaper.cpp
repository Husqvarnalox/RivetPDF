// SPDX-License-Identifier: MPL-2.0
#include "WindowsTextShaper.hpp"

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace rivet::platform::text {

namespace {

using FontKey = std::tuple<double, int, bool, bool>; // size, weight, italic, monospace

FontKey keyOf(const ui::Font& font) {
    return {font.size, static_cast<int>(font.weight), font.italic, font.monospace};
}

struct FontEntry {
    win::ComPtr<IDWriteTextFormat> format;
    double ascentPerEm = 0.0;
    double descentPerEm = 0.0;
};

struct State {
    std::mutex mutex;
    win::ComPtr<IDWriteFactory> factory;
    std::map<FontKey, FontEntry> fonts;
    std::unordered_map<std::string, std::shared_ptr<const LineLayout>> layouts;
};

State& state() {
    static State instance;
    return instance;
}

DWRITE_FONT_WEIGHT weightOf(const ui::Font& font) {
    switch (font.weight) {
    case ui::Font::Weight::Regular: return DWRITE_FONT_WEIGHT_REGULAR;
    case ui::Font::Weight::Semibold: return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    case ui::Font::Weight::Bold: return DWRITE_FONT_WEIGHT_BOLD;
    }
    return DWRITE_FONT_WEIGHT_REGULAR;
}

// Typographic ascent/descent (per em) of the first installed family of
// `families` at the requested weight/style.
bool primaryMetrics(IDWriteFactory* factory, const wchar_t* const* families, DWRITE_FONT_WEIGHT weight,
                    DWRITE_FONT_STYLE style, const wchar_t*& chosen, double& ascent, double& descent) {
    win::ComPtr<IDWriteFontCollection> collection;
    if (FAILED(factory->GetSystemFontCollection(collection.put(), FALSE)) || !collection) return false;
    for (const wchar_t* const* family = families; *family != nullptr; ++family) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(collection->FindFamilyName(*family, &index, &exists)) || !exists) continue;
        win::ComPtr<IDWriteFontFamily> fontFamily;
        win::ComPtr<IDWriteFont> font;
        if (FAILED(collection->GetFontFamily(index, fontFamily.put())) ||
            FAILED(fontFamily->GetFirstMatchingFont(weight, DWRITE_FONT_STRETCH_NORMAL, style, font.put()))) {
            continue;
        }
        DWRITE_FONT_METRICS metrics{};
        font->GetMetrics(&metrics);
        if (metrics.designUnitsPerEm == 0) continue;
        chosen = *family;
        ascent = static_cast<double>(metrics.ascent) / metrics.designUnitsPerEm;
        descent = static_cast<double>(metrics.descent) / metrics.designUnitsPerEm;
        return true;
    }
    return false;
}

// Caller holds the mutex.
const FontEntry* fontFor(State& s, const ui::Font& font) {
    const FontKey key = keyOf(font);
    if (const auto it = s.fonts.find(key); it != s.fonts.end()) return &it->second;

    static const wchar_t* const kSans[] = {L"Segoe UI", L"Tahoma", L"Arial", nullptr};
    static const wchar_t* const kMono[] = {L"Consolas", L"Cascadia Mono", L"Courier New", nullptr};
    const DWRITE_FONT_WEIGHT weight = weightOf(font);
    const DWRITE_FONT_STYLE style = font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;

    FontEntry entry;
    const wchar_t* family = font.monospace ? kMono[0] : kSans[0];
    double ascent = 0.8;
    double descent = 0.2;
    primaryMetrics(s.factory.get(), font.monospace ? kMono : kSans, weight, style, family, ascent, descent);
    entry.ascentPerEm = ascent;
    entry.descentPerEm = descent;
    const auto size = static_cast<FLOAT>(font.size > 0.0 ? font.size : 1.0);
    if (FAILED(s.factory->CreateTextFormat(family, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL, size,
                                           L"en-us", entry.format.put()))) {
        entry.format.reset();
    } else {
        entry.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return &s.fonts.emplace(key, std::move(entry)).first->second;
}

std::string cacheKey(std::string_view text, const ui::Font& font) {
    std::string key;
    key.reserve(text.size() + 24);
    key.append(reinterpret_cast<const char*>(&font.size), sizeof font.size);
    key += static_cast<char>(font.weight);
    key += font.italic ? 'i' : 'n';
    key += font.monospace ? 'm' : 's';
    key.append(text);
    return key;
}

} // namespace

bool initialize() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (s.factory) return true;
    return SUCCEEDED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                           reinterpret_cast<IUnknown**>(s.factory.put())));
}

void shutdown() {
    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.layouts.clear();
    s.fonts.clear();
    s.factory.reset();
}

std::shared_ptr<const LineLayout> layoutLine(std::string_view text, const ui::Font& font) {
    static const auto kEmpty = std::make_shared<const LineLayout>();
    if (text.empty()) return kEmpty;

    State& s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.factory) return kEmpty;

    std::string key = cacheKey(text, font);
    if (const auto it = s.layouts.find(key); it != s.layouts.end()) return it->second;

    const FontEntry* entry = fontFor(s, font);
    auto line = std::make_shared<LineLayout>();
    line->ascent = entry->ascentPerEm * font.size;
    line->descent = entry->descentPerEm * font.size;
    const std::wstring wide = win::widen(text);
    if (entry->format && !wide.empty() &&
        SUCCEEDED(s.factory->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()), entry->format.get(),
                                              1.0e6F, 1.0e6F, line->layout.put()))) {
        DWRITE_TEXT_METRICS metrics{};
        if (SUCCEEDED(line->layout->GetMetrics(&metrics))) line->width = static_cast<double>(metrics.widthIncludingTrailingWhitespace);
        // Text with line breaks lays out as several lines: size the buffer.
        UINT32 lineCount = 0;
        line->layout->GetLineMetrics(nullptr, 0, &lineCount);
        std::vector<DWRITE_LINE_METRICS> lineMetrics(lineCount > 0 ? lineCount : 1);
        if (SUCCEEDED(line->layout->GetLineMetrics(lineMetrics.data(), static_cast<UINT32>(lineMetrics.size()),
                                                   &lineCount)) &&
            lineCount >= 1) {
            line->layoutBaseline = static_cast<double>(lineMetrics[0].baseline);
        } else {
            line->layoutBaseline = line->ascent;
        }
    } else {
        line->layout.reset();
    }

    // Bounded cache: the shell draws a modest, mostly stable set of strings.
    if (s.layouts.size() >= 2048) s.layouts.clear();
    std::shared_ptr<const LineLayout> result = std::move(line);
    s.layouts.emplace(std::move(key), result);
    return result;
}

} // namespace rivet::platform::text
