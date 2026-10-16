// SPDX-License-Identifier: MPL-2.0
#pragma once

// Internal helpers shared by the flowchart and sequence layouts: label
// measurement, centred multi-line text, arrowheads and simple paths.

#include "core/geometry/Point.hpp"
#include "core/geometry/Rect.hpp"
#include "markdown/MarkdownLayout.hpp"
#include "markdown/mermaid/DiagramScene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::markdown::mermaid::detail {

struct LabelBox {
    std::vector<double> lineWidths;
    double width = 0.0;
    double height = 0.0; // lines * lineHeight
    double lineHeight = 0.0;
    double ascent = 0.0;
    double descent = 0.0;
};

inline std::vector<std::string> splitLines(std::string_view text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        out.emplace_back(text.substr(pos, end - pos));
        if (end == text.size()) break;
        pos = end + 1;
    }
    return out;
}

inline LabelBox measureLines(const std::vector<std::string>& lines, const ITextMeasurer& measurer, DiagramFont font,
                             const DiagramStyle& style) {
    LabelBox box;
    const TextStyle ts = diagramTextStyle(font, style.fontSize);
    const TextMetrics probe = measurer.measure("Mg", ts);
    box.ascent = probe.ascent;
    box.descent = probe.descent;
    box.lineHeight = std::max(style.fontSize * style.lineHeightFactor, probe.ascent + probe.descent);
    for (const std::string& line : lines) {
        const double w = line.empty() ? 0.0 : measurer.measure(line, ts).width;
        box.lineWidths.push_back(w);
        box.width = std::max(box.width, w);
    }
    box.height = box.lineHeight * static_cast<double>(lines.size());
    return box;
}

// Lines centred horizontally on `center.x`, the block centred vertically on
// `center.y`.
inline void addCenteredText(DiagramScene& scene, const std::vector<std::string>& lines, const LabelBox& box,
                            core::Point center, DiagramFont font, double size, DiagramColor color) {
    const double top = center.y - box.height / 2.0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].empty()) continue;
        DiagramText text;
        text.utf8 = lines[i];
        text.width = box.lineWidths[i];
        text.font = font;
        text.size = size;
        text.color = color;
        const double lineTop = top + box.lineHeight * static_cast<double>(i);
        const double baseline = lineTop + (box.lineHeight - (box.ascent + box.descent)) / 2.0 + box.ascent;
        text.origin = core::Point(center.x - box.lineWidths[i] / 2.0, baseline);
        scene.items.emplace_back(std::move(text));
    }
}

inline core::Point unitVector(core::Point from, core::Point to) {
    const double dx = to.x - from.x;
    const double dy = to.y - from.y;
    const double len = std::hypot(dx, dy);
    if (len <= 1e-9) return core::Point(0.0, 1.0);
    return core::Point(dx / len, dy / len);
}

inline std::vector<PathSegment> polygon(const std::vector<core::Point>& pts) {
    std::vector<PathSegment> segs;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        PathSegment s;
        s.kind = i == 0 ? PathSegment::Kind::MoveTo : PathSegment::Kind::LineTo;
        s.p = pts[i];
        segs.push_back(s);
    }
    PathSegment close;
    close.kind = PathSegment::Kind::Close;
    segs.push_back(close);
    return segs;
}

inline std::vector<PathSegment> ellipse(const core::Rect& r) {
    constexpr double k = 0.5522847498;
    const double cx = r.minX() + r.size.width / 2.0;
    const double cy = r.minY() + r.size.height / 2.0;
    const double rx = r.size.width / 2.0;
    const double ry = r.size.height / 2.0;
    std::vector<PathSegment> segs;
    PathSegment m;
    m.kind = PathSegment::Kind::MoveTo;
    m.p = core::Point(cx + rx, cy);
    segs.push_back(m);
    const auto cubic = [&](core::Point c1, core::Point c2, core::Point p) {
        PathSegment s;
        s.kind = PathSegment::Kind::CubicTo;
        s.c1 = c1;
        s.c2 = c2;
        s.p = p;
        segs.push_back(s);
    };
    cubic({cx + rx, cy + k * ry}, {cx + k * rx, cy + ry}, {cx, cy + ry});
    cubic({cx - k * rx, cy + ry}, {cx - rx, cy + k * ry}, {cx - rx, cy});
    cubic({cx - rx, cy - k * ry}, {cx - k * rx, cy - ry}, {cx, cy - ry});
    cubic({cx + k * rx, cy - ry}, {cx + rx, cy - k * ry}, {cx + rx, cy});
    PathSegment close;
    close.kind = PathSegment::Kind::Close;
    segs.push_back(close);
    return segs;
}

// Filled triangle with its tip at `tip`, pointing away from `from`.
inline DiagramPath arrowHeadPath(core::Point tip, core::Point from, const DiagramStyle& style) {
    const core::Point d = unitVector(from, tip);
    const core::Point n(-d.y, d.x);
    const core::Point base(tip.x - d.x * style.arrowLength, tip.y - d.y * style.arrowLength);
    const double hw = style.arrowWidth / 2.0;
    DiagramPath path;
    path.segments = polygon({tip, {base.x + n.x * hw, base.y + n.y * hw}, {base.x - n.x * hw, base.y - n.y * hw}});
    path.fill = DiagramColor::EdgeStroke;
    path.stroke = DiagramColor::EdgeStroke;
    path.strokeWidth = 1.0;
    return path;
}

// Where the arrow's shaft ends (the base of the head).
inline core::Point arrowBase(core::Point tip, core::Point from, const DiagramStyle& style) {
    const core::Point d = unitVector(from, tip);
    return core::Point(tip.x - d.x * style.arrowLength, tip.y - d.y * style.arrowLength);
}

} // namespace rivet::markdown::mermaid::detail
