// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "pdf/PdfAnnotation.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

// Engine-independent annotation normalization, validation and appearance
// building (pdf/PdfAnnotation.hpp): runs in both build modes.

namespace {

using namespace rivet::pdf;

constexpr double kEps = 1e-9;

bool nearBox(const PdfBox& a, const PdfBox& b, double eps = 1e-6) {
    return std::fabs(a.left - b.left) <= eps && std::fabs(a.bottom - b.bottom) <= eps &&
           std::fabs(a.right - b.right) <= eps && std::fabs(a.top - b.top) <= eps;
}

PdfQuad quad(double l, double b, double r, double t) {
    return PdfQuad{{l, t}, {r, t}, {l, b}, {r, b}};
}

PdfAnnotationData markup(PdfAnnotationKind kind) {
    PdfAnnotationData d;
    d.kind = kind;
    d.quads.push_back(quad(10.0, 20.0, 110.0, 34.0));
    return d;
}

PdfAnnotationData inkData() {
    PdfAnnotationData d;
    d.kind = PdfAnnotationKind::Ink;
    d.inkStrokes = {{{10.0, 10.0}, {20.0, 30.0}, {40.0, 15.0}}};
    return d;
}

PdfAnnotationData lineData(PdfAnnotationKind kind) {
    PdfAnnotationData d;
    d.kind = kind;
    d.lineStart = {0.0, 0.0};
    d.lineEnd = {100.0, 0.0};
    return d;
}

PdfAnnotationData shapeData(PdfAnnotationKind kind) {
    PdfAnnotationData d;
    d.kind = kind;
    d.rect = PdfBox{10.0, 20.0, 110.0, 80.0};
    return d;
}

bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace

RIVET_TEST(annotationStampNameRoundTrip) {
    for (const PdfStampName n :
         {PdfStampName::Approved, PdfStampName::Draft, PdfStampName::Confidential, PdfStampName::Final}) {
        const auto parsed = parseStampName(stampNameText(n));
        CHECK(parsed.has_value());
        CHECK(*parsed == n);
    }
    CHECK_EQ(std::string(stampNameText(PdfStampName::Approved)), std::string("Approved"));
    CHECK_EQ(std::string(stampNameText(PdfStampName::Draft)), std::string("Draft"));
    CHECK_EQ(std::string(stampNameText(PdfStampName::Confidential)), std::string("Confidential"));
    CHECK_EQ(std::string(stampNameText(PdfStampName::Final)), std::string("Final"));
    CHECK(!parseStampName("approved").has_value());
    CHECK(!parseStampName("APPROVED").has_value());
    CHECK(!parseStampName("").has_value());
    CHECK(!parseStampName("Approved ").has_value());
    CHECK(!parseStampName("Other").has_value());
}

RIVET_TEST(annotationNormalizeClampsAndRotation) {
    PdfAnnotationData d = shapeData(PdfAnnotationKind::Square);
    d.opacity = 0.0F;
    d.borderWidth = 0.0F;
    normalizeAnnotation(d);
    CHECK_NEAR(static_cast<double>(d.opacity), 0.05, 1e-6);
    CHECK_NEAR(static_cast<double>(d.borderWidth), 0.25, 1e-6);

    d.opacity = 7.0F;
    d.borderWidth = 500.0F;
    normalizeAnnotation(d);
    CHECK_NEAR(static_cast<double>(d.opacity), 1.0, 1e-6);
    CHECK_NEAR(static_cast<double>(d.borderWidth), 50.0, 1e-6);

    d.opacity = std::numeric_limits<float>::quiet_NaN();
    d.borderWidth = std::numeric_limits<float>::quiet_NaN();
    normalizeAnnotation(d);
    CHECK_NEAR(static_cast<double>(d.opacity), 1.0, 1e-6);
    CHECK_NEAR(static_cast<double>(d.borderWidth), 1.0, 1e-6);

    const std::pair<int, int> cases[] = {{-90, 270}, {44, 0},  {46, 90},  {0, 0},    {90, 90},
                                         {180, 180}, {270, 270}, {360, 0}, {450, 90}, {-10, 0},
                                         {-100, 270}, {350, 0}};
    for (const auto& [in, out] : cases) {
        PdfAnnotationData r = shapeData(PdfAnnotationKind::Stamp);
        r.rotation = in;
        normalizeAnnotation(r);
        CHECK_EQ(r.rotation, out);
    }
}

RIVET_TEST(annotationNormalizeMarkupRect) {
    for (const PdfAnnotationKind kind :
         {PdfAnnotationKind::Highlight, PdfAnnotationKind::Underline, PdfAnnotationKind::StrikeOut}) {
        PdfAnnotationData d = markup(kind);
        d.quads.push_back(quad(50.0, 40.0, 200.0, 54.0));
        d.rect = PdfBox{};
        normalizeAnnotation(d);
        CHECK(nearBox(d.rect, PdfBox{10.0, 20.0, 200.0, 54.0}));
    }
}

RIVET_TEST(annotationNormalizeInkRect) {
    PdfAnnotationData d = inkData();
    d.borderWidth = 4.0F;
    normalizeAnnotation(d);
    // bbox [10,10,40,30] inflated by 4/2 + 1 = 3.
    CHECK(nearBox(d.rect, PdfBox{7.0, 7.0, 43.0, 33.0}));
}

RIVET_TEST(annotationNormalizeLineAndArrowRect) {
    PdfAnnotationData line = lineData(PdfAnnotationKind::Line);
    normalizeAnnotation(line);
    CHECK(nearBox(line.rect, PdfBox{-1.5, -1.5, 101.5, 1.5}));

    PdfAnnotationData arrow = lineData(PdfAnnotationKind::Arrow);
    normalizeAnnotation(arrow);
    // Head length 6, wings at 30 degrees: y = +-3; x = 100 - 6*cos(30).
    CHECK(nearBox(arrow.rect, PdfBox{-1.5, -4.5, 101.5, 4.5}));

    // A vertical arrow's wings widen the box in x.
    PdfAnnotationData up = lineData(PdfAnnotationKind::Arrow);
    up.lineEnd = {0.0, 100.0};
    up.borderWidth = 10.0F; // head length 30, wings at x = +-15
    normalizeAnnotation(up);
    CHECK(nearBox(up.rect, PdfBox{-21.0, -6.0, 21.0, 106.0}));
}

RIVET_TEST(annotationNormalizeRectShapes) {
    for (const PdfAnnotationKind kind : {PdfAnnotationKind::Square, PdfAnnotationKind::Circle,
                                         PdfAnnotationKind::Note, PdfAnnotationKind::Stamp}) {
        PdfAnnotationData d = shapeData(kind);
        d.rect = PdfBox{110.0, 80.0, 10.0, 20.0};
        normalizeAnnotation(d);
        CHECK(d.rect == (PdfBox{10.0, 20.0, 110.0, 80.0}));
    }
    // Empty derived geometry leaves the (normalized) rect alone.
    PdfAnnotationData empty;
    empty.kind = PdfAnnotationKind::Highlight;
    empty.rect = PdfBox{5.0, 5.0, 1.0, 1.0};
    normalizeAnnotation(empty);
    CHECK(empty.rect == (PdfBox{1.0, 1.0, 5.0, 5.0}));
}

RIVET_TEST(annotationWritableAcceptsValid) {
    CHECK(isWritableAnnotation(markup(PdfAnnotationKind::Highlight)));
    CHECK(isWritableAnnotation(markup(PdfAnnotationKind::Underline)));
    CHECK(isWritableAnnotation(markup(PdfAnnotationKind::StrikeOut)));
    CHECK(isWritableAnnotation(inkData()));
    CHECK(isWritableAnnotation(lineData(PdfAnnotationKind::Line)));
    CHECK(isWritableAnnotation(lineData(PdfAnnotationKind::Arrow)));
    CHECK(isWritableAnnotation(shapeData(PdfAnnotationKind::Square)));
    CHECK(isWritableAnnotation(shapeData(PdfAnnotationKind::Circle)));
    CHECK(isWritableAnnotation(shapeData(PdfAnnotationKind::Note)));
    PdfAnnotationData stamp = shapeData(PdfAnnotationKind::Stamp);
    stamp.rotation = 270;
    CHECK(isWritableAnnotation(stamp));
}

RIVET_TEST(annotationWritableRejectsOtherAndBadScalars) {
    PdfAnnotationData other = shapeData(PdfAnnotationKind::Other);
    CHECK(!isWritableAnnotation(other));

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    PdfAnnotationData d = shapeData(PdfAnnotationKind::Square);
    d.rect.left = nan;
    CHECK(!isWritableAnnotation(d));
    d = shapeData(PdfAnnotationKind::Square);
    d.rect.right = inf;
    CHECK(!isWritableAnnotation(d));
    d = shapeData(PdfAnnotationKind::Square);
    d.rect.right = 2.0e6;
    CHECK(!isWritableAnnotation(d));

    d = shapeData(PdfAnnotationKind::Square);
    d.opacity = 0.04F;
    CHECK(!isWritableAnnotation(d));
    d.opacity = 1.01F;
    CHECK(!isWritableAnnotation(d));
    d.opacity = std::numeric_limits<float>::quiet_NaN();
    CHECK(!isWritableAnnotation(d));
    d = shapeData(PdfAnnotationKind::Square);
    d.borderWidth = 0.2F;
    CHECK(!isWritableAnnotation(d));
    d.borderWidth = 51.0F;
    CHECK(!isWritableAnnotation(d));
    d = shapeData(PdfAnnotationKind::Square);
    d.rotation = 45;
    CHECK(!isWritableAnnotation(d));
    d.rotation = -90;
    CHECK(!isWritableAnnotation(d));

    d = shapeData(PdfAnnotationKind::Square);
    d.rect = PdfBox{0.0, 0.0, 0.9, 50.0};
    CHECK(!isWritableAnnotation(d));
    d.rect = PdfBox{0.0, 0.0, 50.0, 0.9};
    CHECK(!isWritableAnnotation(d));
    d.rect = PdfBox{0.0, 0.0, 1.0, 1.0};
    CHECK(isWritableAnnotation(d));
}

RIVET_TEST(annotationWritableStringLimitsAndUtf8) {
    PdfAnnotationData d = shapeData(PdfAnnotationKind::Note);
    d.contents.assign(kMaxContentsBytes, 'a');
    CHECK(isWritableAnnotation(d));
    d.contents.push_back('a');
    CHECK(!isWritableAnnotation(d));

    d = shapeData(PdfAnnotationKind::Note);
    d.author.assign(kMaxAuthorBytes, 'a');
    CHECK(isWritableAnnotation(d));
    d.author.push_back('a');
    CHECK(!isWritableAnnotation(d));

    d = shapeData(PdfAnnotationKind::Note);
    d.name.assign(256, 'a');
    CHECK(isWritableAnnotation(d));
    d.name.push_back('a');
    CHECK(!isWritableAnnotation(d));

    // Valid UTF-8: Cyrillic, 3-byte, 4-byte.
    d = shapeData(PdfAnnotationKind::Note);
    d.contents = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";
    d.author = "\xE2\x82\xAC \xF0\x9F\x98\x80";
    CHECK(isWritableAnnotation(d));

    const char* const bad[] = {
        "\xC0\xAF",         // overlong
        "\xE0\x80\xAF",     // overlong 3-byte
        "\xED\xA0\x80",     // surrogate
        "\xF4\x90\x80\x80", // > U+10FFFF
        "\xF5\x80\x80\x80", // invalid lead
        "\xD0",             // truncated
        "\xE2\x82",         // truncated
        "\x80",             // stray continuation
        "a\xD0",            // truncated at end
        "\xD0\x20",         // bad continuation
    };
    for (const char* text : bad) {
        PdfAnnotationData a = shapeData(PdfAnnotationKind::Note);
        a.contents = text;
        CHECK(!isWritableAnnotation(a));
        PdfAnnotationData b = shapeData(PdfAnnotationKind::Note);
        b.author = text;
        CHECK(!isWritableAnnotation(b));
        PdfAnnotationData c = shapeData(PdfAnnotationKind::Note);
        c.name = text;
        CHECK(!isWritableAnnotation(c));
    }
}

RIVET_TEST(annotationWritableGeometryLimits) {
    // Markup quads.
    PdfAnnotationData m = markup(PdfAnnotationKind::Highlight);
    m.quads.clear();
    CHECK(!isWritableAnnotation(m));
    m.quads.assign(kMaxQuadsPerAnnotation, quad(0.0, 0.0, 10.0, 10.0));
    CHECK(isWritableAnnotation(m));
    m.quads.push_back(quad(0.0, 0.0, 10.0, 10.0));
    CHECK(!isWritableAnnotation(m));
    m = markup(PdfAnnotationKind::Underline);
    m.quads[0].p3.y = std::numeric_limits<double>::quiet_NaN();
    CHECK(!isWritableAnnotation(m));

    // Ink.
    PdfAnnotationData ink = inkData();
    ink.inkStrokes.clear();
    CHECK(!isWritableAnnotation(ink));
    ink.inkStrokes.assign(kMaxInkStrokes, std::vector<PdfPoint>{{1.0, 1.0}});
    CHECK(isWritableAnnotation(ink));
    ink.inkStrokes.emplace_back(std::vector<PdfPoint>{{1.0, 1.0}});
    CHECK(!isWritableAnnotation(ink));

    ink = inkData();
    ink.inkStrokes.emplace_back(); // empty stroke
    CHECK(!isWritableAnnotation(ink));

    ink = inkData();
    ink.inkStrokes = {std::vector<PdfPoint>(kMaxInkPointsPerStroke, PdfPoint{1.0, 2.0})};
    CHECK(isWritableAnnotation(ink));
    ink.inkStrokes[0].push_back({1.0, 2.0});
    CHECK(!isWritableAnnotation(ink));

    // Total: 5 strokes of 10000 = 50000 ok; one more point is not.
    ink = inkData();
    ink.inkStrokes.assign(5, std::vector<PdfPoint>(kMaxInkPointsPerStroke, PdfPoint{1.0, 2.0}));
    CHECK(isWritableAnnotation(ink));
    ink.inkStrokes.emplace_back(std::vector<PdfPoint>{{3.0, 3.0}});
    CHECK(!isWritableAnnotation(ink));

    ink = inkData();
    ink.inkStrokes[0][1].x = std::numeric_limits<double>::infinity();
    CHECK(!isWritableAnnotation(ink));

    // Line / arrow.
    for (const PdfAnnotationKind kind : {PdfAnnotationKind::Line, PdfAnnotationKind::Arrow}) {
        PdfAnnotationData l = lineData(kind);
        l.lineEnd = {0.4, 0.0};
        CHECK(!isWritableAnnotation(l));
        l.lineEnd = {0.5, 0.0};
        CHECK(isWritableAnnotation(l));
        l.lineEnd = {std::numeric_limits<double>::quiet_NaN(), 0.0};
        CHECK(!isWritableAnnotation(l));
    }
}

RIVET_TEST(annotationAppearanceMarkup) {
    PdfAnnotationData h = markup(PdfAnnotationKind::Highlight);
    h.quads.push_back(quad(10.0, 40.0, 110.0, 54.0));
    h.opacity = 0.5F;
    h.color = PdfColor{1.0F, 1.0F, 0.0F};
    PdfAppearance ap = buildAppearance(h);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(2));
    CHECK_NEAR(static_cast<double>(ap.opacity), 0.5, 1e-6);
    CHECK(ap.texts.empty());
    for (const auto& p : ap.paths) {
        CHECK(p.fill.has_value());
        CHECK(!p.stroke.has_value());
        CHECK_EQ(p.segments.size(), static_cast<std::size_t>(5));
        CHECK(p.segments.back().op == PdfPathSegment::Op::Close);
    }
    // MoveTo p1, LineTo p2, LineTo p4, LineTo p3.
    const PdfQuad q = h.quads[0];
    CHECK(ap.paths[0].segments[0].p == q.p1);
    CHECK(ap.paths[0].segments[1].p == q.p2);
    CHECK(ap.paths[0].segments[2].p == q.p4);
    CHECK(ap.paths[0].segments[3].p == q.p3);

    // Underline: quad height 14 -> t = 1; bar at p3 + up*1.
    PdfAnnotationData u = markup(PdfAnnotationKind::Underline);
    ap = buildAppearance(u);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(1));
    CHECK(ap.paths[0].stroke.has_value());
    CHECK(!ap.paths[0].fill.has_value());
    CHECK_NEAR(static_cast<double>(ap.paths[0].strokeWidth), 1.0, 1e-6);
    CHECK_NEAR(ap.paths[0].segments[0].p.x, 10.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[0].p.y, 21.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[1].p.x, 110.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[1].p.y, 21.0, kEps);

    // Thin quad: minimum width 0.5.
    PdfAnnotationData thin = markup(PdfAnnotationKind::Underline);
    thin.quads[0] = quad(0.0, 0.0, 10.0, 2.0);
    ap = buildAppearance(thin);
    CHECK_NEAR(static_cast<double>(ap.paths[0].strokeWidth), 0.5, 1e-6);

    // StrikeOut: from the middle of the left edge to the middle of the right.
    PdfAnnotationData s = markup(PdfAnnotationKind::StrikeOut);
    ap = buildAppearance(s);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(1));
    CHECK_NEAR(ap.paths[0].segments[0].p.x, 10.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[0].p.y, 27.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[1].p.x, 110.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[1].p.y, 27.0, kEps);
}

RIVET_TEST(annotationAppearanceNote) {
    PdfAnnotationData n = shapeData(PdfAnnotationKind::Note);
    n.rect = PdfBox{100.0, 100.0, 120.0, 120.0};
    n.color = PdfColor{1.0F, 0.8F, 0.0F};
    const PdfAppearance ap = buildAppearance(n);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(4));
    CHECK(ap.paths[0].fill.has_value());
    CHECK(ap.paths[0].stroke.has_value());
    CHECK_NEAR(static_cast<double>(ap.paths[0].stroke->r), 0.55, 1e-5);
    CHECK_NEAR(static_cast<double>(ap.paths[0].stroke->g), 0.44, 1e-5);
    CHECK_NEAR(static_cast<double>(ap.paths[0].strokeWidth), 1.0, 1e-6);
    bool hasCubic = false;
    for (const auto& seg : ap.paths[0].segments) {
        hasCubic = hasCubic || seg.op == PdfPathSegment::Op::CubicTo;
    }
    CHECK(hasCubic);
    // Three horizontal lines at 30/50/70 % from the top, spanning 25..75 %.
    const double expectedY[] = {114.0, 110.0, 106.0};
    for (int i = 0; i < 3; ++i) {
        const auto& p = ap.paths[static_cast<std::size_t>(i) + 1];
        CHECK(p.stroke.has_value());
        CHECK(!p.fill.has_value());
        CHECK_EQ(p.segments.size(), static_cast<std::size_t>(2));
        CHECK_NEAR(p.segments[0].p.x, 105.0, kEps);
        CHECK_NEAR(p.segments[1].p.x, 115.0, kEps);
        CHECK_NEAR(p.segments[0].p.y, expectedY[i], kEps);
        CHECK_NEAR(p.segments[1].p.y, expectedY[i], kEps);
        CHECK_NEAR(static_cast<double>(p.strokeWidth), 1.0, 1e-6); // max(0.75, 20*0.05)
    }
}

RIVET_TEST(annotationAppearanceInk) {
    PdfAnnotationData ink = inkData();
    ink.borderWidth = 3.0F;
    ink.inkStrokes.push_back({{50.0, 50.0}});
    const PdfAppearance ap = buildAppearance(ink);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(2));
    CHECK_EQ(ap.paths[0].segments.size(), static_cast<std::size_t>(3));
    CHECK(ap.paths[0].segments[0].op == PdfPathSegment::Op::MoveTo);
    CHECK(ap.paths[0].segments[1].op == PdfPathSegment::Op::LineTo);
    CHECK(ap.paths[0].roundJoins);
    CHECK(ap.paths[0].stroke.has_value());
    CHECK(!ap.paths[0].fill.has_value());
    CHECK_NEAR(static_cast<double>(ap.paths[0].strokeWidth), 3.0, 1e-6);
    // Single-point stroke becomes a tiny segment.
    CHECK_EQ(ap.paths[1].segments.size(), static_cast<std::size_t>(2));
    CHECK_NEAR(ap.paths[1].segments[1].p.x, 50.01, kEps);
    CHECK_NEAR(ap.paths[1].segments[1].p.y, 50.0, kEps);
}

RIVET_TEST(annotationAppearanceSquareAndCircle) {
    PdfAnnotationData sq = shapeData(PdfAnnotationKind::Square);
    sq.borderWidth = 4.0F;
    PdfAppearance ap = buildAppearance(sq);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(1));
    CHECK(ap.paths[0].stroke.has_value());
    CHECK(!ap.paths[0].fill.has_value());
    CHECK_NEAR(static_cast<double>(ap.paths[0].strokeWidth), 4.0, 1e-6);
    CHECK_NEAR(ap.paths[0].segments[0].p.x, 12.0, kEps); // inset by 2
    CHECK_NEAR(ap.paths[0].segments[0].p.y, 22.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[2].p.x, 108.0, kEps);
    CHECK_NEAR(ap.paths[0].segments[2].p.y, 78.0, kEps);

    sq.interiorColor = PdfColor{0.0F, 1.0F, 0.0F};
    ap = buildAppearance(sq);
    CHECK(ap.paths[0].fill.has_value());
    CHECK(ap.paths[0].stroke.has_value());

    PdfAnnotationData ci = shapeData(PdfAnnotationKind::Circle);
    ci.interiorColor = PdfColor{0.0F, 0.0F, 1.0F};
    ap = buildAppearance(ci);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(1));
    std::size_t cubics = 0;
    for (const auto& seg : ap.paths[0].segments) {
        if (seg.op == PdfPathSegment::Op::CubicTo) {
            ++cubics;
        }
    }
    CHECK_EQ(cubics, static_cast<std::size_t>(4));
    CHECK(ap.paths[0].segments.back().op == PdfPathSegment::Op::Close);
    CHECK(ap.paths[0].fill.has_value());
    CHECK(ap.paths[0].stroke.has_value());
}

RIVET_TEST(annotationAppearanceLineAndArrow) {
    PdfAppearance ap = buildAppearance(lineData(PdfAnnotationKind::Line));
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(1));
    CHECK_EQ(ap.paths[0].segments.size(), static_cast<std::size_t>(2));
    CHECK(ap.paths[0].roundJoins);
    CHECK(!ap.paths[0].fill.has_value());

    ap = buildAppearance(lineData(PdfAnnotationKind::Arrow));
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(2));
    const auto& head = ap.paths[1];
    CHECK_EQ(head.segments.size(), static_cast<std::size_t>(3));
    CHECK(head.roundJoins);
    CHECK(head.stroke.has_value());
    CHECK_NEAR(head.segments[1].p.x, 100.0, kEps);
    CHECK_NEAR(head.segments[0].p.x, 100.0 - 6.0 * std::cos(std::acos(-1.0) / 6.0), 1e-9);
    CHECK_NEAR(head.segments[0].p.y, -3.0, 1e-9);
    CHECK_NEAR(head.segments[2].p.y, 3.0, 1e-9);
}

RIVET_TEST(annotationAppearanceStamp) {
    PdfAnnotationData st = shapeData(PdfAnnotationKind::Stamp);
    st.stampName = PdfStampName::Confidential;
    st.rotation = 90;
    st.color = PdfColor{0.8F, 0.0F, 0.0F};
    const PdfAppearance ap = buildAppearance(st);
    CHECK_EQ(ap.paths.size(), static_cast<std::size_t>(1));
    CHECK(ap.paths[0].stroke.has_value());
    CHECK(!ap.paths[0].fill.has_value());
    // w = 100, h = 60: fw = max(1.5, 3.6) = 3.6.
    CHECK_NEAR(static_cast<double>(ap.paths[0].strokeWidth), 3.6, 1e-5);
    CHECK_EQ(ap.texts.size(), static_cast<std::size_t>(1));
    CHECK_EQ(ap.texts[0].text, std::string("CONFIDENTIAL"));
    CHECK(ap.texts[0].bold);
    CHECK_EQ(ap.texts[0].rotation, 90);
    CHECK(nearBox(ap.texts[0].box, PdfBox{17.2, 27.2, 102.8, 72.8}));

    const char* const names[] = {"APPROVED", "DRAFT", "CONFIDENTIAL", "FINAL"};
    const PdfStampName all[] = {PdfStampName::Approved, PdfStampName::Draft, PdfStampName::Confidential,
                                PdfStampName::Final};
    for (int i = 0; i < 4; ++i) {
        st.stampName = all[i];
        CHECK_EQ(buildAppearance(st).texts[0].text, std::string(names[i]));
    }
}

RIVET_TEST(annotationAppearanceOtherIsEmpty) {
    PdfAnnotationData o;
    o.kind = PdfAnnotationKind::Other;
    const PdfAppearance ap = buildAppearance(o);
    CHECK(ap.paths.empty());
    CHECK(ap.texts.empty());
    CHECK(appearanceContentStream(ap).empty());
}

RIVET_TEST(annotationContentStreamBasics) {
    PdfAnnotationData h = markup(PdfAnnotationKind::Highlight);
    h.color = PdfColor{1.0F, 0.5F, 0.0F};
    std::string cs = appearanceContentStream(buildAppearance(h));
    CHECK(!contains(cs, "/GS gs"));
    CHECK(contains(cs, "q\n"));
    CHECK(contains(cs, "1 0.5 0 rg\n"));
    CHECK(!contains(cs, "RG"));
    CHECK(contains(cs, "10 34 m\n"));
    CHECK(contains(cs, "h\nf\nQ\n"));
    CHECK(contains(cs, "0 J 0 j\n"));

    h.opacity = 0.5F;
    cs = appearanceContentStream(buildAppearance(h));
    CHECK(cs.rfind("/GS gs\n", 0) == 0);

    // Stroke only -> S; round joins.
    PdfAnnotationData ink = inkData();
    cs = appearanceContentStream(buildAppearance(ink));
    CHECK(contains(cs, "1 J 1 j\n"));
    CHECK(contains(cs, "RG\n"));
    CHECK(contains(cs, "\nS\nQ\n"));
    CHECK(!contains(cs, "/GS gs"));

    // Fill + stroke -> B, with a Bezier operator for the ellipse.
    PdfAnnotationData ci = shapeData(PdfAnnotationKind::Circle);
    ci.interiorColor = PdfColor{0.0F, 1.0F, 0.0F};
    cs = appearanceContentStream(buildAppearance(ci));
    CHECK(contains(cs, " c\n"));
    CHECK(contains(cs, "\nB\nQ\n"));
    CHECK(contains(cs, "RG\n"));
    CHECK(contains(cs, "0 1 0 rg\n"));

    // Neither fill nor stroke -> n.
    PdfAppearance bare;
    PdfAppearancePath p;
    p.segments = {PdfPathSegment{PdfPathSegment::Op::MoveTo, PdfPoint{1.0, 2.0}, {}, {}}};
    bare.paths.push_back(p);
    cs = appearanceContentStream(bare);
    CHECK(contains(cs, "1 2 m\nn\nQ\n"));

    // Texts are ignored.
    cs = appearanceContentStream(buildAppearance(shapeData(PdfAnnotationKind::Stamp)));
    CHECK(!contains(cs, "APPROVED"));
}

RIVET_TEST(annotationContentStreamIsAscii) {
    PdfAnnotationData d = shapeData(PdfAnnotationKind::Note);
    d.contents = "\xD0\x9F\xD1\x80";
    d.opacity = 0.3F;
    for (const PdfAnnotationKind kind :
         {PdfAnnotationKind::Note, PdfAnnotationKind::Circle, PdfAnnotationKind::Stamp}) {
        d.kind = kind;
        for (const char ch : appearanceContentStream(buildAppearance(d))) {
            const auto u = static_cast<unsigned char>(ch);
            CHECK(u == '\n' || (u >= 0x20 && u < 0x7F));
        }
    }
}

RIVET_TEST(annotationContentStreamNumberFormatting) {
    auto render = [](double x, double y) {
        PdfAppearance ap;
        PdfAppearancePath p;
        p.segments.push_back(PdfPathSegment{PdfPathSegment::Op::MoveTo, PdfPoint{x, y}, {}, {}});
        ap.paths.push_back(p);
        return appearanceContentStream(ap);
    };
    CHECK(contains(render(1.23456, 2.0), "1.2346 2 m\n"));
    CHECK(contains(render(-0.0, 0.00001), "0 0 m\n"));
    CHECK(contains(render(-0.00004, 100.5), "0 100.5 m\n"));
    CHECK(contains(render(-3.10, 0.1000), "-3.1 0.1 m\n"));
    CHECK(contains(render(123456.0, -0.5), "123456 -0.5 m\n"));
    CHECK(contains(render(0.99999, 7.0), "1 7 m\n"));
}
