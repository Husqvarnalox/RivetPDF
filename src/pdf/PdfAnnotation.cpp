// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfAnnotation.hpp"

#include "pdf/BundledFontMetrics.hpp"
#include "pdf/BundledFontOutlines.hpp"
#include "pdf/PdfTextLayout.hpp"

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

// --- Signatures ----------------------------------------------------------------

constexpr PdfBundledFont kSignatureFont = PdfBundledFont::SerifRegular; // Tinos Regular

struct TextExtent {
    double advanceEm = 0.0; // sum of advances in ems
    double ascentEm = 0.0;
    double descentEm = 0.0; // negative (below the baseline)
    bool anyGlyph = false;
};

TextExtent measureSignatureText(std::string_view utf8) {
    const BundledFontMetrics& metrics = bundledFontMetrics(kSignatureFont);
    TextExtent extent;
    if (!metrics.valid()) return extent;
    const double upm = metrics.unitsPerEm();
    for (const char32_t cp : decodeUtf8(utf8)) {
        extent.advanceEm += metrics.advance(cp) / upm;
        if (metrics.hasGlyph(cp) && cp != U' ') extent.anyGlyph = true;
    }
    extent.ascentEm = metrics.ascent() / upm;
    extent.descentEm = metrics.descent() / upm;
    return extent;
}

double clampAspect(double aspect) {
    if (!std::isfinite(aspect) || !(aspect > 0.0)) return 1.0;
    return std::clamp(aspect, kMinSignatureAspect, kMaxSignatureAspect);
}

// Maps content coordinates (x in [0, cw], y in [0, ch], y-up) of a signature
// fitted into `box` and turned `rotation` clockwise quarter turns to user space.
class SignatureFrame {
public:
    SignatureFrame(const PdfBox& box, int rotation) {
        const double theta = static_cast<double>(rotation) * kPi / 180.0;
        cos_ = std::round(std::cos(theta));
        sin_ = std::round(std::sin(theta)); // exact for quarter turns
        const bool quarter = rotation == 90 || rotation == 270;
        width_ = quarter ? box.height() : box.width();
        height_ = quarter ? box.width() : box.height();
        cx_ = (box.left + box.right) / 2.0;
        cy_ = (box.bottom + box.top) / 2.0;
    }
    double width() const { return width_; }
    double height() const { return height_; }
    // Clockwise by theta in y-up space: x' = x cos + y sin, y' = -x sin + y cos.
    PdfPoint toUser(double x, double y) const {
        const double lx = x - width_ / 2.0;
        const double ly = y - height_ / 2.0;
        return PdfPoint{cx_ + lx * cos_ + ly * sin_, cy_ - lx * sin_ + ly * cos_};
    }

private:
    double cos_ = 1.0;
    double sin_ = 0.0;
    double width_ = 0.0;
    double height_ = 0.0;
    double cx_ = 0.0;
    double cy_ = 0.0;
};

void appendSignature(PdfAppearance& ap, const PdfAnnotationData& data) {
    if (data.signature == nullptr) return;
    const PdfSignatureAppearance& sig = *data.signature;
    const PdfBox box = fitSignatureBox(signatureAspect(sig), data.rect, data.rotation);
    const SignatureFrame frame(box, data.rotation);
    switch (sig.source) {
    case PdfSignatureSource::Ink: {
        const double aspect = std::max(1e-9, sig.aspect);
        const double sx = frame.width() / aspect;
        const double sy = frame.height();
        const double width = std::max(0.25, static_cast<double>(sig.inkWidth) * frame.height());
        for (const auto& stroke : sig.strokes) {
            if (stroke.empty()) continue;
            std::vector<PdfPathSegment> segs;
            segs.reserve(stroke.size() + 1);
            segs.push_back(moveTo(frame.toUser(stroke.front().x * sx, stroke.front().y * sy)));
            if (stroke.size() == 1) {
                segs.push_back(lineTo(frame.toUser(stroke.front().x * sx + 0.01, stroke.front().y * sy)));
            } else {
                for (std::size_t i = 1; i < stroke.size(); ++i) {
                    segs.push_back(lineTo(frame.toUser(stroke[i].x * sx, stroke[i].y * sy)));
                }
            }
            ap.paths.push_back(strokePath(std::move(segs), data.color, width, true));
        }
        break;
    }
    case PdfSignatureSource::Text: {
        const TextExtent extent = measureSignatureText(sig.text);
        const double size = signatureFontSize(sig.text, frame.width(), frame.height());
        if (!(size > 0.0) || !extent.anyGlyph) break;
        const BundledFontMetrics& metrics = bundledFontMetrics(kSignatureFont);
        const double upm = metrics.unitsPerEm();
        const double unit = size / upm;
        const double lineHeight = (extent.ascentEm - extent.descentEm) * size;
        double penX = (frame.width() - extent.advanceEm * size) / 2.0;
        const double baseline = (frame.height() - lineHeight) / 2.0 - extent.descentEm * size;
        PdfAppearancePath glyphs;
        glyphs.fill = data.color;
        for (const char32_t cp : decodeUtf8(sig.text)) {
            for (PdfPathSegment seg : bundledGlyphOutline(kSignatureFont, cp)) {
                const auto map = [&](const PdfPoint& p) {
                    return frame.toUser(penX + p.x * unit, baseline + p.y * unit);
                };
                seg.p = map(seg.p);
                seg.c1 = map(seg.c1);
                seg.c2 = map(seg.c2);
                glyphs.segments.push_back(seg);
            }
            penX += metrics.advance(cp) * unit;
        }
        if (!glyphs.segments.empty()) ap.paths.push_back(std::move(glyphs));
        break;
    }
    case PdfSignatureSource::Image:
        if (sig.image != nullptr && sig.image->isValid()) {
            ap.images.push_back(PdfAppearanceImage{sig.image, box, data.rotation});
        }
        break;
    }
}

} // namespace

std::optional<PdfSignatureAppearance> makeInkSignature(const std::vector<std::vector<PdfPoint>>& padStrokes,
                                                       double penWidth) {
    if (padStrokes.size() > kMaxSignatureStrokes) return std::nullopt;
    const double pen = std::isfinite(penWidth) && penWidth > 0.0 ? penWidth : 2.0;
    BoxAccumulator acc;
    std::size_t total = 0;
    for (const auto& stroke : padStrokes) {
        total += stroke.size();
        if (total > kMaxSignaturePoints) return std::nullopt;
        for (const PdfPoint& p : stroke) {
            if (!isReasonable(p)) return std::nullopt;
            acc.add(p);
        }
    }
    if (!acc.any) return std::nullopt;
    acc.inflate(pen / 2.0);
    PdfBox b = acc.box; // y-down pad space: bottom < top numerically = visual top < visual bottom
    double w = b.width();
    double h = b.height();
    if (w / h > kMaxSignatureAspect) {
        const double grow = (w / kMaxSignatureAspect - h) / 2.0;
        b.bottom -= grow;
        b.top += grow;
        h = b.height();
    } else if (w / h < kMinSignatureAspect) {
        const double grow = (h * kMinSignatureAspect - w) / 2.0;
        b.left -= grow;
        b.right += grow;
        w = b.width();
    }
    PdfSignatureAppearance out;
    out.source = PdfSignatureSource::Ink;
    out.aspect = w / h;
    out.inkWidth = static_cast<float>(std::clamp(pen / h, 0.005, 0.25));
    const double scale = 1.0 / h;
    out.strokes.reserve(padStrokes.size());
    for (const auto& stroke : padStrokes) {
        if (stroke.empty()) continue;
        std::vector<PdfPoint> mapped;
        mapped.reserve(stroke.size());
        // y-down -> y-up: the visual top (smallest y) becomes 1.
        for (const PdfPoint& p : stroke) mapped.push_back(PdfPoint{(p.x - b.left) * scale, (b.top - p.y) * scale});
        out.strokes.push_back(std::move(mapped));
    }
    return out;
}

double signatureAspect(const PdfSignatureAppearance& signature) {
    switch (signature.source) {
    case PdfSignatureSource::Ink:
        return clampAspect(signature.aspect);
    case PdfSignatureSource::Text: {
        const TextExtent extent = measureSignatureText(signature.text);
        const double height = extent.ascentEm - extent.descentEm;
        if (!(extent.advanceEm > 0.0) || !(height > 0.0)) return 1.0;
        return clampAspect(extent.advanceEm / height);
    }
    case PdfSignatureSource::Image:
        if (signature.image == nullptr || signature.image->height() == 0) return 1.0;
        return clampAspect(static_cast<double>(signature.image->width()) /
                           static_cast<double>(signature.image->height()));
    }
    return 1.0;
}

bool isValidSignature(const PdfSignatureAppearance& signature) {
    switch (signature.source) {
    case PdfSignatureSource::Ink: {
        if (signature.strokes.empty() || signature.strokes.size() > kMaxSignatureStrokes) return false;
        if (!std::isfinite(signature.aspect) || signature.aspect < kMinSignatureAspect - 1e-9 ||
            signature.aspect > kMaxSignatureAspect + 1e-9) {
            return false;
        }
        if (!(signature.inkWidth > 0.0F && signature.inkWidth <= 0.5F)) return false;
        std::size_t total = 0;
        for (const auto& stroke : signature.strokes) {
            if (stroke.empty()) return false;
            total += stroke.size();
            if (total > kMaxSignaturePoints) return false;
            for (const PdfPoint& p : stroke) {
                if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::fabs(p.x) > 100.0 || std::fabs(p.y) > 100.0) {
                    return false;
                }
            }
        }
        return true;
    }
    case PdfSignatureSource::Text:
        return !signature.text.empty() && signature.text.size() <= kMaxSignatureTextBytes &&
               isValidUtf8(signature.text) && signature.text.find('\n') == std::string::npos &&
               measureSignatureText(signature.text).anyGlyph;
    case PdfSignatureSource::Image:
        return signature.image != nullptr && signature.image->isValid() && signature.image->width() > 0 &&
               signature.image->height() > 0 && signature.image->width() <= kMaxSignatureImageSide &&
               signature.image->height() <= kMaxSignatureImageSide;
    }
    return false;
}

PdfBox fitSignatureBox(double aspect, const PdfBox& available, int rotation) {
    const PdfBox a = normalizedBox(available);
    const double ratio = clampAspect(aspect);
    const bool quarter = rotation == 90 || rotation == 270;
    const double availW = quarter ? a.height() : a.width();
    const double availH = quarter ? a.width() : a.height();
    double cw = availW;
    double ch = availH;
    if (availH > 0.0 && availW / availH > ratio) {
        cw = availH * ratio;
    } else {
        ch = availW / ratio;
    }
    const double uw = quarter ? ch : cw;
    const double uh = quarter ? cw : ch;
    const double cx = (a.left + a.right) / 2.0;
    const double cy = (a.bottom + a.top) / 2.0;
    return PdfBox{cx - uw / 2.0, cy - uh / 2.0, cx + uw / 2.0, cy + uh / 2.0};
}

double signatureFontSize(std::string_view text, double width, double height) {
    const TextExtent extent = measureSignatureText(text);
    const double lineEm = extent.ascentEm - extent.descentEm;
    if (!(extent.advanceEm > 0.0) || !(lineEm > 0.0) || !(width > 0.0) || !(height > 0.0)) return 0.0;
    return std::min(width / extent.advanceEm, height / lineEm);
}

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
    case PdfAnnotationKind::Signature:
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
    case PdfAnnotationKind::Signature:
        return data.rect.width() >= 1.0 && data.rect.height() >= 1.0 && data.signature != nullptr &&
               isValidSignature(*data.signature);
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
    case PdfAnnotationKind::Signature:
        appendSignature(ap, data);
        break;
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
