// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownGraphics.hpp"

#include "app/MarkdownPaintMeasurer.hpp"
#include "markdown/Utf8.hpp"
#include "pdf/BundledFontMetrics.hpp"
#include "pdf/BundledFontOutlines.hpp"
#include "pdf/BundledFonts.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <variant>

namespace rivet::app {

namespace mm = markdown::mermaid;
namespace mt = markdown::math;

static_assert(static_cast<int>(GraphicRole::NumberText) == static_cast<int>(mm::DiagramColor::NumberText),
              "GraphicRole must mirror mermaid::DiagramColor");

namespace {

// Ascent of the preview's text inside its line box (MarkdownPaintMeasurer).
constexpr double kAscentRatio = 0.8;
constexpr int kCubicSteps = 12;

// ------------------------------------------------------------- glyph cache

struct GlyphData {
    std::vector<pdf::PdfPathSegment> outline; // font units, y up
    bool inked = false;
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
};

// Outlines and ink boxes of the bundled glyphs used by formulas, parsed once
// per (face, code point). Bounded by the faces' coverage.
std::shared_ptr<const GlyphData> glyphData(pdf::PdfBundledFont font, char32_t cp) {
    static std::mutex mutex;
    static std::unordered_map<std::uint64_t, std::shared_ptr<const GlyphData>> cache;
    const std::uint64_t key = (static_cast<std::uint64_t>(font) << 32) | cp;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    auto data = std::make_shared<GlyphData>();
    data->outline = pdf::bundledGlyphOutline(font, cp);
    for (const pdf::PdfPathSegment& s : data->outline) {
        if (s.op == pdf::PdfPathSegment::Op::Close) continue;
        const auto note = [&](const pdf::PdfPoint& p) {
            if (!data->inked) {
                data->xMin = data->xMax = p.x;
                data->yMin = data->yMax = p.y;
                data->inked = true;
                return;
            }
            data->xMin = std::min(data->xMin, p.x);
            data->xMax = std::max(data->xMax, p.x);
            data->yMin = std::min(data->yMin, p.y);
            data->yMax = std::max(data->yMax, p.y);
        };
        note(s.p);
        if (s.op == pdf::PdfPathSegment::Op::CubicTo) {
            note(s.c1);
            note(s.c2);
        }
    }
    const std::lock_guard<std::mutex> lock(mutex);
    return cache.try_emplace(key, std::move(data)).first->second;
}

// The face a formula code point is written with, and the code point written.
pdf::PdfBundledFont mathFace(char32_t cp, char32_t& written) {
    const bool control = cp < 0x20 || (cp >= 0x7F && cp < 0xA0);
    written = cp;
    if (!control && pdf::bundledFontCovers(pdf::PdfBundledFont::MathRegular, cp)) {
        return pdf::PdfBundledFont::MathRegular;
    }
    if (!control && pdf::bundledFontCovers(pdf::PdfBundledFont::SansRegular, cp)) {
        return pdf::PdfBundledFont::SansRegular;
    }
    written = U'?';
    return pdf::PdfBundledFont::MathRegular;
}

// Calls f(face, written code point, pen x at `size`, advance at `size`) per code point.
template <typename F>
void forEachMathGlyph(std::string_view utf8, double size, F&& f) {
    double pen = 0.0;
    for (std::size_t i = 0; i < utf8.size();) {
        const markdown::utf8::Decoded d = markdown::utf8::decode(utf8, i);
        i += d.length;
        char32_t written = U'?';
        const pdf::PdfBundledFont face = mathFace(d.valid ? d.codePoint : U'?', written);
        // The advance exactly as the PDF declares it (BundledFontMetrics::pdfWidth).
        const double advance = pdf::bundledFontMetrics(face).pdfWidth(written) * size / 1000.0;
        f(face, written, pen, advance);
        pen += advance;
    }
}

// ------------------------------------------------------------ text drawing

void drawBaselineText(ui::PaintContext& context, std::string_view text, const markdown::TextStyle& style,
                      core::Point baseline, double width, const ui::Color& color) {
    if (text.empty() || !(style.size > 0.0)) return;
    const ui::Font font = MarkdownPaintMeasurer::fontFor(style);
    const double lh = context.measureText(text, font).height;
    if (!(lh > 0.0)) return;
    context.drawText(text, core::Rect{baseline.x, baseline.y - kAscentRatio * lh, width + 8.0, lh}, font, color,
                     ui::TextAlign::Left);
}

// ---------------------------------------------------------- path utilities

void flatten(const std::vector<mm::PathSegment>& segments, std::vector<std::vector<core::Point>>& out) {
    std::vector<core::Point>* current = nullptr;
    core::Point start;
    for (const mm::PathSegment& s : segments) {
        switch (s.kind) {
        case mm::PathSegment::Kind::MoveTo:
            out.emplace_back().push_back(s.p);
            current = &out.back();
            start = s.p;
            break;
        case mm::PathSegment::Kind::LineTo:
            if (current == nullptr) {
                out.emplace_back().push_back(s.p);
                current = &out.back();
                start = s.p;
            } else {
                current->push_back(s.p);
            }
            break;
        case mm::PathSegment::Kind::CubicTo: {
            if (current == nullptr) break;
            const core::Point p0 = current->back();
            for (int k = 1; k <= kCubicSteps; ++k) {
                const double t = static_cast<double>(k) / kCubicSteps;
                const double u = 1.0 - t;
                const double a = u * u * u;
                const double b = 3.0 * u * u * t;
                const double c = 3.0 * u * t * t;
                const double d = t * t * t;
                current->push_back(core::Point{a * p0.x + b * s.c1.x + c * s.c2.x + d * s.p.x,
                                               a * p0.y + b * s.c1.y + c * s.c2.y + d * s.p.y});
            }
            break;
        }
        case mm::PathSegment::Kind::Close:
            if (current != nullptr) {
                current->push_back(start);
                current = nullptr;
            }
            break;
        }
    }
}

ui::Path toUiPath(const std::vector<mm::PathSegment>& segments, const markdown::LayoutGraphic& g, core::Point offset) {
    const auto map = [&](core::Point p) {
        return core::Point{g.origin.x + p.x * g.scale + offset.x, g.origin.y + p.y * g.scale + offset.y};
    };
    ui::Path path;
    for (const mm::PathSegment& s : segments) {
        switch (s.kind) {
        case mm::PathSegment::Kind::MoveTo: path.moveTo(map(s.p)); break;
        case mm::PathSegment::Kind::LineTo: path.lineTo(map(s.p)); break;
        case mm::PathSegment::Kind::CubicTo: path.cubicTo(map(s.c1), map(s.c2), map(s.p)); break;
        case mm::PathSegment::Kind::Close: path.close(); break;
        }
    }
    return path;
}

void strokeOutline(ui::PaintContext& context, const std::vector<mm::PathSegment>& segments,
                   const markdown::LayoutGraphic& g, core::Point offset, const ui::Color& color, double width,
                   mm::DashStyle dash, bool roundJoins) {
    const std::vector<double> pattern = diagramDashPattern(dash, width);
    const ui::LineCap cap = roundJoins ? ui::LineCap::Round : ui::LineCap::Butt;
    const ui::LineJoin join = roundJoins ? ui::LineJoin::Round : ui::LineJoin::Miter;
    if (pattern.empty()) {
        context.strokePath(toUiPath(segments, g, offset), color, width * g.scale, cap, join);
        return;
    }
    std::vector<mm::PathSegment> pieces;
    for (const std::vector<core::Point>& piece : dashedPieces(segments, pattern)) {
        for (std::size_t i = 0; i < piece.size(); ++i) {
            mm::PathSegment s;
            s.kind = i == 0 ? mm::PathSegment::Kind::MoveTo : mm::PathSegment::Kind::LineTo;
            s.p = piece[i];
            pieces.push_back(s);
        }
    }
    if (!pieces.empty()) context.strokePath(toUiPath(pieces, g, offset), color, width * g.scale, cap, join);
}

} // namespace

// ------------------------------------------------------------------ colours

ui::Color markdownGraphicColor(const MarkdownPreviewPalette& p, GraphicRole role) {
    switch (role) {
    case GraphicRole::Text: return p.text;
    case GraphicRole::NodeFill: return p.tableHeaderBackground;
    case GraphicRole::NodeStroke: return p.marker;
    case GraphicRole::EdgeStroke: return p.marker;
    case GraphicRole::LabelFill: return p.background;
    case GraphicRole::ClusterFill: return p.codeBackground;
    case GraphicRole::ClusterStroke: return p.rule;
    case GraphicRole::NoteFill: return p.inlineCodeBackground;
    case GraphicRole::NoteStroke: return p.rule;
    case GraphicRole::FrameStroke: return p.marker;
    case GraphicRole::FrameFill: return p.tableHeaderBackground;
    case GraphicRole::Lifeline: return p.rule;
    case GraphicRole::NumberFill: return p.link;
    case GraphicRole::NumberText: return p.background;
    case GraphicRole::MathError: return p.error;
    case GraphicRole::Notice: return p.error;
    }
    return p.text;
}

pdf::PdfColor toPdfColor(const ui::Color& c) {
    const auto over = [&](double v) {
        const double a = std::clamp(c.a, 0.0, 1.0);
        return static_cast<float>(std::clamp(v * a + (1.0 - a), 0.0, 1.0));
    };
    return pdf::PdfColor{over(c.r), over(c.g), over(c.b)};
}

// --------------------------------------------------------------------- math

markdown::TextStyle mathTextStyle(mt::MathFont font, double size) {
    markdown::TextStyle style;
    style.kind = markdown::TextKind::Body;
    style.bold = font == mt::MathFont::TextBold;
    style.size = size;
    return style;
}

std::vector<MathGlyphPiece> mathGlyphPieces(std::string_view utf8, double size) {
    std::vector<MathGlyphPiece> pieces;
    forEachMathGlyph(utf8, size, [&](pdf::PdfBundledFont face, char32_t written, double x, double advance) {
        if (pieces.empty() || pieces.back().font != face) {
            MathGlyphPiece& piece = pieces.emplace_back();
            piece.font = face;
            piece.x = x;
        }
        markdown::utf8::append(pieces.back().utf8, written);
        pieces.back().width += advance;
    });
    return pieces;
}

mt::MathGlyphMetrics BundledMathMeasurer::measure(std::string_view utf8, mt::MathFont font, double size) const {
    mt::MathGlyphMetrics m;
    if (font != mt::MathFont::Math) {
        const markdown::TextMetrics t = text_.measure(utf8, mathTextStyle(font, size));
        m.width = t.width;
        m.ascent = t.ascent;
        m.descent = t.descent;
        return m;
    }
    bool inked = false;
    forEachMathGlyph(utf8, size, [&](pdf::PdfBundledFont face, char32_t written, double, double advance) {
        m.width += advance;
        const std::shared_ptr<const GlyphData> glyph = glyphData(face, written);
        if (!glyph->inked) return;
        const double s = size / pdf::bundledFontMetrics(face).unitsPerEm();
        const double ascent = glyph->yMax * s;
        const double descent = -glyph->yMin * s;
        m.ascent = inked ? std::max(m.ascent, ascent) : ascent;
        m.descent = inked ? std::max(m.descent, descent) : descent;
        inked = true;
    });
    return m;
}

void appendMathRunOutlines(ui::Path& path, std::string_view utf8, double size, core::Point origin) {
    forEachMathGlyph(utf8, size, [&](pdf::PdfBundledFont face, char32_t written, double x, double) {
        const std::shared_ptr<const GlyphData> glyph = glyphData(face, written);
        if (!glyph->inked) return;
        const double s = size / pdf::bundledFontMetrics(face).unitsPerEm();
        const auto map = [&](const pdf::PdfPoint& p) { return core::Point{origin.x + x + p.x * s, origin.y - p.y * s}; };
        for (const pdf::PdfPathSegment& seg : glyph->outline) {
            switch (seg.op) {
            case pdf::PdfPathSegment::Op::MoveTo: path.moveTo(map(seg.p)); break;
            case pdf::PdfPathSegment::Op::LineTo: path.lineTo(map(seg.p)); break;
            case pdf::PdfPathSegment::Op::CubicTo: path.cubicTo(map(seg.c1), map(seg.c2), map(seg.p)); break;
            case pdf::PdfPathSegment::Op::Close: path.close(); break;
            }
        }
    });
}

// ------------------------------------------------------------------ diagrams

std::vector<double> diagramDashPattern(mm::DashStyle dash, double strokeWidth) {
    const double w = std::max(strokeWidth, 0.5);
    switch (dash) {
    case mm::DashStyle::Solid: return {};
    case mm::DashStyle::Dashed: return {4.0 * w, 3.0 * w};
    case mm::DashStyle::Dotted: return {1.2 * w, 2.2 * w};
    }
    return {};
}

std::vector<mm::PathSegment> diagramRectSegments(const core::Rect& r, double cornerRadius) {
    using K = mm::PathSegment::Kind;
    std::vector<mm::PathSegment> out;
    const auto seg = [&](K kind, core::Point p, core::Point c1 = {}, core::Point c2 = {}) {
        out.push_back(mm::PathSegment{kind, p, c1, c2});
    };
    const double rad = std::clamp(cornerRadius, 0.0, std::min(r.size.width, r.size.height) / 2.0);
    const double x0 = r.minX();
    const double y0 = r.minY();
    const double x1 = r.maxX();
    const double y1 = r.maxY();
    if (rad <= 0.0) {
        seg(K::MoveTo, {x0, y0});
        seg(K::LineTo, {x1, y0});
        seg(K::LineTo, {x1, y1});
        seg(K::LineTo, {x0, y1});
        seg(K::Close, {});
        return out;
    }
    constexpr double kKappa = 0.5522847498307936;
    const double k = rad * kKappa;
    seg(K::MoveTo, {x0 + rad, y0});
    seg(K::LineTo, {x1 - rad, y0});
    seg(K::CubicTo, {x1, y0 + rad}, {x1 - rad + k, y0}, {x1, y0 + rad - k});
    seg(K::LineTo, {x1, y1 - rad});
    seg(K::CubicTo, {x1 - rad, y1}, {x1, y1 - rad + k}, {x1 - rad + k, y1});
    seg(K::LineTo, {x0 + rad, y1});
    seg(K::CubicTo, {x0, y1 - rad}, {x0 + rad - k, y1}, {x0, y1 - rad + k});
    seg(K::LineTo, {x0, y0 + rad});
    seg(K::CubicTo, {x0 + rad, y0}, {x0, y0 + rad - k}, {x0 + rad - k, y0});
    seg(K::Close, {});
    return out;
}

std::vector<std::vector<core::Point>> dashedPieces(const std::vector<mm::PathSegment>& segments,
                                                   const std::vector<double>& pattern) {
    std::vector<std::vector<core::Point>> polylines;
    flatten(segments, polylines);
    std::vector<std::vector<core::Point>> pieces;
    double total = 0.0;
    for (const double v : pattern) total += v;
    if (pattern.empty() || !(total > 0.0)) return polylines;
    for (const std::vector<core::Point>& line : polylines) {
        std::size_t index = 0;
        double left = pattern[0];
        bool on = true;
        if (on && !line.empty()) pieces.emplace_back().push_back(line.front());
        for (std::size_t i = 1; i < line.size(); ++i) {
            core::Point a = line[i - 1];
            const core::Point b = line[i];
            double length = std::hypot(b.x - a.x, b.y - a.y);
            while (length > 0.0) {
                const double step = std::min(left, length);
                const double t = step / length;
                const core::Point p{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                if (on) pieces.back().push_back(p);
                a = p;
                length -= step;
                left -= step;
                if (left <= 1e-9) {
                    index = (index + 1) % pattern.size();
                    left = pattern[index];
                    on = !on;
                    if (on) pieces.emplace_back().push_back(a);
                }
            }
        }
    }
    std::erase_if(pieces, [](const std::vector<core::Point>& p) { return p.size() < 2; });
    return pieces;
}

// ------------------------------------------------------------------ painting

void paintMarkdownGraphic(ui::PaintContext& context, const markdown::LayoutGraphic& g, core::Point offset,
                          const MarkdownPreviewPalette& palette, const ui::Color& mathColor) {
    const auto map = [&](core::Point p) {
        return core::Point{g.origin.x + p.x * g.scale + offset.x, g.origin.y + p.y * g.scale + offset.y};
    };
    if (g.math != nullptr) {
        const mt::MathScene& scene = *g.math;
        ui::Path normal;
        ui::Path error;
        for (const mt::MathGlyphRun& run : scene.runs) {
            const ui::Color color =
                run.color == mt::MathColor::Error ? markdownGraphicColor(palette, GraphicRole::MathError) : mathColor;
            if (run.font == mt::MathFont::Math) {
                appendMathRunOutlines(run.color == mt::MathColor::Error ? error : normal, run.utf8,
                                      run.size * g.scale, map(run.origin));
            } else {
                drawBaselineText(context, run.utf8, mathTextStyle(run.font, run.size * g.scale), map(run.origin),
                                 run.width * g.scale, color);
            }
        }
        if (!normal.empty()) context.fillPath(normal, mathColor);
        if (!error.empty()) context.fillPath(error, markdownGraphicColor(palette, GraphicRole::MathError));
        for (const mt::MathRule& rule : scene.rules) {
            const core::Point a = map(rule.rect.origin);
            context.fillRect(core::Rect{a.x, a.y, rule.rect.size.width * g.scale, rule.rect.size.height * g.scale},
                             rule.color == mt::MathColor::Error ? markdownGraphicColor(palette, GraphicRole::MathError)
                                                                : mathColor);
        }
        return;
    }
    if (g.diagram == nullptr) return;
    const auto color = [&](mm::DiagramColor c) { return markdownGraphicColor(palette, graphicRole(c)); };
    for (const mm::DiagramItem& item : g.diagram->items) {
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, mm::DiagramRect>) {
                    const std::vector<mm::PathSegment> outline = diagramRectSegments(v.rect, v.cornerRadius);
                    if (v.filled) context.fillPath(toUiPath(outline, g, offset), color(v.fill));
                    if (v.stroked) strokeOutline(context, outline, g, offset, color(v.stroke), v.strokeWidth, v.dash, false);
                } else if constexpr (std::is_same_v<T, mm::DiagramPath>) {
                    if (v.filled) context.fillPath(toUiPath(v.segments, g, offset), color(v.fill));
                    if (v.stroked) {
                        strokeOutline(context, v.segments, g, offset, color(v.stroke), v.strokeWidth, v.dash, false);
                    }
                } else if constexpr (std::is_same_v<T, mm::DiagramPolyline>) {
                    std::vector<mm::PathSegment> line;
                    for (std::size_t i = 0; i < v.points.size(); ++i) {
                        line.push_back(mm::PathSegment{
                            i == 0 ? mm::PathSegment::Kind::MoveTo : mm::PathSegment::Kind::LineTo, v.points[i], {}, {}});
                    }
                    if (line.size() >= 2) strokeOutline(context, line, g, offset, color(v.color), v.width, v.dash, true);
                } else {
                    drawBaselineText(context, v.utf8, mm::diagramTextStyle(v.font, v.size * g.scale), map(v.origin),
                                     v.width * g.scale, color(v.color));
                }
            },
            item);
    }
}

} // namespace rivet::app
