// SPDX-License-Identifier: MPL-2.0

#include "pdf/BundledFontMetrics.hpp"

#include "pdf/BundledFonts.hpp"
#include "pdf/PdfTextLayout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>

namespace rivet::pdf {

namespace {

// Most code points one cmap may contribute (format 12 groups can describe
// huge ranges; the bundled subsets map a few hundred).
constexpr std::size_t kMaxMappedCodePoints = 200000;

// Bounds-checked big-endian reads over a byte range.
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

    std::size_t size() const { return data_.size(); }

    bool has(std::size_t offset, std::size_t length) const {
        return offset <= data_.size() && length <= data_.size() - offset;
    }
    std::optional<std::uint8_t> u8(std::size_t offset) const {
        if (!has(offset, 1)) return std::nullopt;
        return data_[offset];
    }
    std::optional<std::uint16_t> u16(std::size_t offset) const {
        if (!has(offset, 2)) return std::nullopt;
        return static_cast<std::uint16_t>((static_cast<unsigned>(data_[offset]) << 8) | data_[offset + 1]);
    }
    std::optional<std::int16_t> s16(std::size_t offset) const {
        const auto v = u16(offset);
        if (!v) return std::nullopt;
        return static_cast<std::int16_t>(*v);
    }
    std::optional<std::uint32_t> u32(std::size_t offset) const {
        if (!has(offset, 4)) return std::nullopt;
        return (static_cast<std::uint32_t>(data_[offset]) << 24) | (static_cast<std::uint32_t>(data_[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(data_[offset + 2]) << 8) | static_cast<std::uint32_t>(data_[offset + 3]);
    }
    // A sub-range [offset, offset + length), clipped to the data.
    Reader sub(std::size_t offset, std::size_t length) const {
        if (offset > data_.size()) return Reader({});
        return Reader(data_.subspan(offset, std::min(length, data_.size() - offset)));
    }

private:
    std::span<const std::uint8_t> data_;
};

struct Table {
    std::size_t offset = 0;
    std::size_t length = 0;
};

std::optional<Table> findTable(const Reader& font, const char (&tag)[5]) {
    const auto numTables = font.u16(4);
    if (!numTables) return std::nullopt;
    for (std::size_t i = 0; i < *numTables; ++i) {
        const std::size_t record = 12 + 16 * i;
        if (!font.has(record, 16)) return std::nullopt;
        bool match = true;
        for (std::size_t k = 0; k < 4; ++k) {
            if (font.u8(record + k) != static_cast<std::uint8_t>(tag[k])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        const auto offset = font.u32(record + 8);
        const auto length = font.u32(record + 12);
        if (!offset || !length || !font.has(*offset, *length)) return std::nullopt;
        return Table{*offset, *length};
    }
    return std::nullopt;
}

using CmapEntries = std::vector<std::pair<char32_t, std::uint16_t>>;

void addMapping(CmapEntries& out, std::uint32_t cp, std::uint32_t glyph, std::size_t numGlyphs) {
    if (glyph == 0 || glyph >= numGlyphs || cp > 0x10FFFF || out.size() >= kMaxMappedCodePoints) return;
    out.emplace_back(static_cast<char32_t>(cp), static_cast<std::uint16_t>(glyph));
}

bool parseFormat4(const Reader& sub, std::size_t numGlyphs, CmapEntries& out) {
    const auto length = sub.u16(2);
    const auto segX2 = sub.u16(6);
    if (!length || !segX2 || *segX2 == 0 || (*segX2 % 2) != 0) return false;
    const Reader table = sub.sub(0, *length);
    const std::size_t segCount = *segX2 / 2u;
    const std::size_t endCodes = 14;
    const std::size_t startCodes = endCodes + 2 * segCount + 2;
    const std::size_t deltas = startCodes + 2 * segCount;
    const std::size_t rangeOffsets = deltas + 2 * segCount;
    if (!table.has(rangeOffsets, 2 * segCount)) return false;
    for (std::size_t i = 0; i < segCount; ++i) {
        const std::uint16_t end = *table.u16(endCodes + 2 * i);
        const std::uint16_t start = *table.u16(startCodes + 2 * i);
        const std::uint16_t delta = *table.u16(deltas + 2 * i);
        const std::size_t rangeOffsetPos = rangeOffsets + 2 * i;
        const std::uint16_t rangeOffset = *table.u16(rangeOffsetPos);
        if (start > end) continue;
        for (std::uint32_t c = start; c <= end && c != 0xFFFF; ++c) {
            std::uint32_t glyph = 0;
            if (rangeOffset == 0) {
                glyph = (c + delta) & 0xFFFFu;
            } else {
                const std::size_t address = rangeOffsetPos + rangeOffset + 2 * (c - start);
                const auto raw = table.u16(address);
                if (!raw) break; // the rest of the segment is out of range too
                glyph = *raw == 0 ? 0 : ((*raw + delta) & 0xFFFFu);
            }
            addMapping(out, c, glyph, numGlyphs);
        }
    }
    return true;
}

bool parseFormat12(const Reader& sub, std::size_t numGlyphs, CmapEntries& out) {
    const auto length = sub.u32(4);
    const auto groups = sub.u32(12);
    if (!length || !groups) return false;
    const Reader table = sub.sub(0, *length);
    for (std::size_t g = 0; g < *groups; ++g) {
        const std::size_t at = 16 + 12 * g;
        if (!table.has(at, 12)) break;
        const std::uint32_t first = *table.u32(at);
        const std::uint32_t last = *table.u32(at + 4);
        const std::uint32_t startGlyph = *table.u32(at + 8);
        if (first > last || last > 0x10FFFF) continue;
        for (std::uint32_t c = first; c <= last && out.size() < kMaxMappedCodePoints; ++c) {
            const std::uint64_t glyph = static_cast<std::uint64_t>(startGlyph) + (c - first);
            if (glyph >= numGlyphs) break;
            addMapping(out, c, static_cast<std::uint32_t>(glyph), numGlyphs);
        }
    }
    return true;
}

CmapEntries parseCmap(const Reader& font, const Table& cmap, std::size_t numGlyphs) {
    const Reader table = font.sub(cmap.offset, cmap.length);
    const auto count = table.u16(2);
    if (!count) return {};
    // Preference: (3,10) format 12 > (0,x) format 12 > (3,1) format 4 > (0,x) format 4.
    int bestScore = 0;
    std::size_t bestOffset = 0;
    for (std::size_t i = 0; i < *count; ++i) {
        const std::size_t record = 4 + 8 * i;
        const auto platform = table.u16(record);
        const auto encoding = table.u16(record + 2);
        const auto offset = table.u32(record + 4);
        if (!platform || !encoding || !offset) break;
        const auto format = table.u16(*offset);
        if (!format) continue;
        int score = 0;
        if (*format == 12 && *platform == 3 && *encoding == 10) score = 4;
        else if (*format == 12 && *platform == 0) score = 3;
        else if (*format == 4 && *platform == 3 && *encoding == 1) score = 2;
        else if (*format == 4 && *platform == 0) score = 1;
        if (score > bestScore) {
            bestScore = score;
            bestOffset = *offset;
        }
    }
    CmapEntries entries;
    if (bestScore == 0) return entries;
    const Reader sub = table.sub(bestOffset, table.size());
    const bool ok = bestScore >= 3 ? parseFormat12(sub, numGlyphs, entries) : parseFormat4(sub, numGlyphs, entries);
    if (!ok) return {};
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const auto& a, const auto& b) { return a.first == b.first; }),
                  entries.end());
    return entries;
}

} // namespace

BundledFontMetrics::BundledFontMetrics(std::span<const std::uint8_t> fontProgram) {
    const Reader font(fontProgram);
    const auto head = findTable(font, "head");
    const auto hhea = findTable(font, "hhea");
    const auto maxp = findTable(font, "maxp");
    const auto hmtx = findTable(font, "hmtx");
    const auto cmap = findTable(font, "cmap");
    if (!head || !hhea || !maxp || !hmtx || !cmap) return;

    const auto upem = font.u16(head->offset + 18);
    if (head->length < 54 || !upem || *upem < 16 || *upem > 16384) return;

    if (hhea->length < 36) return;
    const auto ascender = font.s16(hhea->offset + 4);
    const auto descender = font.s16(hhea->offset + 6);
    const auto gap = font.s16(hhea->offset + 8);
    const auto numHMetrics = font.u16(hhea->offset + 34);
    const auto numGlyphs = font.u16(maxp->offset + 4);
    if (maxp->length < 6 || !ascender || !descender || !gap || !numHMetrics || !numGlyphs || *numGlyphs == 0 ||
        *numHMetrics == 0) {
        return;
    }

    const std::size_t metricCount = std::min<std::size_t>(*numHMetrics, *numGlyphs);
    if (hmtx->length < 4 * metricCount) return;
    std::vector<std::uint16_t> advances(*numGlyphs, 0);
    for (std::size_t g = 0; g < metricCount; ++g) {
        advances[g] = *font.u16(hmtx->offset + 4 * g);
    }
    for (std::size_t g = metricCount; g < advances.size(); ++g) {
        advances[g] = advances[metricCount - 1];
    }

    double ascent = *ascender;
    double descent = *descender;
    double lineGap = *gap;
    if (const auto os2 = findTable(font, "OS/2"); os2 && os2->length >= 74) {
        const auto selection = font.u16(os2->offset + 62);
        const auto typoAscender = font.s16(os2->offset + 68);
        const auto typoDescender = font.s16(os2->offset + 70);
        const auto typoLineGap = font.s16(os2->offset + 72);
        if (selection && (*selection & 0x80u) != 0 && typoAscender && typoDescender && typoLineGap) {
            ascent = *typoAscender;
            descent = *typoDescender;
            lineGap = *typoLineGap;
        }
    }

    CmapEntries entries = parseCmap(font, *cmap, *numGlyphs);
    if (entries.empty()) return;

    unitsPerEm_ = *upem;
    ascent_ = ascent;
    descent_ = descent;
    lineGap_ = lineGap;
    cmap_ = std::move(entries);
    advances_ = std::move(advances);
    valid_ = true;
}

std::uint16_t BundledFontMetrics::glyphOf(char32_t cp) const {
    const auto it = std::lower_bound(cmap_.begin(), cmap_.end(), cp,
                                     [](const auto& entry, char32_t value) { return entry.first < value; });
    return (it != cmap_.end() && it->first == cp) ? it->second : std::uint16_t{0};
}

bool BundledFontMetrics::hasGlyph(char32_t cp) const {
    return glyphOf(cp) != 0;
}

double BundledFontMetrics::advance(char32_t cp) const {
    const std::uint16_t glyph = glyphOf(cp);
    return (glyph == 0 || glyph >= advances_.size()) ? 0.0 : static_cast<double>(advances_[glyph]);
}

double BundledFontMetrics::pdfWidth(char32_t cp) const {
    const std::uint16_t glyph = glyphOf(cp);
    if (glyph == 0 || glyph >= advances_.size()) return 0.0;
    // Integer arithmetic: the truncation must match PDFium bit for bit.
    const std::uint32_t units = static_cast<std::uint32_t>(advances_[glyph]) * 1000u /
                                static_cast<std::uint32_t>(unitsPerEm_);
    return static_cast<double>(units);
}

double BundledFontMetrics::textWidth(std::string_view utf8, double fontSize) const {
    double thousandths = 0.0;
    for (const char32_t cp : decodeUtf8(utf8)) thousandths += pdfWidth(cp);
    return thousandths * fontSize / 1000.0;
}

const BundledFontMetrics& bundledFontMetrics(PdfBundledFont font) {
    static const std::array<BundledFontMetrics, 6> all{
        BundledFontMetrics(bundledFontData(PdfBundledFont::SansRegular)),
        BundledFontMetrics(bundledFontData(PdfBundledFont::SansBold)),
        BundledFontMetrics(bundledFontData(PdfBundledFont::SerifRegular)),
        BundledFontMetrics(bundledFontData(PdfBundledFont::SerifBold)),
        BundledFontMetrics(bundledFontData(PdfBundledFont::MonoRegular)),
        BundledFontMetrics(bundledFontData(PdfBundledFont::MonoBold)),
    };
    const auto index = static_cast<std::size_t>(font);
    return all[index < all.size() ? index : 0];
}

} // namespace rivet::pdf
