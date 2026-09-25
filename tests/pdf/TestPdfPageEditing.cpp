// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef RIVET_PDF_TEXT_FIXTURE_DIR
#define RIVET_PDF_TEXT_FIXTURE_DIR "tests/pdf/fixtures"
#endif

// Page-editing foundation: view-aware reads (renderPage/textPage/pageLinks
// through a PdfPageView) and PdfEngine::assembleDocument round trips against
// the markers-5 / import-3 / cropbox / password fixtures. PDFium-ON bodies
// only; every body returns early when no PDFium backend exists.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using rivet::pdf::PdfAssemblyPage;
using rivet::pdf::PdfAssemblyRequest;
using rivet::pdf::PdfBox;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfPageView;

fs::path fixture(const char* name) {
    return fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / name;
}

// Null (after an OFF-build reporting CHECK) when no PDFium backend exists.
std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = rivet::pdf::createEngine();
    CHECK(engine != nullptr);
    if (!engine) return nullptr;
    if (!engine->isAvailable()) {
        CHECK_EQ(engine->backendName(), "none");
        return nullptr;
    }
    return engine;
}

std::unique_ptr<PdfDocument> openPath(PdfEngine& engine, const fs::path& path, std::string_view password = {}) {
    auto opened = engine.openDocument(path, password);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

PdfPageView nativeView(const PdfDocument& document, std::size_t page) {
    const auto info = document.pageInfo(page);
    CHECK(info.has_value());
    return info.has_value() ? info->view : PdfPageView{};
}

// Collects every byte an assembly produces.
class MemorySink final : public rivet::pdf::IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        const auto* bytes = static_cast<const char*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
        return core::ok();
    }
    const std::vector<char>& bytes() const { return bytes_; }

private:
    std::vector<char> bytes_;
};

// Fails on the N-th write (0-based) with an Io error, or throws.
class FailingSink final : public rivet::pdf::IPdfByteSink {
public:
    FailingSink(std::size_t failAt, bool throwInstead) : failAt_(failAt), throw_(throwInstead) {}
    core::Status write(const void*, std::size_t) override {
        if (calls_++ == failAt_) {
            if (throw_) throw std::runtime_error("sink exploded");
            return std::unexpected(core::makeError(core::ErrorCode::Io, "disk full (test)", "test"));
        }
        return core::ok();
    }
    std::size_t calls() const { return calls_; }

private:
    std::size_t failAt_;
    bool throw_;
    std::size_t calls_ = 0;
};

fs::path uniqueTempPath(const char* tag) {
    static std::atomic<int> counter{0};
    return fs::temp_directory_path() /
           ("rivet-page-editing-" + std::string(tag) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(counter.fetch_add(1)) + ".pdf");
}

void writeFile(const fs::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    CHECK(out.good());
}

void removeQuietly(const fs::path& path) {
    std::error_code ignored;
    fs::remove(path, ignored);
}

// Writes assembled bytes to a temp file and opens them. The document keeps
// its own descriptor (PdfiumFileSource), so the file is unlinked right away.
std::unique_ptr<PdfDocument> reload(PdfEngine& engine, const MemorySink& sink, std::string_view password = {}) {
    CHECK(!sink.bytes().empty());
    const fs::path path = uniqueTempPath("reload");
    writeFile(path, sink.bytes());
    auto opened = engine.openDocument(path, password);
    removeQuietly(path);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

// The page's marker token ("PAGE-3", "IMPORT-2", "Page 2 of 4"), or the raw
// text when no known marker is found.
std::string marker(const PdfDocument& document, std::size_t page) {
    const auto text = document.textPage(page);
    CHECK(text.has_value());
    if (!text.has_value()) return "<no text>";
    const std::string& all = (*text)->text();
    for (const std::string_view prefix : {std::string_view("PAGE-"), std::string_view("IMPORT-")}) {
        const auto at = all.find(prefix);
        if (at != std::string::npos && at + prefix.size() < all.size()) {
            return all.substr(at, prefix.size() + 1);
        }
    }
    const auto at = all.find("Page ");
    if (at != std::string::npos) return all.substr(at, 11);
    return all;
}

std::vector<std::string> markers(const PdfDocument& document) {
    std::vector<std::string> result;
    for (std::size_t i = 0; i < document.info().pageCount; ++i) result.push_back(marker(document, i));
    return result;
}

std::vector<std::string> strings(std::initializer_list<const char*> items) {
    return std::vector<std::string>(items.begin(), items.end());
}

bool nearBox(const PdfBox& a, const PdfBox& b, double eps = 0.01) {
    return std::fabs(a.left - b.left) <= eps && std::fabs(a.bottom - b.bottom) <= eps &&
           std::fabs(a.right - b.right) <= eps && std::fabs(a.top - b.top) <= eps;
}

bool nearRect(const core::Rect& a, const core::Rect& b, double eps) {
    return std::fabs(a.minX() - b.minX()) <= eps && std::fabs(a.minY() - b.minY()) <= eps &&
           std::fabs(a.maxX() - b.maxX()) <= eps && std::fabs(a.maxY() - b.maxY()) <= eps;
}

// Fraction of non-white pixels in a device-pixel region (clipped to the
// bitmap).
double inkRatio(const core::Bitmap& bitmap, std::uint32_t px, std::uint32_t py, std::uint32_t pw,
                std::uint32_t ph) {
    if (px >= bitmap.width() || py >= bitmap.height()) return 0.0;
    const std::uint32_t w = std::min(pw, bitmap.width() - px);
    const std::uint32_t h = std::min(ph, bitmap.height() - py);
    const auto* base = reinterpret_cast<const std::uint8_t*>(bitmap.data());
    std::size_t ink = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto* row = base + (static_cast<std::size_t>(py) + y) * bitmap.stride() +
                          static_cast<std::size_t>(px) * 4;
        for (std::uint32_t x = 0; x < w; ++x) {
            if (row[x * 4] < 250 || row[x * 4 + 1] < 250 || row[x * 4 + 2] < 250) ++ink;
        }
    }
    return w * h == 0 ? 0.0 : static_cast<double>(ink) / static_cast<double>(w * h);
}

core::Rect fullPage(const PdfPageView& view) {
    return core::Rect{core::Point{0.0, 0.0}, rivet::pdf::displaySize(view)};
}

bool isCode(const core::Status& status, core::ErrorCode code) {
    return !status.has_value() && status.error().code == code;
}

PdfAssemblyRequest request(PdfAssemblyRequest::Mode mode, const PdfDocument* base) {
    PdfAssemblyRequest result;
    result.mode = mode;
    result.base = base;
    return result;
}

void addPage(PdfAssemblyRequest& req, const PdfDocument& source, std::size_t index) {
    req.pages.push_back(PdfAssemblyPage{&source, index, nativeView(source, index)});
}

} // namespace

// ---------------------------------------------------------------------------
// Page info and view-aware reads
// ---------------------------------------------------------------------------

RIVET_TEST(pdfiumPageInfoExposesBoxes) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("cropbox.pdf"));
    if (!doc) return;

    const auto p0 = doc->pageInfo(0);
    const auto p1 = doc->pageInfo(1);
    const auto p2 = doc->pageInfo(2);
    CHECK(p0.has_value() && p1.has_value() && p2.has_value());
    if (!p0 || !p1 || !p2) return;

    CHECK(nearBox(p0->mediaBox, PdfBox{0, 0, 612, 792}));
    CHECK(nearBox(p0->view.cropBox, PdfBox{36, 36, 576, 756}));
    CHECK(p0->view.rotation == core::PageRotation::None);
    CHECK_NEAR(p0->sizePoints.width, 540.0, 0.01);
    CHECK_NEAR(p0->sizePoints.height, 720.0, 0.01);

    CHECK(p1->view.rotation == core::PageRotation::Clockwise90);
    CHECK(p1->rotation == core::PageRotation::Clockwise90);
    CHECK(nearBox(p1->view.cropBox, PdfBox{36, 36, 576, 756}));
    CHECK_NEAR(p1->sizePoints.width, 720.0, 0.01);
    CHECK_NEAR(p1->sizePoints.height, 540.0, 0.01);

    CHECK(nearBox(p2->mediaBox, PdfBox{100, 200, 400, 600}));
    CHECK(nearBox(p2->view.cropBox, PdfBox{100, 200, 400, 600}));
}

RIVET_TEST(pdfiumNativeViewRenderMatchesClassicRender) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    for (const char* name : {"markers-5.pdf", "cropbox.pdf", "rot90.pdf", "rot270.pdf"}) {
        auto doc = openPath(*engine, fixture(name));
        if (!doc) return;
        for (std::size_t page = 0; page < doc->info().pageCount; ++page) {
            const PdfPageView view = nativeView(*doc, page);
            const core::Rect rect = fullPage(view);
            auto classic = doc->renderPage(page, rect, 0.5);
            auto viewed = doc->renderPage(page, view, rect, 0.5);
            CHECK(classic.has_value() && viewed.has_value());
            if (!classic || !viewed) continue;
            CHECK_EQ(classic->width(), viewed->width());
            CHECK_EQ(classic->height(), viewed->height());
            CHECK_EQ(classic->stride(), viewed->stride());
            if (classic->sizeBytes() == viewed->sizeBytes()) {
                CHECK(std::memcmp(classic->data(), viewed->data(), classic->sizeBytes()) == 0);
            }
        }
    }
}

RIVET_TEST(pdfiumRotatedViewRendersMarkerAtMappedPosition) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("markers-5.pdf"));
    if (!doc) return;

    // Page 1's block: user x 72..132, y 100..160 on a 612x792 page.
    const PdfBox block{72, 100, 132, 160};
    for (const auto rotation : {core::PageRotation::None, core::PageRotation::Clockwise90,
                                core::PageRotation::Clockwise180, core::PageRotation::Clockwise270}) {
        const PdfPageView view{rotation, PdfBox{0, 0, 612, 792}};
        const core::Size size = rivet::pdf::displaySize(view);
        auto bitmap = doc->renderPage(0, view, fullPage(view), 1.0);
        CHECK(bitmap.has_value());
        if (!bitmap) continue;
        CHECK_EQ(bitmap->width(), static_cast<std::uint32_t>(size.width));
        CHECK_EQ(bitmap->height(), static_cast<std::uint32_t>(size.height));

        const core::Rect where = rivet::pdf::userBoxToDisplayRect(view, block);
        // Interior (5 pt inset) fully inked; the mirrored spot is empty.
        CHECK_GT(inkRatio(*bitmap, static_cast<std::uint32_t>(where.minX()) + 5,
                          static_cast<std::uint32_t>(where.minY()) + 5, 50, 50),
                 0.95);
        const std::uint32_t mirrorX = static_cast<std::uint32_t>(size.width - where.maxX());
        const std::uint32_t mirrorY = static_cast<std::uint32_t>(size.height - where.maxY());
        CHECK_LT(inkRatio(*bitmap, mirrorX + 5, mirrorY + 5, 50, 50), 0.05);
    }
}

RIVET_TEST(pdfiumCropViewsRenderExpectedRegion) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto markers = openPath(*engine, fixture("markers-5.pdf"));
    auto crop = openPath(*engine, fixture("cropbox.pdf"));
    if (!markers || !crop) return;

    // A 150x120 crop around page 1's block: block at display x 22..82,
    // y 40..100.
    const PdfPageView tight{core::PageRotation::None, PdfBox{50, 80, 200, 200}};
    auto small = markers->renderPage(0, tight, fullPage(tight), 1.0);
    CHECK(small.has_value());
    if (small) {
        CHECK_EQ(small->width(), 150u);
        CHECK_EQ(small->height(), 120u);
        CHECK_GT(inkRatio(*small, 27, 45, 50, 50), 0.95);
        CHECK_LT(inkRatio(*small, 0, 0, 15, 15), 0.05);
        CHECK_LT(inkRatio(*small, 100, 0, 50, 30), 0.05);
    }

    // cropbox.pdf page 1 natively hides the block at user 0..30 x 0..30; a
    // media-box view shows it at display 0..30 x 762..792.
    const PdfPageView media{core::PageRotation::None, PdfBox{0, 0, 612, 792}};
    auto full = crop->renderPage(0, media, fullPage(media), 1.0);
    CHECK(full.has_value());
    if (full) {
        CHECK_EQ(full->width(), 612u);
        CHECK_EQ(full->height(), 792u);
        CHECK_GT(inkRatio(*full, 3, 765, 24, 24), 0.95);
        // Crop-box corner block (user 36..72 x 720..756) -> display 36..72 x 36..72.
        CHECK_GT(inkRatio(*full, 40, 40, 28, 28), 0.95);
    }
    auto native = crop->renderPage(0, fullPage(nativeView(*crop, 0)), 1.0);
    CHECK(native.has_value());
    if (native) {
        CHECK_EQ(native->width(), 540u);
        CHECK_EQ(native->height(), 720u);
        CHECK_GT(inkRatio(*native, 4, 4, 28, 28), 0.95); // crop corner block at 0..36
    }

    // Non-zero-origin media box (page 3): the corner block at display 0..30.
    const PdfPageView offsetRot{core::PageRotation::Clockwise180, PdfBox{100, 200, 400, 600}};
    auto rotated = crop->renderPage(2, offsetRot, fullPage(offsetRot), 1.0);
    CHECK(rotated.has_value());
    if (rotated) {
        CHECK_EQ(rotated->width(), 300u);
        CHECK_EQ(rotated->height(), 400u);
        // 180: user 100..130 x 570..600 -> display 270..300 x 370..400.
        CHECK_GT(inkRatio(*rotated, 273, 373, 24, 24), 0.95);
        CHECK_LT(inkRatio(*rotated, 3, 3, 24, 24), 0.05);
    }
}

RIVET_TEST(pdfiumInvalidViewsAreRejected) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("cropbox.pdf"));
    if (!doc) return;

    const PdfPageView outside{core::PageRotation::None, PdfBox{-10, 0, 612, 792}};
    const PdfPageView empty{core::PageRotation::None, PdfBox{10, 10, 10, 50}};
    const PdfPageView badRotation{static_cast<core::PageRotation>(7), PdfBox{0, 0, 612, 792}};
    for (const PdfPageView& view : {outside, empty, badRotation}) {
        const core::Rect rect{core::Point{0, 0}, core::Size{10, 10}};
        auto render = doc->renderPage(0, view, rect, 1.0);
        CHECK(!render.has_value() && render.error().code == core::ErrorCode::InvalidArgument);
        auto text = doc->textPage(0, view);
        CHECK(!text.has_value() && text.error().code == core::ErrorCode::InvalidArgument);
        auto links = doc->pageLinks(0, view);
        CHECK(!links.has_value() && links.error().code == core::ErrorCode::InvalidArgument);
    }
    // Page 3's media box starts at (100, 200): a zero-origin view exceeds it.
    auto shifted = doc->textPage(2, PdfPageView{core::PageRotation::None, PdfBox{0, 0, 300, 400}});
    CHECK(!shifted.has_value());
    // Out-of-range page stays InvalidArgument through the view overloads.
    auto missing = doc->textPage(99, nativeView(*doc, 0));
    CHECK(!missing.has_value() && missing.error().code == core::ErrorCode::InvalidArgument);
}

RIVET_TEST(pdfiumTextRectsFollowViews) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("markers-5.pdf"));
    if (!doc) return;

    const PdfPageView native = nativeView(*doc, 0);
    auto nativeText = doc->textPage(0);
    CHECK(nativeText.has_value());
    if (!nativeText) return;
    const auto& nativeChars = (*nativeText)->chars();
    CHECK(!nativeChars.empty());
    if (nativeChars.empty()) return;

    const PdfPageView views[] = {
        {core::PageRotation::Clockwise90, native.cropBox},
        {core::PageRotation::Clockwise180, native.cropBox},
        {core::PageRotation::Clockwise270, PdfBox{40, 600, 500, 780}},
        {core::PageRotation::None, PdfBox{60, 650, 400, 770}},
    };
    for (const PdfPageView& view : views) {
        auto text = doc->textPage(0, view);
        CHECK(text.has_value());
        if (!text) continue;
        CHECK_EQ((*text)->text(), (*nativeText)->text());
        const auto& chars = (*text)->chars();
        CHECK_EQ(chars.size(), nativeChars.size());
        if (chars.size() != nativeChars.size()) continue;
        for (std::size_t i = 0; i < chars.size(); ++i) {
            if (nativeChars[i].bounds.isEmpty()) continue;
            const PdfBox user = rivet::pdf::displayRectToUserBox(native, nativeChars[i].bounds);
            const core::Rect expected = rivet::pdf::userBoxToDisplayRect(view, user);
            CHECK(nearRect(chars[i].bounds, expected, 0.05));
        }
    }
}

RIVET_TEST(pdfiumLinkRectsFollowViews) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("internal-links.pdf"));
    if (!doc) return;

    // First link: user [72 700 320 724] on 612x792.
    const PdfPageView view{core::PageRotation::Clockwise90, PdfBox{0, 0, 612, 792}};
    auto links = doc->pageLinks(0, view);
    CHECK(links.has_value());
    if (!links) return;
    CHECK_EQ(links->size(), std::size_t{2});
    if (links->empty() || (*links)[0].rects.empty()) return;
    const core::Rect expected = rivet::pdf::userBoxToDisplayRect(view, PdfBox{72, 700, 320, 724});
    CHECK(nearRect((*links)[0].rects[0], expected, 0.05));
    CHECK_EQ((*links)[0].destination.pageIndex, std::size_t{1});
}

RIVET_TEST(pdfiumDestinationCarriesUserPoint) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("outline.pdf"));
    if (!doc) return;
    auto outline = doc->outline();
    CHECK(outline.has_value() && outline->has_value());
    if (!outline || !outline->has_value()) return;
    const auto& root = **outline;
    if (root.children.empty() || root.children[0].children.empty()) {
        CHECK(false);
        return;
    }
    const auto& dest = root.children[0].children[0].destination;
    CHECK(dest.has_value());
    if (!dest) return;
    // /XYZ 72 720 on page 2.
    CHECK(dest->hasUserPoint);
    CHECK_NEAR(dest->userX, 72.0, 0.01);
    CHECK_NEAR(dest->userY, 720.0, 0.01);
    const auto remapped = rivet::pdf::userToDisplay(
        PdfPageView{core::PageRotation::Clockwise180, PdfBox{0, 0, 612, 792}}, dest->userX, dest->userY);
    CHECK_NEAR(remapped.x, 540.0, 0.01);
    CHECK_NEAR(remapped.y, 720.0, 0.01);
}

// ---------------------------------------------------------------------------
// Assembly round trips
// ---------------------------------------------------------------------------

RIVET_TEST(pdfiumAssemblyIdentityAndReorder) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("markers-5.pdf"));
    if (!base) return;

    {
        auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
        for (std::size_t i = 0; i < 5; ++i) addPage(req, *base, i);
        MemorySink sink;
        CHECK(engine->assembleDocument(req, sink).has_value());
        auto out = reload(*engine, sink);
        if (out) CHECK(markers(*out) == strings({"PAGE-1", "PAGE-2", "PAGE-3", "PAGE-4", "PAGE-5"}));
    }
    {
        auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
        for (std::size_t i : {4u, 2u, 0u, 1u, 3u}) addPage(req, *base, i);
        MemorySink sink;
        CHECK(engine->assembleDocument(req, sink).has_value());
        auto out = reload(*engine, sink);
        if (out) {
            CHECK_EQ(out->info().pageCount, std::size_t{5});
            CHECK(markers(*out) == strings({"PAGE-5", "PAGE-3", "PAGE-1", "PAGE-2", "PAGE-4"}));
        }
    }
    // The live base is untouched.
    CHECK(markers(*base) == strings({"PAGE-1", "PAGE-2", "PAGE-3", "PAGE-4", "PAGE-5"}));
}

RIVET_TEST(pdfiumAssemblyDeleteAndDuplicate) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("markers-5.pdf"));
    if (!base) return;

    {
        auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
        for (std::size_t i : {0u, 2u, 4u}) addPage(req, *base, i);
        MemorySink sink;
        CHECK(engine->assembleDocument(req, sink).has_value());
        auto out = reload(*engine, sink);
        if (out) CHECK(markers(*out) == strings({"PAGE-1", "PAGE-3", "PAGE-5"}));
    }
    {
        auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
        for (std::size_t i : {1u, 1u, 0u, 1u}) addPage(req, *base, i);
        MemorySink sink;
        CHECK(engine->assembleDocument(req, sink).has_value());
        auto out = reload(*engine, sink);
        if (out) CHECK(markers(*out) == strings({"PAGE-2", "PAGE-2", "PAGE-1", "PAGE-2"}));
    }
    CHECK_EQ(base->info().pageCount, std::size_t{5});
}

RIVET_TEST(pdfiumAssemblyImportsFromAnotherDocument) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("markers-5.pdf"));
    auto other = openPath(*engine, fixture("import-3.pdf"));
    if (!base || !other) return;

    auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    addPage(req, *base, 0);
    addPage(req, *other, 1);
    addPage(req, *base, 1);
    addPage(req, *other, 0);
    addPage(req, *other, 1);
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    auto out = reload(*engine, sink);
    if (!out) return;
    CHECK(markers(*out) == strings({"PAGE-1", "IMPORT-2", "PAGE-2", "IMPORT-1", "IMPORT-2"}));
    const auto landscape = out->pageInfo(1);
    CHECK(landscape.has_value());
    if (landscape) {
        CHECK_NEAR(landscape->sizePoints.width, 792.0, 0.01);
        CHECK_NEAR(landscape->sizePoints.height, 612.0, 0.01);
    }
    CHECK_EQ(other->info().pageCount, std::size_t{3});
}

RIVET_TEST(pdfiumAssemblyAppliesRotationAndCrop) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("markers-5.pdf"));
    auto crop = openPath(*engine, fixture("cropbox.pdf"));
    if (!base || !crop) return;

    auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    req.pages.push_back({base.get(), 0, PdfPageView{core::PageRotation::Clockwise90, PdfBox{0, 0, 612, 792}}});
    req.pages.push_back({base.get(), 1, PdfPageView{core::PageRotation::None, PdfBox{50, 80, 200, 200}}});
    req.pages.push_back({base.get(), 2, PdfPageView{core::PageRotation::Clockwise270, PdfBox{10, 20, 300, 400}}});
    // cropbox.pdf page 2 is natively /Rotate 90 with a crop box: un-rotate
    // it and widen the view to the whole media box.
    req.pages.push_back({crop.get(), 1, PdfPageView{core::PageRotation::None, PdfBox{0, 0, 612, 792}}});
    // A duplicate of base page 1 with a different view than the reused one.
    req.pages.push_back({base.get(), 1, PdfPageView{core::PageRotation::Clockwise180, PdfBox{0, 0, 612, 792}}});

    MemorySink sink;
    const auto status = engine->assembleDocument(req, sink);
    CHECK(status.has_value());
    auto out = reload(*engine, sink);
    if (!out) return;
    const auto got = markers(*out);
    CHECK_EQ(got.size(), std::size_t{5});
    if (got.size() == 5) {
        CHECK_EQ(got[0], std::string("PAGE-1"));
        CHECK_EQ(got[1], std::string("PAGE-2"));
        CHECK_EQ(got[2], std::string("PAGE-3"));
        CHECK(got[3].find("CROP") != std::string::npos);
        CHECK_EQ(got[4], std::string("PAGE-2"));
    }
    CHECK_EQ(out->info().pageCount, req.pages.size());
    for (std::size_t i = 0; i < req.pages.size() && i < out->info().pageCount; ++i) {
        const auto info = out->pageInfo(i);
        CHECK(info.has_value());
        if (!info) continue;
        CHECK(info->view.rotation == req.pages[i].view.rotation);
        CHECK(nearBox(info->view.cropBox, req.pages[i].view.cropBox));
        const core::Size expected = rivet::pdf::displaySize(req.pages[i].view);
        CHECK_NEAR(info->sizePoints.width, expected.width, 0.01);
        CHECK_NEAR(info->sizePoints.height, expected.height, 0.01);
    }
    // The saved rotated page renders like the live page through the view.
    auto live = base->renderPage(0, req.pages[0].view, fullPage(req.pages[0].view), 0.5);
    const auto savedInfo = out->pageInfo(0);
    if (live && savedInfo) {
        auto saved = out->renderPage(0, fullPage(savedInfo->view), 0.5);
        CHECK(saved.has_value());
        if (saved && saved->sizeBytes() == live->sizeBytes()) {
            CHECK(std::memcmp(saved->data(), live->data(), live->sizeBytes()) == 0);
        } else {
            CHECK(false);
        }
    }
}

RIVET_TEST(pdfiumAssemblyFreshExtract) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("markers-5.pdf"));
    auto other = openPath(*engine, fixture("import-3.pdf"));
    if (!base || !other) return;

    auto req = request(PdfAssemblyRequest::Mode::Fresh, nullptr);
    addPage(req, *base, 3);
    addPage(req, *base, 3);
    addPage(req, *other, 0);
    req.pages.push_back({base.get(), 0, PdfPageView{core::PageRotation::Clockwise180, PdfBox{0, 0, 300, 300}}});
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    auto out = reload(*engine, sink);
    if (!out) return;
    CHECK(markers(*out).size() == 4);
    const auto got = markers(*out);
    if (got.size() == 4) {
        CHECK_EQ(got[0], std::string("PAGE-4"));
        CHECK_EQ(got[1], std::string("PAGE-4"));
        CHECK_EQ(got[2], std::string("IMPORT-1"));
    }
    const auto last = out->pageInfo(3);
    CHECK(last.has_value());
    if (last) {
        CHECK(last->view.rotation == core::PageRotation::Clockwise180);
        CHECK(nearBox(last->view.cropBox, PdfBox{0, 0, 300, 300}));
    }
}

RIVET_TEST(pdfiumAssemblyEncryptedBaseRoundTrip) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("password.pdf"), "rivet");
    if (!base) return;
    CHECK_EQ(base->info().pageCount, std::size_t{4});

    auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    for (std::size_t i : {3u, 0u, 2u}) addPage(req, *base, i);
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    CHECK(!sink.bytes().empty());

    // PDFium re-encrypts with the base's security handler: the output still
    // needs the password.
    const fs::path path = uniqueTempPath("encrypted");
    writeFile(path, sink.bytes());
    auto locked = engine->openDocument(path, "");
    CHECK(!locked.has_value() && locked.error().code == core::ErrorCode::PasswordRequired);
    auto opened = engine->openDocument(path, "rivet");
    removeQuietly(path);
    CHECK(opened.has_value());
    if (!opened) return;
    CHECK(markers(**opened) == strings({"Page 4 of 4", "Page 1 of 4", "Page 3 of 4"}));
}

RIVET_TEST(pdfiumAssemblyReadsOriginalBytesAfterPathReplaced) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    const fs::path path = uniqueTempPath("replaced");
    const fs::path replacement = uniqueTempPath("replacement");
    std::error_code error;
    fs::copy_file(fixture("markers-5.pdf"), path, error);
    CHECK(!error);
    fs::copy_file(fixture("import-3.pdf"), replacement, error);
    CHECK(!error);
    auto base = openPath(*engine, path);
    if (!base) {
        removeQuietly(path);
        removeQuietly(replacement);
        return;
    }
    // Atomic replace of the path (what a save does).
    fs::rename(replacement, path, error);
    CHECK(!error);

    auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    for (std::size_t i : {4u, 0u}) addPage(req, *base, i);
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    removeQuietly(path);
    auto out = reload(*engine, sink);
    if (out) CHECK(markers(*out) == strings({"PAGE-5", "PAGE-1"}));
}

RIVET_TEST(pdfiumAssemblyRejectsBadRequests) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("markers-5.pdf"));
    if (!base) return;
    auto foreignEngine = pdfiumEngine();
    if (!foreignEngine) return;
    auto foreign = openPath(*foreignEngine, fixture("import-3.pdf"));
    if (!foreign) return;

    MemorySink sink;
    const auto invalid = core::ErrorCode::InvalidArgument;

    auto empty = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    CHECK(isCode(engine->assembleDocument(empty, sink), invalid));

    auto outOfRange = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    outOfRange.pages.push_back({base.get(), 5, nativeView(*base, 0)});
    CHECK(isCode(engine->assembleDocument(outOfRange, sink), invalid));

    auto nullSource = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    nullSource.pages.push_back({nullptr, 0, PdfPageView{}});
    CHECK(isCode(engine->assembleDocument(nullSource, sink), invalid));

    auto nullBase = request(PdfAssemblyRequest::Mode::PreserveBase, nullptr);
    addPage(nullBase, *base, 0);
    CHECK(isCode(engine->assembleDocument(nullBase, sink), invalid));

    auto foreignPage = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    addPage(foreignPage, *foreign, 0);
    CHECK(isCode(engine->assembleDocument(foreignPage, sink), invalid));

    auto foreignBase = request(PdfAssemblyRequest::Mode::PreserveBase, foreign.get());
    addPage(foreignBase, *base, 0);
    CHECK(isCode(engine->assembleDocument(foreignBase, sink), invalid));

    auto badView = request(PdfAssemblyRequest::Mode::Fresh, nullptr);
    badView.pages.push_back({base.get(), 0, PdfPageView{core::PageRotation::None, PdfBox{0, 0, 700, 792}}});
    CHECK(isCode(engine->assembleDocument(badView, sink), invalid));

    auto badRotation = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    badRotation.pages.push_back(
        {base.get(), 0, PdfPageView{static_cast<core::PageRotation>(9), PdfBox{0, 0, 612, 792}}});
    CHECK(isCode(engine->assembleDocument(badRotation, sink), invalid));

    CHECK(sink.bytes().empty()); // nothing was written for rejected requests
    CHECK_EQ(base->info().pageCount, std::size_t{5});
}

RIVET_TEST(pdfiumAssemblyReportsSinkFailures) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto base = openPath(*engine, fixture("markers-5.pdf"));
    if (!base) return;
    auto req = request(PdfAssemblyRequest::Mode::PreserveBase, base.get());
    for (std::size_t i : {1u, 0u}) addPage(req, *base, i);

    // Count the writes of a successful save to exercise the last one too.
    MemorySink counting;
    CHECK(engine->assembleDocument(req, counting).has_value());
    class CountingSink final : public rivet::pdf::IPdfByteSink {
    public:
        core::Status write(const void*, std::size_t) override {
            ++calls;
            return core::ok();
        }
        std::size_t calls = 0;
    } counter;
    CHECK(engine->assembleDocument(req, counter).has_value());
    CHECK_GT(counter.calls, std::size_t{0});

    for (const std::size_t failAt : {std::size_t{0}, counter.calls - 1}) {
        FailingSink failing(failAt, false);
        const auto status = engine->assembleDocument(req, failing);
        CHECK(isCode(status, core::ErrorCode::Io));
        FailingSink throwing(failAt, true);
        const auto thrown = engine->assembleDocument(req, throwing);
        CHECK(isCode(thrown, core::ErrorCode::Internal));
    }
    // The engine still works afterwards.
    MemorySink sink;
    CHECK(engine->assembleDocument(req, sink).has_value());
    auto out = reload(*engine, sink);
    if (out) CHECK(markers(*out) == strings({"PAGE-2", "PAGE-1"}));
}
