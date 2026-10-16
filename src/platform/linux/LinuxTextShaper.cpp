// SPDX-License-Identifier: MPL-2.0
#include "LinuxTextShaper.hpp"

#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <hb-ot.h>
#include <hb.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace rivet::platform::text {

namespace {

// One loaded font file/instance: a HarfBuzz font for shaping (font units,
// scaled per call) and a cairo face for rendering. Never destroyed.
struct Face {
    hb_font_t* hbFont = nullptr;
    cairo_font_face_t* cairoFace = nullptr;
    FcCharSet* charset = nullptr; // borrowed from `pattern`
    FcPattern* pattern = nullptr; // render-prepared match, owned
    double upem = 1000.0;
    double ascender = 0.0;  // font units, positive up
    double descender = 0.0; // font units, positive down
};

using StyleKey = std::tuple<int, bool, bool>; // fontconfig weight, italic, monospace

struct Style {
    Face* primary = nullptr;
    std::unordered_map<char32_t, Face*> fallback; // code point -> face (primary when none)
};

int fcWeight(ui::Font::Weight weight) {
    switch (weight) {
    case ui::Font::Weight::Regular: return FC_WEIGHT_REGULAR;
    case ui::Font::Weight::Semibold: return FC_WEIGHT_DEMIBOLD;
    case ui::Font::Weight::Bold: return FC_WEIGHT_BOLD;
    }
    return FC_WEIGHT_REGULAR;
}

class FontRegistry {
public:
    static FontRegistry& instance() {
        // Process-lifetime (like the CTFont cache on macOS): intentionally
        // never destroyed, so no static-destruction-order hazards with
        // worker threads that may still measure text during shutdown.
        static FontRegistry* registry = new FontRegistry();
        return *registry;
    }

    std::shared_ptr<const ShapedLine> shape(std::string_view text, const ui::Font& font) {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::string key;
        key.reserve(text.size() + 24);
        key += std::to_string(font.size);
        key += static_cast<char>('0' + static_cast<int>(font.weight));
        key += font.italic ? 'i' : 'r';
        key += font.monospace ? 'm' : 's';
        key += '\x1f';
        key.append(text.data(), text.size());
        if (const auto it = cache_.find(key); it != cache_.end()) return it->second;

        auto line = std::make_shared<ShapedLine>(shapeUncached(text, font));
        if (cache_.size() >= kCacheLimit) cache_.clear();
        cache_.emplace(std::move(key), line);
        return line;
    }

private:
    static constexpr std::size_t kCacheLimit = 8192;

    FontRegistry() { FcInit(); }

    static std::vector<char32_t> decodeUtf8(std::string_view text) {
        std::vector<char32_t> out;
        out.reserve(text.size());
        std::size_t i = 0;
        while (i < text.size()) {
            const auto b0 = static_cast<unsigned char>(text[i]);
            std::size_t length = 0;
            char32_t cp = 0;
            if (b0 < 0x80) {
                length = 1;
                cp = b0;
            } else if ((b0 & 0xE0) == 0xC0) {
                length = 2;
                cp = b0 & 0x1F;
            } else if ((b0 & 0xF0) == 0xE0) {
                length = 3;
                cp = b0 & 0x0F;
            } else if ((b0 & 0xF8) == 0xF0) {
                length = 4;
                cp = b0 & 0x07;
            }
            bool valid = length > 0 && i + length <= text.size();
            for (std::size_t k = 1; valid && k < length; ++k) {
                const auto b = static_cast<unsigned char>(text[i + k]);
                if ((b & 0xC0) != 0x80) {
                    valid = false;
                } else {
                    cp = (cp << 6) | (b & 0x3F);
                }
            }
            if (!valid) {
                out.push_back(0xFFFD);
                ++i;
                continue;
            }
            out.push_back(cp);
            i += length;
        }
        return out;
    }

    Face* loadFace(FcPattern* match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || file == nullptr) {
            FcPatternDestroy(match);
            return nullptr;
        }
        int index = 0;
        FcPatternGetInteger(match, FC_INDEX, 0, &index);
        FcBool embolden = FcFalse;
        FcPatternGetBool(match, FC_EMBOLDEN, 0, &embolden);
        FcMatrix* matrix = nullptr;
        const bool hasMatrix = FcPatternGetMatrix(match, FC_MATRIX, 0, &matrix) == FcResultMatch;

        std::string key = reinterpret_cast<const char*>(file);
        key += '#';
        key += std::to_string(index);
        key += embolden ? "b" : "";
        key += hasMatrix ? "m" : "";
        if (const auto it = faces_.find(key); it != faces_.end()) {
            FcPatternDestroy(match);
            return it->second;
        }

        hb_blob_t* blob = hb_blob_create_from_file(reinterpret_cast<const char*>(file));
        if (blob == nullptr || hb_blob_get_length(blob) == 0) {
            hb_blob_destroy(blob);
            FcPatternDestroy(match);
            return nullptr;
        }
        // The high 16 bits of FC_INDEX name a variable-font instance.
        hb_face_t* hbFace = hb_face_create(blob, static_cast<unsigned>(index) & 0xFFFFu);
        hb_blob_destroy(blob);
        auto* face = new Face();
        face->upem = static_cast<double>(std::max(1u, hb_face_get_upem(hbFace)));
        face->hbFont = hb_font_create(hbFace);
        hb_face_destroy(hbFace);
        hb_ot_font_set_funcs(face->hbFont);
        const int upem = static_cast<int>(face->upem);
        hb_font_set_scale(face->hbFont, upem, upem);
        hb_font_extents_t extents{};
        if (hb_font_get_h_extents(face->hbFont, &extents)) {
            face->ascender = static_cast<double>(extents.ascender);
            face->descender = -static_cast<double>(extents.descender);
        } else {
            face->ascender = face->upem * 0.8;
            face->descender = face->upem * 0.2;
        }
        face->pattern = match;
        FcPatternGetCharSet(match, FC_CHARSET, 0, &face->charset);
        // cairo honours FC_EMBOLDEN / FC_MATRIX from the render-prepared
        // pattern: synthetic bold and oblique where the family lacks them.
        face->cairoFace = cairo_ft_font_face_create_for_pattern(match);
        faces_.emplace(std::move(key), face);
        return face;
    }

    static FcPattern* basePattern(const StyleKey& style) {
        FcPattern* pattern = FcPatternCreate();
        const bool monospace = std::get<2>(style);
        FcPatternAddString(pattern, FC_FAMILY,
                           reinterpret_cast<const FcChar8*>(monospace ? "monospace" : "sans-serif"));
        FcPatternAddInteger(pattern, FC_WEIGHT, std::get<0>(style));
        FcPatternAddInteger(pattern, FC_SLANT, std::get<1>(style) ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
        return pattern;
    }

    Face* match(FcPattern* pattern) {
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result = FcResultNoMatch;
        FcPattern* matched = FcFontMatch(nullptr, pattern, &result);
        FcPatternDestroy(pattern);
        if (matched == nullptr) return nullptr;
        return loadFace(matched);
    }

    Style& style(const ui::Font& font) {
        const StyleKey key{fcWeight(font.weight), font.italic, font.monospace};
        Style& entry = styles_[key];
        if (entry.primary == nullptr) entry.primary = match(basePattern(key));
        return entry;
    }

    Face* faceFor(Style& entry, char32_t cp) {
        Face* primary = entry.primary;
        hb_codepoint_t glyph = 0;
        if (cp < 0x20 || hb_font_get_nominal_glyph(primary->hbFont, cp, &glyph)) return primary;
        if (const auto it = entry.fallback.find(cp); it != entry.fallback.end()) return it->second;

        Face* chosen = primary;
        int weight = FC_WEIGHT_REGULAR;
        int slant = FC_SLANT_ROMAN;
        FcPatternGetInteger(primary->pattern, FC_WEIGHT, 0, &weight);
        FcPatternGetInteger(primary->pattern, FC_SLANT, 0, &slant);
        FcPattern* pattern = FcPatternCreate();
        FcCharSet* charset = FcCharSetCreate();
        FcCharSetAddChar(charset, static_cast<FcChar32>(cp));
        FcPatternAddCharSet(pattern, FC_CHARSET, charset);
        FcCharSetDestroy(charset);
        FcPatternAddInteger(pattern, FC_WEIGHT, weight);
        FcPatternAddInteger(pattern, FC_SLANT, slant);
        Face* candidate = match(pattern);
        if (candidate != nullptr && candidate->charset != nullptr &&
            FcCharSetHasChar(candidate->charset, static_cast<FcChar32>(cp))) {
            chosen = candidate;
        }
        entry.fallback.emplace(cp, chosen);
        return chosen;
    }

    ShapedLine shapeUncached(std::string_view text, const ui::Font& font) {
        ShapedLine line;
        if (text.empty() || !(font.size > 0.0)) return line;
        Style& entry = style(font);
        if (entry.primary == nullptr) return line;

        const std::vector<char32_t> cps = decodeUtf8(text);
        std::vector<Face*> faces(cps.size());
        for (std::size_t i = 0; i < cps.size(); ++i) faces[i] = faceFor(entry, cps[i]);

        // Codepoints are passed as UTF-32 (hb_codepoint_t).
        const std::vector<hb_codepoint_t> codepoints(cps.begin(), cps.end());
        double pen = 0.0;
        std::size_t start = 0;
        while (start < cps.size()) {
            std::size_t end = start + 1;
            while (end < cps.size() && faces[end] == faces[start]) ++end;
            Face* face = faces[start];
            const double scale = font.size / face->upem;
            line.ascent = std::max(line.ascent, face->ascender * scale);
            line.descent = std::max(line.descent, face->descender * scale);

            hb_buffer_t* buffer = hb_buffer_create();
            // Full context, shaped item = [start, end).
            hb_buffer_add_codepoints(buffer, codepoints.data(), static_cast<int>(codepoints.size()),
                                     static_cast<unsigned>(start), static_cast<int>(end - start));
            hb_buffer_guess_segment_properties(buffer);
            hb_shape(face->hbFont, buffer, nullptr, 0);
            unsigned count = 0;
            const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
            const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, nullptr);
            ShapedRun run;
            run.face = face->cairoFace;
            run.glyphs.reserve(count);
            for (unsigned g = 0; g < count; ++g) {
                ShapedGlyph glyph;
                glyph.index = infos[g].codepoint;
                glyph.x = pen + static_cast<double>(positions[g].x_offset) * scale;
                glyph.y = -static_cast<double>(positions[g].y_offset) * scale;
                run.glyphs.push_back(glyph);
                pen += static_cast<double>(positions[g].x_advance) * scale;
            }
            hb_buffer_destroy(buffer);
            line.runs.push_back(std::move(run));
            start = end;
        }
        line.width = pen;
        return line;
    }

    std::mutex mutex_;
    std::map<StyleKey, Style> styles_;
    std::unordered_map<std::string, Face*> faces_;
    std::unordered_map<std::string, std::shared_ptr<const ShapedLine>> cache_;
};

} // namespace

std::shared_ptr<const ShapedLine> shapeLine(std::string_view text, const ui::Font& font) {
    return FontRegistry::instance().shape(text, font);
}

LineMetrics measureLine(std::string_view text, const ui::Font& font) {
    const auto line = shapeLine(text, font);
    return LineMetrics{line->width, line->ascent, line->descent};
}

void applyFontOptions(cairo_t* cr) {
    cairo_font_options_t* options = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
    cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
    cairo_set_font_options(cr, options);
    cairo_font_options_destroy(options);
}

} // namespace rivet::platform::text
