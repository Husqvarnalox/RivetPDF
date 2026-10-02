// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfAnnotation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <utility>

namespace rivet::pdf {

namespace {

constexpr double kKappa = 0.5523;
constexpr double kMaxCoordinate = 1.0e6;
constexpr std::size_t kMaxNameBytes = 256;
constexpr double kPi = 3.14159265358979323846;

bool isReasonable(double v) {
    return std::isfinite(v) && std::fabs(v) <= kMaxCoordinate;
}

bool isReasonable(const PdfPoint& p) {
    return isReasonable(p.x) && isReasonable(p.y);
}

bool isReasonable(const PdfBox& b) {
    return isReasonable(b.left) && isReasonable(b.bottom) && isReasonable(b.right) &&
           isReasonable(b.top);
}

bool isReasonable(const PdfColor& c) {
    return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b);
}

// Strict UTF-8: no overlongs, no surrogates, nothing above U+10FFFF, no
// truncated sequences.
bool isValidUtf8(const std::string& s) {
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80U) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        unsigned min = 0;
        unsigned cp = 0;
        if (b0 >= 0xC2U && b0 <= 0xDFU) {
            extra = 1;
            min = 0x80U;
            cp = b0 & 0x1FU;
        } else if (b0 >= 0xE0U && b0 <= 0xEFU) {
            extra = 2;
            min = 0x800U;
            cp = b0 & 0x0FU;
        } else if (b0 >= 0xF0U && b0 <= 0xF4U) {
            extra = 3;
            min = 0x10000U;
            cp = b0 & 0x07U;
        } else {
            return false; // continuation byte as lead, C0/C1, F5..FF
        }
        if (i + extra >= n) {
            return false; // truncated
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto bk = static_cast<unsigned char>(s[i + k]);
            if ((bk & 0xC0U) != 0x80U) {
                return false;
            }
            cp = (cp << 6U) | (bk & 0x3FU);
        }
        if (cp < min || cp > 0x10FFFFU || (cp >= 0xD800U && cp <= 0xDFFFU)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

PdfBox normalizedBox(PdfBox b) {
    if (b.left > b.right) {
        std::swap(b.left, b.right);
    }
    if (b.bottom > b.top) {
        std::swap(b.bottom, b.top);
    }
    return b;
}

struct BoxAccumulator {
    bool any = false;
    PdfBox box;
    void add(const PdfPoint& p) {
        if (!any) {
            box = PdfBox{p.x, p.y, p.x, p.y};
            any = true;
            return;
        }
        box.left = std::min(box.left, p.x);
        box.right = std::max(box.right, p.x);
        box.bottom = std::min(box.bottom, p.y);
        box.top = std::max(box.top, p.y);
    }
    void inflate(double d) {
        box.left -= d;
        box.bottom -= d;
        box.right += d;
        box.top += d;
    }
};

struct ArrowHead {
    PdfPoint wing1;
    PdfPoint wing2;
};

// d = unit(end - start); L = max(6, 3 * borderWidth);
// wings = end - L * rotate(d, +-30 degrees).
ArrowHead arrowHead(const PdfPoint& start, const PdfPoint& end, double borderWidth) {
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
    const double headLength = std::max(6.0, 3.0 * borderWidth);
    const double c = std::cos(kPi / 6.0);
    const double s = std::sin(kPi / 6.0);
    const double r1x = dx * c - dy * s; // rotate +30
    const double r1y = dx * s + dy * c;
    const double r2x = dx * c + dy * s; // rotate -30
    const double r2y = -dx * s + dy * c;
    return ArrowHead{PdfPoint{end.x - headLength * r1x, end.y - headLength * r1y},
                     PdfPoint{end.x - headLength * r2x, end.y - headLength * r2y}};
}

PdfPathSegment moveTo(PdfPoint p) {
    PdfPathSegment s;
    s.op = PdfPathSegment::Op::MoveTo;
    s.p = p;
    return s;
}

PdfPathSegment lineTo(PdfPoint p) {
    PdfPathSegment s;
    s.op = PdfPathSegment::Op::LineTo;
    s.p = p;
    return s;
}

PdfPathSegment cubicTo(PdfPoint c1, PdfPoint c2, PdfPoint p) {
    PdfPathSegment s;
    s.op = PdfPathSegment::Op::CubicTo;
    s.c1 = c1;
    s.c2 = c2;
    s.p = p;
    return s;
}

PdfPathSegment closePath() {
    PdfPathSegment s;
    s.op = PdfPathSegment::Op::Close;
    return s;
}

PdfBox inset(const PdfBox& b, double d) {
    const double maxInset = std::max(0.0, std::min(b.width(), b.height()) / 2.0);
    const double v = std::clamp(d, 0.0, maxInset);
    return PdfBox{b.left + v, b.bottom + v, b.right - v, b.top - v};
}

std::vector<PdfPathSegment> roundedRect(const PdfBox& b, double radius) {
    const double r = std::clamp(radius, 0.0, std::max(0.0, std::min(b.width(), b.height()) / 2.0));
    const double k = kKappa * r;
    std::vector<PdfPathSegment> s;
    s.push_back(moveTo({b.left + r, b.bottom}));
    s.push_back(lineTo({b.right - r, b.bottom}));
    s.push_back(cubicTo({b.right - r + k, b.bottom}, {b.right, b.bottom + r - k}, {b.right, b.bottom + r}));
    s.push_back(lineTo({b.right, b.top - r}));
    s.push_back(cubicTo({b.right, b.top - r + k}, {b.right - r + k, b.top}, {b.right - r, b.top}));
    s.push_back(lineTo({b.left + r, b.top}));
    s.push_back(cubicTo({b.left + r - k, b.top}, {b.left, b.top - r + k}, {b.left, b.top - r}));
    s.push_back(lineTo({b.left, b.bottom + r}));
    s.push_back(cubicTo({b.left, b.bottom + r - k}, {b.left + r - k, b.bottom}, {b.left + r, b.bottom}));
    s.push_back(closePath());
    return s;
}

std::vector<PdfPathSegment> ellipse(const PdfBox& b) {
    const double cx = (b.left + b.right) / 2.0;
    const double cy = (b.bottom + b.top) / 2.0;
    const double rx = b.width() / 2.0;
    const double ry = b.height() / 2.0;
    const double kx = kKappa * rx;
    const double ky = kKappa * ry;
    std::vector<PdfPathSegment> s;
    s.push_back(moveTo({cx + rx, cy}));
    s.push_back(cubicTo({cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry}));
    s.push_back(cubicTo({cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy}));
    s.push_back(cubicTo({cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry}));
    s.push_back(cubicTo({cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy}));
    s.push_back(closePath());
    return s;
}

PdfColor darkened(const PdfColor& c) {
    return PdfColor{c.r * 0.55F, c.g * 0.55F, c.b * 0.55F};
}

PdfAppearancePath strokePath(std::vector<PdfPathSegment> segments, const PdfColor& color, double width,
                             bool round) {
    PdfAppearancePath p;
    p.segments = std::move(segments);
    p.stroke = color;
    p.strokeWidth = static_cast<float>(width);
    p.roundJoins = round;
    return p;
}

void appendNumber(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += '0';
        return;
    }
    std::array<char, 64> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::fixed, 4);
    if (res.ec != std::errc()) {
        out += '0';
        return;
    }
    std::string s(buf.data(), res.ptr);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') {
            s.pop_back();
        }
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
    }
    if (s == "-0" || s.empty()) {
        s = "0";
    }
    out += s;
}

void appendColor(std::string& out, const PdfColor& c, const char* op) {
    appendNumber(out, static_cast<double>(c.r));
    out += ' ';
    appendNumber(out, static_cast<double>(c.g));
    out += ' ';
    appendNumber(out, static_cast<double>(c.b));
    out += ' ';
    out += op;
    out += '\n';
}

void appendPoint(std::string& out, const PdfPoint& p) {
    appendNumber(out, p.x);
    out += ' ';
    appendNumber(out, p.y);
}

} // namespace

const char* stampNameText(PdfStampName name) {
    switch (name) {
    case PdfStampName::Approved: return "Approved";
    case PdfStampName::Draft: return "Draft";
    case PdfStampName::Confidential: return "Confidential";
    case PdfStampName::Final: return "Final";
    }
    return "Approved";
}

std::optional<PdfStampName> parseStampName(std::string_view text) {
    for (const PdfStampName n : {PdfStampName::Approved, PdfStampName::Draft, PdfStampName::Confidential,
                                 PdfStampName::Final}) {
        if (text == stampNameText(n)) {
            return n;
        }
    }
    return std::nullopt;
}

void normalizeAnnotation(PdfAnnotationData& data) {
    data.opacity = std::isnan(data.opacity) ? 1.0F : std::clamp(data.opacity, 0.05F, 1.0F);
    data.borderWidth = std::isnan(data.borderWidth) ? 1.0F : std::clamp(data.borderWidth, 0.25F, 50.0F);

    const long quarter = std::lround(static_cast<double>(data.rotation) / 90.0);
    data.rotation = static_cast<int>(((quarter % 4) + 4) % 4) * 90;

    const double half = static_cast<double>(data.borderWidth) / 2.0 + 1.0;
    BoxAccumulator acc;
    switch (data.kind) {
    case PdfAnnotationKind::Highlight:
    case PdfAnnotationKind::Underline:
    case PdfAnnotationKind::StrikeOut:
        for (const PdfQuad& q : data.quads) {
            acc.add(q.p1);
            acc.add(q.p2);
            acc.add(q.p3);
            acc.add(q.p4);
        }
        break;
    case PdfAnnotationKind::Ink:
        for (const auto& stroke : data.inkStrokes) {
            for (const PdfPoint& p : stroke) {
                acc.add(p);
            }
        }
        if (acc.any) {
            acc.inflate(half);
        }
        break;
    case PdfAnnotationKind::Line:
        acc.add(data.lineStart);
        acc.add(data.lineEnd);
        acc.inflate(half);
        break;
    case PdfAnnotationKind::Arrow: {
        const ArrowHead head = arrowHead(data.lineStart, data.lineEnd, static_cast<double>(data.borderWidth));
        acc.add(data.lineStart);
        acc.add(data.lineEnd);
        acc.add(head.wing1);
        acc.add(head.wing2);
        acc.inflate(half);
        break;
    }
    case PdfAnnotationKind::Square:
    case PdfAnnotationKind::Circle:
    case PdfAnnotationKind::Note:
    case PdfAnnotationKind::Stamp:
    case PdfAnnotationKind::Other:
        break;
    }
    data.rect = acc.any ? acc.box : normalizedBox(data.rect);
}

bool isWritableAnnotation(const PdfAnnotationData& data) {
    if (data.kind == PdfAnnotationKind::Other) {
        return false;
    }
    if (data.contents.size() > kMaxContentsBytes || data.author.size() > kMaxAuthorBytes ||
        data.name.size() > kMaxNameBytes) {
        return false;
    }
    if (!isValidUtf8(data.contents) || !isValidUtf8(data.author) || !isValidUtf8(data.name)) {
        return false;
    }
    if (!isReasonable(data.rect) || !isReasonable(data.color) ||
        (data.interiorColor && !isReasonable(*data.interiorColor))) {
        return false;
    }
    if (!(data.opacity >= 0.05F && data.opacity <= 1.0F) ||
        !(data.borderWidth >= 0.25F && data.borderWidth <= 50.0F)) {
        return false;
    }
    if (data.rotation != 0 && data.rotation != 90 && data.rotation != 180 && data.rotation != 270) {
        return false;
    }

    switch (data.kind) {
    case PdfAnnotationKind::Highlight:
    case PdfAnnotationKind::Underline:
    case PdfAnnotationKind::StrikeOut:
        if (data.quads.empty() || data.quads.size() > kMaxQuadsPerAnnotation) {
            return false;
        }
        for (const PdfQuad& q : data.quads) {
            if (!isReasonable(q.p1) || !isReasonable(q.p2) || !isReasonable(q.p3) || !isReasonable(q.p4)) {
                return false;
            }
        }
        return true;
    case PdfAnnotationKind::Ink: {
        if (data.inkStrokes.empty() || data.inkStrokes.size() > kMaxInkStrokes) {
            return false;
        }
        std::size_t total = 0;
        for (const auto& stroke : data.inkStrokes) {
            if (stroke.empty() || stroke.size() > kMaxInkPointsPerStroke) {
                return false;
            }
            total += stroke.size();
            if (total > kMaxInkPointsTotal) {
                return false;
            }
            for (const PdfPoint& p : stroke) {
                if (!isReasonable(p)) {
                    return false;
                }
            }
        }
        return true;
    }
    case PdfAnnotationKind::Line:
    case PdfAnnotationKind::Arrow:
        if (!isReasonable(data.lineStart) || !isReasonable(data.lineEnd)) {
            return false;
        }
        return std::hypot(data.lineEnd.x - data.lineStart.x, data.lineEnd.y - data.lineStart.y) >= 0.5;
    case PdfAnnotationKind::Square:
    case PdfAnnotationKind::Circle:
    case PdfAnnotationKind::Note:
    case PdfAnnotationKind::Stamp:
        return data.rect.width() >= 1.0 && data.rect.height() >= 1.0;
    case PdfAnnotationKind::Other:
        return false;
    }
    return false;
}

PdfAppearance buildAppearance(const PdfAnnotationData& data) {
    PdfAppearance ap;
    ap.opacity = data.opacity;
    const double bw = static_cast<double>(data.borderWidth);

    switch (data.kind) {
    case PdfAnnotationKind::Highlight:
        for (const PdfQuad& q : data.quads) {
            PdfAppearancePath p;
            p.segments = {moveTo(q.p1), lineTo(q.p2), lineTo(q.p4), lineTo(q.p3), closePath()};
            p.fill = data.color;
            ap.paths.push_back(std::move(p));
        }
        break;
    case PdfAnnotationKind::Underline:
    case PdfAnnotationKind::StrikeOut:
        for (const PdfQuad& q : data.quads) {
            double nx = q.p1.x - q.p3.x;
            double ny = q.p1.y - q.p3.y;
            const double h = std::hypot(nx, ny);
            if (h > 0.0) {
                nx /= h;
                ny /= h;
            } else {
                nx = 0.0;
                ny = 1.0;
            }
            const double t = std::max(0.5, h / 14.0);
            if (data.kind == PdfAnnotationKind::Underline) {
                ap.paths.push_back(strokePath({moveTo({q.p3.x + nx * t, q.p3.y + ny * t}),
                                               lineTo({q.p4.x + nx * t, q.p4.y + ny * t})},
                                              data.color, t, false));
            } else {
                ap.paths.push_back(
                    strokePath({moveTo({(q.p1.x + q.p3.x) / 2.0, (q.p1.y + q.p3.y) / 2.0}),
                                lineTo({(q.p2.x + q.p4.x) / 2.0, (q.p2.y + q.p4.y) / 2.0})},
                               data.color, t, false));
            }
        }
        break;
    case PdfAnnotationKind::Note: {
        const PdfBox& r = data.rect;
        const double w = r.width();
        const double h = r.height();
        const PdfColor dark = darkened(data.color);
        PdfAppearancePath body;
        body.segments = roundedRect(inset(r, 1.0), std::min(w, h) * 0.15);
        body.fill = data.color;
        body.stroke = dark;
        body.strokeWidth = 1.0F;
        ap.paths.push_back(std::move(body));
        const double lineWidth = std::max(0.75, h * 0.05);
        for (const double frac : {0.3, 0.5, 0.7}) {
            const double y = r.top - h * frac;
            ap.paths.push_back(strokePath({moveTo({r.left + w * 0.25, y}), lineTo({r.left + w * 0.75, y})},
                                          dark, lineWidth, false));
        }
        break;
    }
    case PdfAnnotationKind::Ink:
        for (const auto& stroke : data.inkStrokes) {
            if (stroke.empty()) {
                continue;
            }
            std::vector<PdfPathSegment> segs;
            segs.push_back(moveTo(stroke.front()));
            if (stroke.size() == 1) {
                segs.push_back(lineTo({stroke.front().x + 0.01, stroke.front().y}));
            } else {
                for (std::size_t i = 1; i < stroke.size(); ++i) {
                    segs.push_back(lineTo(stroke[i]));
                }
            }
            ap.paths.push_back(strokePath(std::move(segs), data.color, bw, true));
        }
        break;
    case PdfAnnotationKind::Square:
    case PdfAnnotationKind::Circle: {
        const PdfBox b = inset(data.rect, bw / 2.0);
        PdfAppearancePath p;
        if (data.kind == PdfAnnotationKind::Square) {
            p.segments = {moveTo({b.left, b.bottom}), lineTo({b.right, b.bottom}), lineTo({b.right, b.top}),
                          lineTo({b.left, b.top}), closePath()};
        } else {
            p.segments = ellipse(b);
        }
        p.stroke = data.color;
        p.strokeWidth = data.borderWidth;
        p.fill = data.interiorColor;
        ap.paths.push_back(std::move(p));
        break;
    }
    case PdfAnnotationKind::Line:
    case PdfAnnotationKind::Arrow:
        ap.paths.push_back(
            strokePath({moveTo(data.lineStart), lineTo(data.lineEnd)}, data.color, bw, true));
        if (data.kind == PdfAnnotationKind::Arrow) {
            const ArrowHead head = arrowHead(data.lineStart, data.lineEnd, bw);
            ap.paths.push_back(strokePath({moveTo(head.wing1), lineTo(data.lineEnd), lineTo(head.wing2)},
                                          data.color, bw, true));
        }
        break;
    case PdfAnnotationKind::Stamp: {
        const PdfBox& r = data.rect;
        const double m = std::min(r.width(), r.height());
        const double fw = std::max(1.5, m * 0.06);
        ap.paths.push_back(strokePath(roundedRect(inset(r, fw / 2.0), m * 0.12), data.color, fw, false));
        PdfAppearanceText text;
        const std::string name = stampNameText(data.stampName);
        text.text.reserve(name.size());
        for (const char ch : name) {
            text.text += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        text.box = inset(r, fw * 2.0);
        text.rotation = data.rotation;
        text.color = data.color;
        text.bold = true;
        ap.texts.push_back(std::move(text));
        break;
    }
    case PdfAnnotationKind::Other:
        break;
    }
    return ap;
}

std::string appearanceContentStream(const PdfAppearance& appearance) {
    std::string out;
    if (appearance.opacity < 1.0F) {
        out += "/GS gs\n";
    }
    for (const PdfAppearancePath& path : appearance.paths) {
        out += "q\n";
        appendNumber(out, static_cast<double>(path.strokeWidth));
        out += " w\n";
        out += path.roundJoins ? "1 J 1 j\n" : "0 J 0 j\n";
        if (path.stroke) {
            appendColor(out, *path.stroke, "RG");
        }
        if (path.fill) {
            appendColor(out, *path.fill, "rg");
        }
        for (const PdfPathSegment& seg : path.segments) {
            switch (seg.op) {
            case PdfPathSegment::Op::MoveTo:
                appendPoint(out, seg.p);
                out += " m\n";
                break;
            case PdfPathSegment::Op::LineTo:
                appendPoint(out, seg.p);
                out += " l\n";
                break;
            case PdfPathSegment::Op::CubicTo:
                appendPoint(out, seg.c1);
                out += ' ';
                appendPoint(out, seg.c2);
                out += ' ';
                appendPoint(out, seg.p);
                out += " c\n";
                break;
            case PdfPathSegment::Op::Close:
                out += "h\n";
                break;
            }
        }
        if (path.fill && path.stroke) {
            out += "B\n";
        } else if (path.fill) {
            out += "f\n";
        } else if (path.stroke) {
            out += "S\n";
        } else {
            out += "n\n";
        }
        out += "Q\n";
    }
    return out;
}

} // namespace rivet::pdf
