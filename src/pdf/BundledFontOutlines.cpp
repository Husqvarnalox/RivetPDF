// SPDX-License-Identifier: MPL-2.0

#include "pdf/BundledFontOutlines.hpp"

#include "pdf/BundledFontMetrics.hpp"
#include "pdf/BundledFonts.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>

namespace rivet::pdf {

namespace {

constexpr int kMaxCompositeDepth = 8;
constexpr std::size_t kMaxComponents = 64;
constexpr std::size_t kMaxPointsPerGlyph = 4096;
constexpr std::size_t kMaxContours = 1024;

// Bounds-checked big-endian reads (same contract as BundledFontMetrics').
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : data_(data) {}

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

struct GlyphPoint {
    double x = 0.0;
    double y = 0.0;
    bool onCurve = true;
};

struct Contours {
    std::vector<std::vector<GlyphPoint>> contours;
    std::size_t points = 0;
};

struct Font {
    Reader data;
    Table glyf;
    Table loca;
    bool longLoca = false;
    std::uint16_t numGlyphs = 0;
};

// The byte range of glyph `id` inside glyf (nullopt = empty or invalid).
std::optional<Table> glyphRange(const Font& font, std::uint16_t id) {
    if (id >= font.numGlyphs) return std::nullopt;
    std::size_t start = 0;
    std::size_t end = 0;
    if (font.longLoca) {
        const auto a = font.data.u32(font.loca.offset + 4u * id);
        const auto b = font.data.u32(font.loca.offset + 4u * (id + 1u));
        if (!a || !b || 4u * (static_cast<std::size_t>(id) + 2u) > font.loca.length) return std::nullopt;
        start = *a;
        end = *b;
    } else {
        const auto a = font.data.u16(font.loca.offset + 2u * id);
        const auto b = font.data.u16(font.loca.offset + 2u * (id + 1u));
        if (!a || !b || 2u * (static_cast<std::size_t>(id) + 2u) > font.loca.length) return std::nullopt;
        start = 2u * static_cast<std::size_t>(*a);
        end = 2u * static_cast<std::size_t>(*b);
    }
    if (end <= start || end > font.glyf.length) return std::nullopt;
    return Table{font.glyf.offset + start, end - start};
}

bool readSimple(const Font& font, const Table& glyph, int contourCount, Contours& out) {
    const Reader& r = font.data;
    if (static_cast<std::size_t>(contourCount) > kMaxContours) return false;
    std::size_t at = glyph.offset + 10;
    std::vector<std::uint16_t> ends(static_cast<std::size_t>(contourCount));
    for (std::size_t i = 0; i < ends.size(); ++i) {
        const auto e = r.u16(at);
        if (!e) return false;
        ends[i] = *e;
        if (i > 0 && ends[i] < ends[i - 1]) return false;
        at += 2;
    }
    const std::size_t pointCount = ends.empty() ? 0 : static_cast<std::size_t>(ends.back()) + 1;
    if (pointCount == 0) return true;
    if (out.points + pointCount > kMaxPointsPerGlyph) return false;
    const auto instructionLength = r.u16(at);
    if (!instructionLength) return false;
    at += 2 + *instructionLength;

    std::vector<std::uint8_t> flags;
    flags.reserve(pointCount);
    while (flags.size() < pointCount) {
        const auto flag = r.u8(at++);
        if (!flag) return false;
        flags.push_back(*flag);
        if ((*flag & 0x08u) != 0) {
            const auto repeat = r.u8(at++);
            if (!repeat) return false;
            for (unsigned k = 0; k < *repeat && flags.size() < pointCount; ++k) flags.push_back(*flag);
        }
    }

    std::vector<GlyphPoint> points(pointCount);
    int value = 0;
    for (std::size_t i = 0; i < pointCount; ++i) {
        const std::uint8_t f = flags[i];
        if ((f & 0x02u) != 0) {
            const auto dx = r.u8(at++);
            if (!dx) return false;
            value += (f & 0x10u) != 0 ? *dx : -static_cast<int>(*dx);
        } else if ((f & 0x10u) == 0) {
            const auto dx = r.s16(at);
            if (!dx) return false;
            at += 2;
            value += *dx;
        }
        points[i].x = value;
        points[i].onCurve = (f & 0x01u) != 0;
    }
    value = 0;
    for (std::size_t i = 0; i < pointCount; ++i) {
        const std::uint8_t f = flags[i];
        if ((f & 0x04u) != 0) {
            const auto dy = r.u8(at++);
            if (!dy) return false;
            value += (f & 0x20u) != 0 ? *dy : -static_cast<int>(*dy);
        } else if ((f & 0x20u) == 0) {
            const auto dy = r.s16(at);
            if (!dy) return false;
            at += 2;
            value += *dy;
        }
        points[i].y = value;
    }
    if (at > glyph.offset + glyph.length) return false;

    std::size_t first = 0;
    for (const std::uint16_t end : ends) {
        out.contours.emplace_back(points.begin() + static_cast<std::ptrdiff_t>(first),
                                  points.begin() + static_cast<std::ptrdiff_t>(end) + 1);
        first = static_cast<std::size_t>(end) + 1;
    }
    out.points += pointCount;
    return true;
}

bool readGlyph(const Font& font, std::uint16_t id, int depth, Contours& out);

bool readComposite(const Font& font, const Table& glyph, int depth, Contours& out) {
    const Reader& r = font.data;
    std::size_t at = glyph.offset + 10;
    for (std::size_t component = 0; component < kMaxComponents; ++component) {
        const auto flags = r.u16(at);
        const auto child = r.u16(at + 2);
        if (!flags || !child) return false;
        at += 4;
        double dx = 0.0;
        double dy = 0.0;
        if ((*flags & 0x0001u) != 0) { // ARG_1_AND_2_ARE_WORDS
            const auto a = r.s16(at);
            const auto b = r.s16(at + 2);
            if (!a || !b) return false;
            dx = *a;
            dy = *b;
            at += 4;
        } else {
            const auto a = r.u8(at);
            const auto b = r.u8(at + 1);
            if (!a || !b) return false;
            dx = static_cast<std::int8_t>(*a);
            dy = static_cast<std::int8_t>(*b);
            at += 2;
        }
        if ((*flags & 0x0002u) == 0) { // point matching: not used by the bundled fonts
            dx = 0.0;
            dy = 0.0;
        }
        double a = 1.0, b = 0.0, c = 0.0, d = 1.0;
        const auto f2dot14 = [&](std::size_t offset) -> std::optional<double> {
            const auto v = r.s16(offset);
            if (!v) return std::nullopt;
            return static_cast<double>(*v) / 16384.0;
        };
        if ((*flags & 0x0008u) != 0) { // WE_HAVE_A_SCALE
            const auto s = f2dot14(at);
            if (!s) return false;
            a = d = *s;
            at += 2;
        } else if ((*flags & 0x0040u) != 0) { // X_AND_Y_SCALE
            const auto sx = f2dot14(at);
            const auto sy = f2dot14(at + 2);
            if (!sx || !sy) return false;
            a = *sx;
            d = *sy;
            at += 4;
        } else if ((*flags & 0x0080u) != 0) { // TWO_BY_TWO
            const auto m0 = f2dot14(at);
            const auto m1 = f2dot14(at + 2);
            const auto m2 = f2dot14(at + 4);
            const auto m3 = f2dot14(at + 6);
            if (!m0 || !m1 || !m2 || !m3) return false;
            a = *m0;
            b = *m1;
            c = *m2;
            d = *m3;
            at += 8;
        }
        Contours part;
        part.points = out.points;
        if (!readGlyph(font, *child, depth + 1, part)) return false;
        for (auto& contour : part.contours) {
            for (GlyphPoint& p : contour) {
                const double x = p.x;
                const double y = p.y;
                p.x = a * x + c * y + dx;
                p.y = b * x + d * y + dy;
            }
            out.contours.push_back(std::move(contour));
        }
        out.points = part.points;
        if ((*flags & 0x0020u) == 0) return true; // no MORE_COMPONENTS
    }
    return false; // too many components
}

bool readGlyph(const Font& font, std::uint16_t id, int depth, Contours& out) {
    if (depth > kMaxCompositeDepth) return false;
    const std::optional<Table> glyph = glyphRange(font, id);
    if (!glyph) return true; // empty glyph (space) or out of range: nothing to draw
    const auto contourCount = font.data.s16(glyph->offset);
    if (!contourCount || glyph->length < 10) return false;
    if (*contourCount >= 0) return readSimple(font, *glyph, *contourCount, out);
    return readComposite(font, *glyph, depth, out);
}

PdfPathSegment segment(PdfPathSegment::Op op, PdfPoint p, PdfPoint c1 = {}, PdfPoint c2 = {}) {
    PdfPathSegment s;
    s.op = op;
    s.p = p;
    s.c1 = c1;
    s.c2 = c2;
    return s;
}

PdfPoint mid(const GlyphPoint& a, const GlyphPoint& b) {
    return PdfPoint{(a.x + b.x) / 2.0, (a.y + b.y) / 2.0};
}

// One closed TrueType contour (on/off-curve points) as path segments.
void appendContour(const std::vector<GlyphPoint>& contour, std::vector<PdfPathSegment>& out) {
    const std::size_t n = contour.size();
    if (n < 2) return;
    // Start on an on-curve point; with none, at the midpoint of the first two.
    std::size_t startIndex = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (contour[i].onCurve) {
            startIndex = i;
            break;
        }
    }
    PdfPoint start;
    std::size_t first = 0;
    if (startIndex == n) {
        start = mid(contour[0], contour[1]);
        first = 1;
    } else {
        start = PdfPoint{contour[startIndex].x, contour[startIndex].y};
        first = startIndex + 1;
    }
    out.push_back(segment(PdfPathSegment::Op::MoveTo, start));
    PdfPoint current = start;
    std::optional<PdfPoint> control;
    const auto quadTo = [&](PdfPoint c, PdfPoint p) {
        const PdfPoint c1{current.x + 2.0 / 3.0 * (c.x - current.x), current.y + 2.0 / 3.0 * (c.y - current.y)};
        const PdfPoint c2{p.x + 2.0 / 3.0 * (c.x - p.x), p.y + 2.0 / 3.0 * (c.y - p.y)};
        out.push_back(segment(PdfPathSegment::Op::CubicTo, p, c1, c2));
        current = p;
    };
    for (std::size_t k = 0; k < n; ++k) {
        const GlyphPoint& gp = contour[(first + k) % n];
        const PdfPoint p{gp.x, gp.y};
        if (gp.onCurve) {
            if (control) {
                quadTo(*control, p);
                control.reset();
            } else {
                out.push_back(segment(PdfPathSegment::Op::LineTo, p));
                current = p;
            }
        } else {
            if (control) quadTo(*control, PdfPoint{(control->x + p.x) / 2.0, (control->y + p.y) / 2.0});
            control = p;
        }
    }
    if (control) {
        quadTo(*control, start);
    }
    out.push_back(segment(PdfPathSegment::Op::Close, {}));
}

} // namespace

std::vector<PdfPathSegment> trueTypeGlyphOutline(std::span<const std::uint8_t> fontProgram, std::uint16_t glyphId) {
    Font font{Reader(fontProgram), {}, {}, false, 0};
    const auto head = findTable(font.data, "head");
    const auto maxp = findTable(font.data, "maxp");
    const auto glyf = findTable(font.data, "glyf");
    const auto loca = findTable(font.data, "loca");
    if (!head || !maxp || !glyf || !loca || head->length < 54 || maxp->length < 6) return {};
    const auto format = font.data.s16(head->offset + 50);
    const auto numGlyphs = font.data.u16(maxp->offset + 4);
    if (!format || !numGlyphs || (*format != 0 && *format != 1)) return {};
    font.glyf = *glyf;
    font.loca = *loca;
    font.longLoca = *format == 1;
    font.numGlyphs = *numGlyphs;

    Contours contours;
    if (glyphId == 0 || !readGlyph(font, glyphId, 0, contours)) return {};
    std::vector<PdfPathSegment> out;
    for (const auto& contour : contours.contours) appendContour(contour, out);
    return out;
}

std::vector<PdfPathSegment> bundledGlyphOutline(PdfBundledFont font, char32_t codepoint) {
    const std::uint16_t glyph = bundledFontMetrics(font).glyphId(codepoint);
    if (glyph == 0) return {};
    return trueTypeGlyphOutline(bundledFontData(font), glyph);
}

} // namespace rivet::pdf
