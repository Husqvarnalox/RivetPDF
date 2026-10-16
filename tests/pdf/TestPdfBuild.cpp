// SPDX-License-Identifier: MPL-2.0

#include "RivetTest.h"

#include "core/Error.hpp"
#include "pdf/PdfBuild.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <utility>

// PDFium-free validation of the document-build description (ADR-0023).
// Both build modes.

namespace {

using namespace rivet::pdf;
using rivet::core::ErrorCode;

PdfBuildText text(std::string utf8, PdfBundledFont font = PdfBundledFont::SansRegular) {
    PdfBuildText t;
    t.font = font;
    t.origin = {72.0, 700.0};
    t.utf8 = std::move(utf8);
    return t;
}

PdfBuildDocument onePage() {
    PdfBuildDocument doc;
    PdfBuildPage page;
    page.items.emplace_back(text("Hello"));
    PdfBuildRect rect;
    rect.box = {72.0, 72.0, 200.0, 100.0};
    rect.fill = PdfColor{0.5F, 0.5F, 0.5F};
    page.items.emplace_back(rect);
    PdfBuildImage image;
    image.image.format = PdfImageData::Format::Bgra;
    image.image.width = 2;
    image.image.height = 2;
    image.image.stride = 8;
    image.image.bytes.assign(16, 0xFF);
    image.box = {300.0, 300.0, 400.0, 400.0};
    page.items.emplace_back(image);
    page.links.push_back({{72.0, 690.0, 120.0, 712.0}, "https://example.com/a?b=c"});
    doc.pages.push_back(std::move(page));
    return doc;
}

bool rejected(const PdfBuildDocument& doc, std::string* message = nullptr) {
    const rivet::core::Status status = validatePdfBuild(doc);
    if (status.has_value()) return false;
    if (message != nullptr) *message = status.error().message;
    return status.error().code == ErrorCode::InvalidArgument;
}

} // namespace

RIVET_TEST(pdfBuildAcceptsWellFormedDocument) {
    CHECK(validatePdfBuild(onePage()).has_value());
    // Cyrillic and Greek are covered by every bundled face.
    PdfBuildDocument doc = onePage();
    doc.pages[0].items.emplace_back(text("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xCE\xA9\xCE\xBC\xCE\xAD\xCE\xB3\xCE\xB1",
                                         PdfBundledFont::MonoBold));
    // Mail and http links, any scheme case.
    doc.pages[0].links.push_back({{10.0, 10.0, 20.0, 20.0}, "mailto:someone@example.com"});
    doc.pages[0].links.push_back({{10.0, 10.0, 20.0, 20.0}, "HTTP://example.com"});
    CHECK(validatePdfBuild(doc).has_value());
    // A box touching the page edges is inside the page.
    PdfBuildRect edge;
    edge.box = {0.0, 0.0, 612.0, 792.0};
    edge.stroke = PdfColor{};
    doc.pages[0].items.emplace_back(edge);
    CHECK(validatePdfBuild(doc).has_value());
}

RIVET_TEST(pdfBuildRejectsStructuralProblems) {
    CHECK(rejected(PdfBuildDocument{}));

    PdfBuildDocument doc = onePage();
    doc.pages[0].width = 2.0;
    CHECK(rejected(doc));
    doc.pages[0].width = std::numeric_limits<double>::quiet_NaN();
    CHECK(rejected(doc));
    doc.pages[0].width = kMaxBuildPageSize + 1.0;
    CHECK(rejected(doc));

    doc = onePage();
    std::get<PdfBuildRect>(doc.pages[0].items[1]).box = {500.0, 72.0, 700.0, 100.0}; // past the right edge
    CHECK(rejected(doc));
    std::get<PdfBuildRect>(doc.pages[0].items[1]).box = {100.0, 72.0, 90.0, 100.0}; // inverted
    CHECK(rejected(doc));

    doc = onePage();
    std::get<PdfBuildRect>(doc.pages[0].items[1]).fill.reset();
    CHECK(rejected(doc)); // neither fill nor stroke
    std::get<PdfBuildRect>(doc.pages[0].items[1]).stroke = PdfColor{0.0F, 0.0F, 2.0F};
    CHECK(rejected(doc)); // color out of range
    std::get<PdfBuildRect>(doc.pages[0].items[1]).stroke = PdfColor{};
    std::get<PdfBuildRect>(doc.pages[0].items[1]).strokeWidth = 0.0F;
    CHECK(rejected(doc));

    doc = onePage();
    std::get<PdfBuildImage>(doc.pages[0].items[2]).image.bytes.resize(4); // shorter than its rows
    CHECK(rejected(doc));

    doc = onePage();
    doc.pages[0].links[0].box = {-5.0, 0.0, 10.0, 10.0};
    CHECK(rejected(doc));
}

RIVET_TEST(pdfBuildValidatesPaths) {
    const auto triangle = [] {
        PdfBuildPath path;
        path.segments.push_back({PdfPathSegment::Op::MoveTo, {100.0, 100.0}, {}, {}});
        path.segments.push_back({PdfPathSegment::Op::LineTo, {200.0, 100.0}, {}, {}});
        path.segments.push_back({PdfPathSegment::Op::CubicTo, {150.0, 180.0}, {210.0, 150.0}, {170.0, 190.0}});
        path.segments.push_back({PdfPathSegment::Op::Close, {}, {}, {}});
        path.fill = PdfColor{0.2F, 0.4F, 0.6F};
        path.stroke = PdfColor{};
        path.strokeWidth = 1.5F;
        path.dash = {4.0F, 3.0F};
        return path;
    };
    PdfBuildDocument doc = onePage();
    doc.pages[0].items.emplace_back(triangle());
    CHECK(validatePdfBuild(doc).has_value());

    const auto rejectedWith = [&](auto mutate) {
        PdfBuildDocument bad = onePage();
        PdfBuildPath path = triangle();
        mutate(path);
        bad.pages[0].items.emplace_back(std::move(path));
        return rejected(bad);
    };
    CHECK(rejectedWith([](PdfBuildPath& p) { p.segments.clear(); }));
    CHECK(rejectedWith([](PdfBuildPath& p) { p.segments.front().op = PdfPathSegment::Op::LineTo; }));
    CHECK(rejectedWith([](PdfBuildPath& p) { p.segments[1].p.x = 700.0; }));  // off the page
    CHECK(rejectedWith([](PdfBuildPath& p) { p.segments[2].c2.y = -1.0; })); // a control point too
    CHECK(rejectedWith([](PdfBuildPath& p) { p.segments[1].p.y = std::numeric_limits<double>::infinity(); }));
    CHECK(rejectedWith([](PdfBuildPath& p) {
        p.fill.reset();
        p.stroke.reset();
    }));
    CHECK(rejectedWith([](PdfBuildPath& p) { p.fill = PdfColor{1.5F, 0.0F, 0.0F}; }));
    CHECK(rejectedWith([](PdfBuildPath& p) { p.strokeWidth = 0.0F; }));
    CHECK(rejectedWith([](PdfBuildPath& p) { p.dash = {4.0F, 0.0F}; }));
    CHECK(rejectedWith([](PdfBuildPath& p) { p.dash.assign(kMaxBuildDashCount + 1, 1.0F); }));
    CHECK(rejectedWith([](PdfBuildPath& p) {
        p.segments.resize(kMaxBuildPathSegments + 1, PdfPathSegment{PdfPathSegment::Op::LineTo, {150.0, 150.0}, {}, {}});
    }));
    // A dash pattern is ignored without a stroke but must still be valid; fill only is fine.
    CHECK(!rejectedWith([](PdfBuildPath& p) {
        p.stroke.reset();
        p.dash.clear();
    }));
}

RIVET_TEST(pdfBuildRejectsBadText) {
    std::string message;
    PdfBuildDocument doc = onePage();
    // An uncovered CJK character is named as U+XXXX.
    std::get<PdfBuildText>(doc.pages[0].items[0]).utf8 = "ab\xE4\xB8\xAD";
    CHECK(rejected(doc, &message));
    CHECK(message.find("U+4E2D") != std::string::npos);

    std::get<PdfBuildText>(doc.pages[0].items[0]).utf8 = "line\nbreak";
    CHECK(rejected(doc, &message));
    CHECK(message.find("U+000A") != std::string::npos);
    std::get<PdfBuildText>(doc.pages[0].items[0]).utf8 = "tab\there";
    CHECK(rejected(doc));
    std::get<PdfBuildText>(doc.pages[0].items[0]).utf8 = "bad \xC3";
    CHECK(rejected(doc));
    std::get<PdfBuildText>(doc.pages[0].items[0]).utf8.clear();
    CHECK(rejected(doc));
    std::get<PdfBuildText>(doc.pages[0].items[0]).utf8 = std::string(kMaxBuildTextBytes + 1, 'a');
    CHECK(rejected(doc));

    doc = onePage();
    std::get<PdfBuildText>(doc.pages[0].items[0]).fontSize = 0.5;
    CHECK(rejected(doc));
    std::get<PdfBuildText>(doc.pages[0].items[0]).fontSize = 11.0;
    std::get<PdfBuildText>(doc.pages[0].items[0]).origin = {700.0, 10.0};
    CHECK(rejected(doc));
    std::get<PdfBuildText>(doc.pages[0].items[0]).origin = {10.0, std::numeric_limits<double>::infinity()};
    CHECK(rejected(doc));
    std::get<PdfBuildText>(doc.pages[0].items[0]).origin = {10.0, 10.0};
    std::get<PdfBuildText>(doc.pages[0].items[0]).color = PdfColor{-0.1F, 0.0F, 0.0F};
    CHECK(rejected(doc));
}

RIVET_TEST(pdfBuildRejectsBadLinks) {
    const char* const bad[] = {
        "javascript:alert(1)", "file:///etc/passwd", "ftp://example.com", "noscheme", ":nothing",
        "https:", "https:/x", "mailto:", "https://exa mple.com", "https://example.com/\x01",
        "https://ex\xC3\xA4mple.com",
    };
    for (const char* uri : bad) {
        PdfBuildDocument doc = onePage();
        doc.pages[0].links[0].uri = uri;
        CHECK(rejected(doc));
    }
    PdfBuildDocument doc = onePage();
    doc.pages[0].links[0].uri = "https://example.com/" + std::string(kMaxBuildUriBytes, 'a');
    CHECK(rejected(doc));
}
