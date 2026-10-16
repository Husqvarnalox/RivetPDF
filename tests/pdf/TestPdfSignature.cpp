// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "pdf/BundledFontOutlines.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfSignatures.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// Engine-independent visual-signature appearances (ink normalization, typed
// auto-size, image fit), bundled glyph outlines and byte-range analysis
// (ADR-0025): runs in both build modes.

namespace {

using namespace rivet::pdf;
namespace core = rivet::core;

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

bool nearBox(const PdfBox& a, const PdfBox& b, double eps = 1e-6) {
    return near(a.left, b.left, eps) && near(a.bottom, b.bottom, eps) && near(a.right, b.right, eps) &&
           near(a.top, b.top, eps);
}

struct Extent {
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();
    void add(const PdfPoint& p) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
};

Extent pathExtent(const PdfAppearance& ap) {
    Extent e;
    for (const PdfAppearancePath& path : ap.paths) {
        for (const PdfPathSegment& s : path.segments) {
            if (s.op != PdfPathSegment::Op::Close) e.add(s.p);
        }
    }
    return e;
}

PdfSignatureAppearancePtr share(PdfSignatureAppearance s) {
    return std::make_shared<const PdfSignatureAppearance>(std::move(s));
}

PdfAnnotationData signatureData(PdfSignatureAppearancePtr sig, PdfBox rect, int rotation = 0) {
    PdfAnnotationData d;
    d.kind = PdfAnnotationKind::Signature;
    d.rect = rect;
    d.rotation = rotation;
    d.color = PdfColor{0.0F, 0.0F, 0.4F};
    d.signature = std::move(sig);
    normalizeAnnotation(d);
    return d;
}

std::shared_ptr<const core::Bitmap> bitmap(std::uint32_t w, std::uint32_t h) {
    auto created = core::Bitmap::create(w, h);
    CHECK(created.has_value());
    return std::make_shared<const core::Bitmap>(std::move(*created));
}

} // namespace

RIVET_TEST(signatureInkNormalization) {
    // Pad space is y-down: (10,10) is the visual top-left.
    const std::vector<std::vector<PdfPoint>> pad = {{{10, 10}, {110, 60}}, {{60, 35}}};
    const auto sig = makeInkSignature(pad, 2.0);
    CHECK(sig.has_value());
    if (!sig) return;
    CHECK(sig->source == PdfSignatureSource::Ink);
    // Tight box padded by half the pen: 102 x 52.
    CHECK(near(sig->aspect, 102.0 / 52.0));
    CHECK(near(static_cast<double>(sig->inkWidth), 2.0 / 52.0, 1e-6));
    CHECK_EQ(sig->strokes.size(), std::size_t{2});
    // The visual top-left point lands at the top (y = 1 - pad) of the unit-height box.
    CHECK(near(sig->strokes[0][0].x, 1.0 / 52.0));
    CHECK(near(sig->strokes[0][0].y, 1.0 - 1.0 / 52.0));
    CHECK(near(sig->strokes[0][1].x, 101.0 / 52.0));
    CHECK(near(sig->strokes[0][1].y, 1.0 / 52.0));
    CHECK(isValidSignature(*sig));
    CHECK(near(signatureAspect(*sig), 102.0 / 52.0));
}

RIVET_TEST(signatureInkFlatLineKeepsUsableAspect) {
    const auto sig = makeInkSignature({{{0, 50}, {400, 50}}}, 1.0);
    CHECK(sig.has_value());
    if (!sig) return;
    CHECK(near(sig->aspect, kMaxSignatureAspect));
    // The line sits in the vertical middle of the padded box.
    CHECK(near(sig->strokes[0][0].y, 0.5));
}

RIVET_TEST(signatureInkLimits) {
    CHECK(!makeInkSignature({}, 2.0).has_value());
    CHECK(!makeInkSignature({{}}, 2.0).has_value());
    CHECK(!makeInkSignature({{{0, 0}, {std::nan(""), 1}}}, 2.0).has_value());
    std::vector<std::vector<PdfPoint>> many(kMaxSignatureStrokes + 1, std::vector<PdfPoint>{{1, 1}});
    CHECK(!makeInkSignature(many, 2.0).has_value());
    std::vector<PdfPoint> longStroke(kMaxSignaturePoints + 1, PdfPoint{3, 4});
    CHECK(!makeInkSignature({longStroke}, 2.0).has_value());
    longStroke.resize(kMaxSignaturePoints);
    longStroke.back() = PdfPoint{50, 9};
    CHECK(makeInkSignature({longStroke}, 2.0).has_value());

    PdfSignatureAppearance bad;
    bad.source = PdfSignatureSource::Ink;
    CHECK(!isValidSignature(bad)); // no strokes
    bad.strokes = {{{0.5, 0.5}}};
    bad.inkWidth = 0.0F;
    CHECK(!isValidSignature(bad));
}

RIVET_TEST(signatureInkAppearanceFitsRect) {
    const auto sig = makeInkSignature({{{0, 0}, {200, 100}}}, 4.0);
    CHECK(sig.has_value());
    if (!sig) return;
    const PdfAnnotationData data = signatureData(share(*sig), PdfBox{100, 100, 400, 200});
    CHECK(isWritableAnnotation(data));
    const PdfAppearance ap = buildAppearance(data);
    CHECK_EQ(ap.paths.size(), std::size_t{1});
    CHECK(ap.images.empty());
    CHECK(ap.paths[0].stroke.has_value());
    CHECK(ap.paths[0].roundJoins);
    // Fitted by height: 100 tall, 204/104*100 wide, centered horizontally.
    const double width = 204.0 / 104.0 * 100.0;
    const Extent e = pathExtent(ap);
    CHECK(e.minY >= 100.0 - 1e-6);
    CHECK(e.maxY <= 200.0 + 1e-6);
    CHECK(near(e.minX, 250.0 - width / 2.0 + 2.0 / 104.0 * 100.0, 1e-6));
    CHECK(near(e.maxX, 250.0 + width / 2.0 - 2.0 / 104.0 * 100.0, 1e-6));
    // The pen scales with the placed height.
    CHECK(near(static_cast<double>(ap.paths[0].strokeWidth), 4.0 / 104.0 * 100.0, 1e-3));
}

RIVET_TEST(signatureFitBoxAndRotation) {
    const PdfBox available{0, 0, 300, 300};
    CHECK(nearBox(fitSignatureBox(2.0, available, 0), PdfBox{0, 75, 300, 225}));
    // A quarter turn: the content width runs along user y.
    CHECK(nearBox(fitSignatureBox(2.0, available, 90), PdfBox{75, 0, 225, 300}));
    CHECK(nearBox(fitSignatureBox(0.5, PdfBox{0, 0, 400, 100}, 0), PdfBox{175, 0, 225, 100}));
    // Out-of-range aspects are clamped.
    CHECK(nearBox(fitSignatureBox(1000.0, PdfBox{0, 0, 200, 200}, 0),
                  fitSignatureBox(kMaxSignatureAspect, PdfBox{0, 0, 200, 200}, 0)));
}

RIVET_TEST(signatureImageFit) {
    PdfSignatureAppearance sig;
    sig.source = PdfSignatureSource::Image;
    sig.image = bitmap(200, 100);
    CHECK(isValidSignature(sig));
    CHECK(near(signatureAspect(sig), 2.0));
    const PdfAnnotationData data = signatureData(share(sig), PdfBox{0, 0, 300, 300});
    const PdfAppearance ap = buildAppearance(data);
    CHECK_EQ(ap.images.size(), std::size_t{1});
    CHECK(ap.paths.empty());
    if (ap.images.empty()) return;
    CHECK(nearBox(ap.images[0].box, PdfBox{0, 75, 300, 225}));
    CHECK_EQ(ap.images[0].rotation, 0);
    CHECK(ap.images[0].bitmap == sig.image);

    const PdfAppearance turned = buildAppearance(signatureData(share(sig), PdfBox{0, 0, 300, 300}, 270));
    CHECK_EQ(turned.images.size(), std::size_t{1});
    if (!turned.images.empty()) {
        CHECK(nearBox(turned.images[0].box, PdfBox{75, 0, 225, 300}));
        CHECK_EQ(turned.images[0].rotation, 270);
    }

    PdfSignatureAppearance tooBig;
    tooBig.source = PdfSignatureSource::Image;
    tooBig.image = bitmap(kMaxSignatureImageSide + 1, 10);
    CHECK(!isValidSignature(tooBig));
    tooBig.image = nullptr;
    CHECK(!isValidSignature(tooBig));
}

RIVET_TEST(signatureTypedAutoSize) {
    const std::string name = "Jane Doe";
    // The size makes the line fill one side exactly and fit the other.
    const double wide = signatureFontSize(name, 1000.0, 40.0);
    const double tall = signatureFontSize(name, 100.0, 400.0);
    CHECK(wide > 0.0);
    CHECK(tall > 0.0);
    CHECK(tall < wide);
    CHECK(near(signatureFontSize(name, 200.0, 80.0) * 2.0, signatureFontSize(name, 400.0, 160.0), 1e-9));
    CHECK(near(signatureFontSize("", 100.0, 100.0), 0.0));

    PdfSignatureAppearance sig;
    sig.source = PdfSignatureSource::Text;
    sig.text = name;
    CHECK(isValidSignature(sig));
    const double aspect = signatureAspect(sig);
    CHECK(aspect > 1.0);
    const PdfAnnotationData data = signatureData(share(sig), PdfBox{100, 100, 400, 160});
    const PdfAppearance ap = buildAppearance(data);
    CHECK_EQ(ap.paths.size(), std::size_t{1}); // all glyphs in one filled path
    if (ap.paths.empty()) return;
    CHECK(ap.paths[0].fill.has_value());
    CHECK(!ap.paths[0].stroke.has_value());
    CHECK(ap.paths[0].segments.front().op == PdfPathSegment::Op::MoveTo);
    // The glyph ink stays inside the fitted box and spans most of its width.
    const PdfBox box = fitSignatureBox(aspect, data.rect, 0);
    const Extent e = pathExtent(ap);
    CHECK(e.minX >= box.left - 1.0);
    CHECK(e.maxX <= box.right + 1.0);
    CHECK(e.minY >= box.bottom - 1.0);
    CHECK(e.maxY <= box.top + 1.0);
    CHECK(e.maxX - e.minX > 0.8 * box.width());

    PdfSignatureAppearance blank;
    blank.source = PdfSignatureSource::Text;
    blank.text = "   ";
    CHECK(!isValidSignature(blank));
    blank.text = std::string(kMaxSignatureTextBytes + 1, 'a');
    CHECK(!isValidSignature(blank));
    blank.text = "a\nb";
    CHECK(!isValidSignature(blank));
    blank.text = std::string("\xC3\x28", 2); // invalid UTF-8
    CHECK(!isValidSignature(blank));
}

RIVET_TEST(signatureNeedsAppearanceToBeWritable) {
    PdfAnnotationData d;
    d.kind = PdfAnnotationKind::Signature;
    d.rect = PdfBox{0, 0, 100, 40};
    CHECK(!isWritableAnnotation(d));
    PdfSignatureAppearance sig;
    sig.source = PdfSignatureSource::Text;
    sig.text = "Ok";
    d.signature = share(sig);
    CHECK(isWritableAnnotation(d));
    d.rect = PdfBox{0, 0, 0.5, 40};
    CHECK(!isWritableAnnotation(d));
}

RIVET_TEST(bundledGlyphOutlines) {
    const auto a = bundledGlyphOutline(PdfBundledFont::SerifRegular, U'A');
    CHECK(!a.empty());
    if (a.empty()) return;
    CHECK(a.front().op == PdfPathSegment::Op::MoveTo);
    CHECK(a.back().op == PdfPathSegment::Op::Close);
    bool curve = false;
    for (const PdfPathSegment& s : bundledGlyphOutline(PdfBundledFont::SerifRegular, U'o')) {
        if (s.op == PdfPathSegment::Op::CubicTo) curve = true;
    }
    CHECK(curve); // quadratic contours become cubics
    CHECK(bundledGlyphOutline(PdfBundledFont::SerifRegular, U' ').empty());
    CHECK(bundledGlyphOutline(PdfBundledFont::SerifRegular, U'\U0001F600').empty());
    // An accented letter is a composite glyph in most TrueType fonts.
    CHECK(!bundledGlyphOutline(PdfBundledFont::SerifRegular, U'é').empty());
    // Garbage programs never crash and yield nothing.
    const std::vector<std::uint8_t> junk(64, 0xFF);
    CHECK(trueTypeGlyphOutline(junk, 5).empty());
    CHECK(trueTypeGlyphOutline({}, 1).empty());
}

RIVET_TEST(byteRangeCoverage) {
    const std::vector<std::int64_t> whole = {0, 100, 300, 700};
    const PdfByteRangeCoverage full = analyzeByteRange(whole, 1000);
    CHECK(full.valid);
    CHECK(full.coversWholeDocument);
    CHECK(!full.modifiedAfterSigning);
    CHECK_EQ(full.signedEnd, std::uint64_t{1000});

    const PdfByteRangeCoverage appended = analyzeByteRange(whole, 1500);
    CHECK(appended.valid);
    CHECK(!appended.coversWholeDocument);
    CHECK(appended.modifiedAfterSigning);

    // Not starting at 0, or no hole, or two holes: valid but partial.
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{10, 90, 300, 700}, 1000).coversWholeDocument);
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{0, 1000}, 1000).coversWholeDocument);
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{0, 100, 200, 100, 400, 600}, 1000).coversWholeDocument);

    // Malformed.
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{}, 1000).valid);
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{0, 100, 300}, 1000).valid);
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{0, -1, 300, 700}, 1000).valid);
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{0, 400, 300, 700}, 1000).valid); // overlap
    CHECK(!analyzeByteRange(std::vector<std::int64_t>{0, 100, 300, 800}, 1000).valid); // past the end

    PdfSignatureInfo info;
    info.coverage = full;
    CHECK(signatureStatusText(info).find("not verified") != std::string::npos);
    CHECK(signatureStatusText(info).find("whole document") != std::string::npos);
    info.coverage = appended;
    CHECK(signatureStatusText(info).find("changed after signing") != std::string::npos);
}
