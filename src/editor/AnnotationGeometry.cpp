// SPDX-License-Identifier: MPL-2.0
#include "editor/AnnotationGeometry.hpp"

#include "core/geometry/Rotation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace rivet::editor::geometry {
namespace {

constexpr double kPi = 3.14159265358979323846;

bool pointInPolygon(core::Point p, const std::array<core::Point, 4>& poly) {
    bool inside = false;
    for (std::size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const core::Point a = poly[i];
        const core::Point b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) inside = !inside;
    }
    return inside;
}

core::Rect insetRect(const core::Rect& r, double d) {
    const double v = std::clamp(d, 0.0, std::max(0.0, std::min(r.size.width, r.size.height) / 2.0));
    return core::Rect{r.origin.x + v, r.origin.y + v, r.size.width - 2.0 * v, r.size.height - 2.0 * v};
}

bool insideEllipse(core::Point p, const core::Rect& bounds) {
    const double a = bounds.size.width / 2.0;
    const double b = bounds.size.height / 2.0;
    if (a <= 1e-9 || b <= 1e-9) return false;
    const core::Point c = bounds.center();
    const double nx = (p.x - c.x) / a;
    const double ny = (p.y - c.y) / b;
    return nx * nx + ny * ny <= 1.0;
}

DisplayPath mapPath(const pdf::PdfPageView& view, const pdf::PdfAppearancePath& path) {
    DisplayPath out;
    out.fill = path.fill;
    out.stroke = path.stroke;
    out.strokeWidth = path.strokeWidth;
    out.roundJoins = path.roundJoins;
    out.segments.reserve(path.segments.size());
    for (const pdf::PdfPathSegment& seg : path.segments) {
        DisplayPathSegment mapped;
        mapped.op = seg.op;
        if (seg.op != pdf::PdfPathSegment::Op::Close) mapped.p = toDisplay(view, seg.p);
        if (seg.op == pdf::PdfPathSegment::Op::CubicTo) {
            mapped.c1 = toDisplay(view, seg.c1);
            mapped.c2 = toDisplay(view, seg.c2);
        }
        out.segments.push_back(mapped);
    }
    return out;
}

} // namespace

double distanceToSegment(core::Point p, core::Point a, core::Point b) {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double lenSq = dx * dx + dy * dy;
    if (lenSq <= 0.0) return std::hypot(p.x - a.x, p.y - a.y);
    const double t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

bool pointInQuad(core::Point p, const DisplayQuad& quad, double tolerance) {
    // TL, TR, BL, BR -> polygon order TL, TR, BR, BL.
    const std::array<core::Point, 4> poly{quad[0], quad[1], quad[3], quad[2]};
    if (pointInPolygon(p, poly)) return true;
    if (tolerance <= 0.0) return false;
    for (std::size_t i = 0; i < poly.size(); ++i) {
        if (distanceToSegment(p, poly[i], poly[(i + 1) % poly.size()]) <= tolerance) return true;
    }
    return false;
}

double distanceToRectOutline(core::Point p, const core::Rect& rect) {
    const double l = rect.minX();
    const double r = rect.maxX();
    const double t = rect.minY();
    const double b = rect.maxY();
    const bool inside = p.x >= l && p.x <= r && p.y >= t && p.y <= b;
    if (inside) return std::min({p.x - l, r - p.x, p.y - t, b - p.y});
    const double dx = std::max({l - p.x, 0.0, p.x - r});
    const double dy = std::max({t - p.y, 0.0, p.y - b});
    return std::hypot(dx, dy);
}

double distanceToEllipseOutline(core::Point p, const core::Rect& bounds) {
    const double a = bounds.size.width / 2.0;
    const double b = bounds.size.height / 2.0;
    const core::Point c = bounds.center();
    if (a <= 1e-9 || b <= 1e-9) {
        // Degenerate: a segment (or a point).
        return distanceToSegment(p, core::Point{c.x - a, c.y - b}, core::Point{c.x + a, c.y + b});
    }
    const double px = std::fabs(p.x - c.x);
    const double py = std::fabs(p.y - c.y);
    // Closest point on the first-quadrant arc by the standard fixed-point
    // iteration on the evolute (converges in a handful of steps). It is
    // degenerate for points near the centre (inside the evolute), so several
    // start angles are tried and the vertices are always candidates; the
    // minimum of true distances to arc points is returned.
    double best = std::min(std::hypot(px - a, py), std::hypot(px, py - b));
    for (const double start : {std::atan2(py * a, px * b), 0.3, 0.8, 1.3}) {
        double t = start;
        for (int i = 0; i < 12; ++i) {
            const double cx = std::cos(t);
            const double sx = std::sin(t);
            const double x = a * cx;
            const double y = b * sx;
            const double ex = (a * a - b * b) * cx * cx * cx / a;
            const double ey = (b * b - a * a) * sx * sx * sx / b;
            const double rx = x - ex;
            const double ry = y - ey;
            const double qx = px - ex;
            const double qy = py - ey;
            const double r = std::hypot(rx, ry);
            const double q = std::hypot(qx, qy);
            if (q <= 1e-12) break;
            const double tx = std::clamp((qx * r / q + ex) / a, 0.0, 1.0);
            const double ty = std::clamp((qy * r / q + ey) / b, 0.0, 1.0);
            t = std::atan2(ty, tx);
        }
        best = std::min(best, std::hypot(px - a * std::cos(t), py - b * std::sin(t)));
    }
    return best;
}

std::array<core::Point, 2> arrowHeadWings(core::Point start, core::Point end, double width) {
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    const double len = std::hypot(dx, dy);
    if (len > 0.0) {
        dx /= len;
        dy /= len;
    } else {
        dx = 1.0;
        dy = 0.0;
    }
    const double headLength = std::max(6.0, 3.0 * width);
    const double c = std::cos(kPi / 6.0);
    const double s = std::sin(kPi / 6.0);
    return {core::Point{end.x - headLength * (dx * c - dy * s), end.y - headLength * (dx * s + dy * c)},
            core::Point{end.x - headLength * (dx * c + dy * s), end.y - headLength * (-dx * s + dy * c)}};
}

std::vector<core::Point> reduceStroke(std::span<const core::Point> points, double tolerance) {
    const std::size_t n = points.size();
    if (n <= 2) return std::vector<core::Point>(points.begin(), points.end());
    std::vector<bool> keep(n, false);
    keep[0] = true;
    keep[n - 1] = true;
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    stack.emplace_back(0, n - 1);
    while (!stack.empty()) {
        const auto [first, last] = stack.back();
        stack.pop_back();
        if (last <= first + 1) continue;
        double worst = -1.0;
        std::size_t worstAt = first;
        for (std::size_t i = first + 1; i < last; ++i) {
            const double d = distanceToSegment(points[i], points[first], points[last]);
            if (d > worst) {
                worst = d;
                worstAt = i;
            }
        }
        if (worst > tolerance) {
            keep[worstAt] = true;
            stack.emplace_back(first, worstAt);
            stack.emplace_back(worstAt, last);
        }
    }
    std::vector<core::Point> out;
    for (std::size_t i = 0; i < n; ++i) {
        if (keep[i]) out.push_back(points[i]);
    }
    return out;
}

core::Point toDisplay(const pdf::PdfPageView& view, const pdf::PdfPoint& point) {
    return pdf::userToDisplay(view, point.x, point.y);
}

pdf::PdfPoint toUser(const pdf::PdfPageView& view, core::Point point) {
    const auto [x, y] = pdf::displayToUser(view, point);
    return pdf::PdfPoint{x, y};
}

core::Rect boxToDisplayRect(const pdf::PdfPageView& view, const pdf::PdfBox& box) {
    const core::Point a = pdf::userToDisplay(view, box.left, box.bottom);
    const core::Point b = pdf::userToDisplay(view, box.right, box.top);
    return core::Rect{core::Point{std::min(a.x, b.x), std::min(a.y, b.y)},
                      core::Size{std::fabs(b.x - a.x), std::fabs(b.y - a.y)}};
}

pdf::PdfBox rectToUserBox(const pdf::PdfPageView& view, const core::Rect& rect) {
    return pdf::displayRectToUserBox(view, rect);
}

AnnotationView makeAnnotationView(core::AnnotationId id, const pdf::PdfAnnotationData& data,
                                  const pdf::PdfPageView& view, bool drawnByOverlay) {
    AnnotationView out;
    out.id = id;
    out.kind = data.kind;
    out.caps = annotationCaps(data.kind);
    out.bounds = boxToDisplayRect(view, data.rect);
    for (const pdf::PdfQuad& q : data.quads) {
        out.quads.push_back(DisplayQuad{toDisplay(view, q.p1), toDisplay(view, q.p2), toDisplay(view, q.p3),
                                        toDisplay(view, q.p4)});
    }
    for (const auto& stroke : data.inkStrokes) {
        std::vector<core::Point> mapped;
        mapped.reserve(stroke.size());
        for (const pdf::PdfPoint& p : stroke) mapped.push_back(toDisplay(view, p));
        out.strokes.push_back(std::move(mapped));
    }
    out.lineStart = toDisplay(view, data.lineStart);
    out.lineEnd = toDisplay(view, data.lineEnd);
    out.style = AnnotationStyle{data.color, data.interiorColor, data.opacity, data.borderWidth};
    out.contents = data.contents;
    out.author = data.author;
    out.stampName = data.stampName;
    out.drawnByOverlay = drawnByOverlay;
    if (drawnByOverlay) {
        const pdf::PdfAppearance appearance = pdf::buildAppearance(data);
        DisplayAppearance mapped;
        mapped.opacity = appearance.opacity;
        mapped.paths.reserve(appearance.paths.size());
        for (const pdf::PdfAppearancePath& path : appearance.paths) mapped.paths.push_back(mapPath(view, path));
        const int viewDegrees = core::rotationDegrees(view.rotation);
        for (const pdf::PdfAppearanceText& text : appearance.texts) {
            DisplayText t;
            t.text = text.text;
            t.box = boxToDisplayRect(view, text.box);
            t.rotation = ((text.rotation + viewDegrees) % 360 + 360) % 360;
            t.color = text.color;
            t.bold = text.bold;
            mapped.texts.push_back(std::move(t));
        }
        out.appearance = std::move(mapped);
    }
    return out;
}

bool hitsAnnotation(const AnnotationView& view, core::Point point, double tolerancePoints) {
    const double tol = std::max(0.0, tolerancePoints);
    const double half = static_cast<double>(view.style.borderWidth) / 2.0;
    switch (view.kind) {
    case pdf::PdfAnnotationKind::Highlight:
    case pdf::PdfAnnotationKind::Underline:
    case pdf::PdfAnnotationKind::StrikeOut:
        for (const DisplayQuad& quad : view.quads) {
            if (pointInQuad(point, quad, tol)) return true;
        }
        return false;
    case pdf::PdfAnnotationKind::Note:
    case pdf::PdfAnnotationKind::Stamp:
        return point.x >= view.bounds.minX() - tol && point.x <= view.bounds.maxX() + tol &&
               point.y >= view.bounds.minY() - tol && point.y <= view.bounds.maxY() + tol;
    case pdf::PdfAnnotationKind::Square:
    case pdf::PdfAnnotationKind::Circle: {
        // The stroke is centred on the bounds inset by half its width.
        const core::Rect centre = insetRect(view.bounds, half);
        const bool square = view.kind == pdf::PdfAnnotationKind::Square;
        if (view.style.interiorColor.has_value() &&
            (square ? centre.contains(point) : insideEllipse(point, centre))) {
            return true;
        }
        const double distance = square ? distanceToRectOutline(point, centre) : distanceToEllipseOutline(point, centre);
        return distance <= half + tol;
    }
    case pdf::PdfAnnotationKind::Ink:
        for (const auto& stroke : view.strokes) {
            if (stroke.size() == 1 && std::hypot(point.x - stroke[0].x, point.y - stroke[0].y) <= half + tol) {
                return true;
            }
            for (std::size_t i = 1; i < stroke.size(); ++i) {
                if (distanceToSegment(point, stroke[i - 1], stroke[i]) <= half + tol) return true;
            }
        }
        return false;
    case pdf::PdfAnnotationKind::Line:
    case pdf::PdfAnnotationKind::Arrow: {
        if (distanceToSegment(point, view.lineStart, view.lineEnd) <= half + tol) return true;
        if (view.kind == pdf::PdfAnnotationKind::Arrow) {
            const auto wings = arrowHeadWings(view.lineStart, view.lineEnd, static_cast<double>(view.style.borderWidth));
            return distanceToSegment(point, wings[0], view.lineEnd) <= half + tol ||
                   distanceToSegment(point, wings[1], view.lineEnd) <= half + tol;
        }
        return false;
    }
    case pdf::PdfAnnotationKind::Other:
        return false;
    }
    return false;
}

} // namespace rivet::editor::geometry
