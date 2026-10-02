// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef RIVET_PDF_TEXT_FIXTURE_DIR
#define RIVET_PDF_TEXT_FIXTURE_DIR "tests/pdf/fixtures"
#endif

// Hostile / over-limit annotation inputs (Phase 4 hardening) against the
// annots-many.pdf and annots-hostile.pdf fixtures (see make_fixtures.py for
// the page layout). Reading must never crash, over-limit or malformed
// entries must degrade to opaque (editable = false, kind Other), the page
// must still render, and a save without edits must preserve them. PDFium-ON
// bodies only: every body returns early when no backend is built in.

namespace {

namespace fs = std::filesystem;
namespace core = rivet::core;
using namespace rivet::pdf;

fs::path fixture(const char* name) {
    return fs::path(RIVET_PDF_TEXT_FIXTURE_DIR) / name;
}

std::unique_ptr<PdfEngine> pdfiumEngine() {
    std::unique_ptr<PdfEngine> engine = createEngine();
    CHECK(engine != nullptr);
    if (!engine) return nullptr;
    if (!engine->isAvailable()) {
        CHECK_EQ(engine->backendName(), "none");
        return nullptr;
    }
    return engine;
}

std::unique_ptr<PdfDocument> openPath(PdfEngine& engine, const fs::path& path) {
    auto opened = engine.openDocument(path, {});
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

class MemorySink final : public IPdfByteSink {
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

fs::path uniqueTempPath() {
    static int counter = 0;
    return fs::temp_directory_path() /
           ("rivet-annot-limits-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(counter++) + ".pdf");
}

std::unique_ptr<PdfDocument> reload(PdfEngine& engine, const MemorySink& sink) {
    CHECK(!sink.bytes().empty());
    const fs::path path = uniqueTempPath();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(sink.bytes().data(), static_cast<std::streamsize>(sink.bytes().size()));
        CHECK(out.good());
    }
    auto opened = engine.openDocument(path, {});
    std::error_code ignored;
    fs::remove(path, ignored);
    CHECK(opened.has_value());
    if (!opened.has_value()) return nullptr;
    return std::move(*opened);
}

PdfPageView nativeView(const PdfDocument& document, std::size_t page) {
    const auto info = document.pageInfo(page);
    CHECK(info.has_value());
    return info.has_value() ? info->view : PdfPageView{};
}

PdfPageAnnotationsPtr readPage(const PdfDocument& document, std::size_t page) {
    const auto read = document.annotations(page);
    CHECK(read.has_value());
    return read.has_value() ? *read : nullptr;
}

// PreserveBase save of every page, no edits; `repeatPage` appends one extra
// copy of that page (a PDFium import, exercising the clone path too).
std::unique_ptr<PdfDocument> saveUnchanged(PdfEngine& engine, const PdfDocument& base,
                                           std::optional<std::size_t> repeatPage = std::nullopt) {
    PdfAssemblyRequest request;
    request.mode = PdfAssemblyRequest::Mode::PreserveBase;
    request.base = &base;
    for (std::size_t i = 0; i < base.info().pageCount; ++i) {
        request.pages.push_back(PdfAssemblyPage{&base, i, nativeView(base, i), nullptr});
    }
    if (repeatPage) {
        request.pages.push_back(PdfAssemblyPage{&base, *repeatPage, nativeView(base, *repeatPage), nullptr});
    }
    MemorySink sink;
    const auto status = engine.assembleDocument(request, sink, nullptr);
    CHECK(status.has_value());
    if (!status.has_value()) return nullptr;
    return reload(engine, sink);
}

// Number of non-white device pixels of a rendered page region.
bool renders(PdfDocument& document, std::size_t page) {
    const PdfPageView view = nativeView(document, page);
    const auto info = document.pageInfo(page);
    if (!info.has_value()) return false;
    const core::Rect tile{0, 0, 200, 200};
    const auto bitmap = document.renderPage(page, view, std::span<const std::uint32_t>{}, tile, 1.0);
    CHECK(bitmap.has_value());
    return bitmap.has_value() && bitmap->width() == 200 && bitmap->height() == 200;
}

bool isValidUtf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            extra = 1;
            code = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            code = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            code = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= text.size()) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            code = (code << 6) | (cc & 0x3F);
        }
        if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) || (extra == 3 && code < 0x10000) ||
            code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

// Compares the per-entry shape that must survive a save without edits.
void expectSameShape(const PdfPageAnnotations& a, const PdfPageAnnotations& b) {
    CHECK_EQ(a.annotsCount, b.annotsCount);
    CHECK_EQ(a.items.size(), b.items.size());
    for (std::size_t i = 0; i < std::min(a.items.size(), b.items.size()); ++i) {
        CHECK_EQ(a.items[i].editable, b.items[i].editable);
        CHECK(a.items[i].kind == b.items[i].kind);
        CHECK_EQ(a.items[i].isPopup, b.items[i].isPopup);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// More annotations than Rivet reads in detail
// ---------------------------------------------------------------------------

RIVET_TEST(annotation_limits_page_with_more_than_the_cap) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots-many.pdf"));
    if (!doc) return;

    const auto start = std::chrono::steady_clock::now();
    const auto page = readPage(*doc, 0);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!page) return;
    CHECK_EQ(page->items.size(), kMaxAnnotationsPerPage);
    CHECK_EQ(page->annotsCount, std::uint32_t{4200});
    CHECK(seconds < 30.0); // generous: Debug + sanitizers
    for (std::size_t i = 0; i < page->items.size(); ++i) {
        CHECK_EQ(page->items[i].index, static_cast<std::uint32_t>(i));
    }
    // Every entry is the same valid Square.
    CHECK(page->items.front().editable);
    CHECK(page->items.front().kind == PdfAnnotationKind::Square);
    CHECK(renders(*doc, 0));

    // A save without edits preserves the real count (the tail is opaque).
    auto saved = saveUnchanged(*engine, *doc);
    if (!saved) return;
    const auto after = readPage(*saved, 0);
    if (!after) return;
    CHECK_EQ(after->annotsCount, std::uint32_t{4200});
    CHECK_EQ(after->items.size(), kMaxAnnotationsPerPage);
    CHECK(renders(*saved, 0));
}

// ---------------------------------------------------------------------------
// String limits (page 0)
// ---------------------------------------------------------------------------

RIVET_TEST(annotation_limits_over_long_strings_are_opaque_and_preserved) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots-hostile.pdf"));
    if (!doc) return;
    CHECK_EQ(doc->info().pageCount, std::size_t{8});

    const auto page = readPage(*doc, 0);
    if (!page || page->items.size() != 5) {
        CHECK(false);
        return;
    }
    for (std::size_t i = 0; i < 3; ++i) { // over /Contents, /T, /NM limits
        CHECK(!page->items[i].editable);
        CHECK(page->items[i].kind == PdfAnnotationKind::Other);
    }
    // Exactly at the limit: editable; contents intact.
    CHECK(page->items[3].editable);
    CHECK_EQ(page->items[3].data.contents.size(), kMaxContentsBytes);
    CHECK(page->items[4].editable);
    CHECK_EQ(page->items[4].data.contents, "plain");
    CHECK_EQ(page->items[4].data.author, "Tester");
    CHECK(renders(*doc, 0));

    auto saved = saveUnchanged(*engine, *doc);
    if (!saved) return;
    const auto after = readPage(*saved, 0);
    if (!after) return;
    expectSameShape(*page, *after);
    if (after->items.size() == 5) CHECK_EQ(after->items[3].data.contents.size(), kMaxContentsBytes);
}

// ---------------------------------------------------------------------------
// Geometry limits (page 1)
// ---------------------------------------------------------------------------

RIVET_TEST(annotation_limits_quads_and_ink_over_limits_are_opaque) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots-hostile.pdf"));
    if (!doc) return;

    const auto page = readPage(*doc, 1);
    if (!page || page->items.size() != 12) {
        CHECK(false);
        return;
    }
    const auto opaque = [&](std::size_t i) {
        CHECK(!page->items[i].editable);
        CHECK(page->items[i].kind == PdfAnnotationKind::Other);
    };
    opaque(0); // 4097 quads
    CHECK(page->items[1].editable); // exactly kMaxQuadsPerAnnotation
    CHECK(page->items[1].kind == PdfAnnotationKind::Highlight);
    CHECK_EQ(page->items[1].data.quads.size(), kMaxQuadsPerAnnotation);
    opaque(2);  // odd /QuadPoints length
    // Non-numeric /QuadPoints entries: PDFium's public API coerces them to 0
    // and offers no way to see the original types, so the reader cannot tell
    // (no custom parser). Accepted as editable, but only with finite,
    // writable geometry - never a crash or garbage.
    if (page->items[3].editable) {
        CHECK(isWritableAnnotation(page->items[3].data));
        CHECK_EQ(page->items[3].data.quads.size(), std::size_t{1});
    }
    opaque(4);  // no /QuadPoints
    opaque(5);  // 10001-point stroke
    opaque(6);  // 257 strokes
    opaque(7);  // empty /InkList
    opaque(8);  // strokes are not arrays
    // A stroke of 3 numbers: PDFium keeps the one complete point and drops the
    // dangling coordinate; harmless, so it may stay editable (as one point).
    if (page->items[9].editable) {
        CHECK(isWritableAnnotation(page->items[9].data));
        CHECK_EQ(page->items[9].data.inkStrokes.size(), std::size_t{1});
        CHECK_EQ(page->items[9].data.inkStrokes[0].size(), std::size_t{1});
    }
    opaque(10); // total points over the cap
    CHECK(page->items[11].editable);
    CHECK(page->items[11].kind == PdfAnnotationKind::Ink);
    CHECK(renders(*doc, 1));

    auto saved = saveUnchanged(*engine, *doc);
    if (!saved) return;
    const auto after = readPage(*saved, 1);
    if (!after) return;
    expectSameShape(*page, *after);
    CHECK(renders(*saved, 1));
}

// ---------------------------------------------------------------------------
// Malformed /Annots (pages 2, 3, 4)
// ---------------------------------------------------------------------------

RIVET_TEST(annotation_limits_malformed_entries_degrade_to_opaque) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots-hostile.pdf"));
    if (!doc) return;

    const auto page = readPage(*doc, 2);
    if (!page) return;
    CHECK_EQ(page->annotsCount, std::uint32_t{13});
    CHECK_EQ(page->items.size(), std::size_t{13});
    if (page->items.size() != 13) return;
    // 0..4: null, integer, dangling reference, string, boolean; 5: no /Rect;
    // 7: coordinates of ~1e30; 8, 11: /Rect that is short or non-numeric.
    for (const std::size_t i : {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{4},
                                std::size_t{5}, std::size_t{7}, std::size_t{8}, std::size_t{11}}) {
        CHECK(!page->items[i].editable);
        CHECK(page->items[i].kind == PdfAnnotationKind::Other);
    }
    // Whatever the reader decides for the dictionaries, it must be safe: an
    // editable item has a finite, normalized rect inside the sane range.
    for (const PdfPageAnnotation& item : page->items) {
        if (!item.editable) continue;
        CHECK(item.data.rect.left <= item.data.rect.right);
        CHECK(item.data.rect.bottom <= item.data.rect.top);
        CHECK(isWritableAnnotation(item.data));
    }
    // An inverted /Rect is normalized, not rejected.
    CHECK(page->items[6].editable);
    CHECK(page->items[6].data.rect.left == 100.0 && page->items[6].data.rect.top == 200.0);
    // Without /Subtype or with an unknown one: opaque.
    CHECK(!page->items[9].editable);
    CHECK(!page->items[10].editable);
    // The last entry is a good Square.
    CHECK(page->items[12].editable);
    CHECK(page->items[12].kind == PdfAnnotationKind::Square);
    CHECK(renders(*doc, 2));

    auto saved = saveUnchanged(*engine, *doc);
    if (!saved) return;
    const auto after = readPage(*saved, 2);
    if (!after) return;
    expectSameShape(*page, *after);
    CHECK(renders(*saved, 2));
}

RIVET_TEST(annotation_limits_annots_that_is_not_an_array) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots-hostile.pdf"));
    if (!doc) return;
    for (const std::size_t pageIndex : {std::size_t{3}, std::size_t{4}}) {
        const auto page = readPage(*doc, pageIndex);
        if (!page) continue;
        CHECK(page->items.empty());
        CHECK_EQ(page->annotsCount, std::uint32_t{0});
        CHECK(renders(*doc, pageIndex));
    }
    auto saved = saveUnchanged(*engine, *doc);
    if (!saved) return;
    CHECK(renders(*saved, 3));
    CHECK(renders(*saved, 4));
}

// ---------------------------------------------------------------------------
// Relations and actions (pages 5, 6)
// ---------------------------------------------------------------------------

RIVET_TEST(annotation_limits_popup_loops_actions_and_widgets_are_inert) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots-hostile.pdf"));
    if (!doc) return;

    const auto page = readPage(*doc, 5);
    if (!page || page->items.size() != 9) {
        CHECK(false);
        return;
    }
    // A note whose /Popup is itself, and a popup parented to itself.
    CHECK(page->items[1].isPopup);
    CHECK(!page->items[1].editable);
    // A popup that lives on another page is not an /Annots index of this one.
    CHECK(!page->items[2].popupIndex.has_value());
    // FileAttachment, both Link actions, Widget: never editable.
    for (const std::size_t i : {std::size_t{5}, std::size_t{6}, std::size_t{7}, std::size_t{8}}) {
        CHECK(!page->items[i].editable);
        CHECK(page->items[i].kind == PdfAnnotationKind::Other);
    }
    // Any popup index that is reported must be a valid index of the page.
    for (const PdfPageAnnotation& item : page->items) {
        if (item.popupIndex) CHECK(*item.popupIndex < page->annotsCount);
    }
    CHECK(renders(*doc, 5));

    const auto foreign = readPage(*doc, 6);
    if (!foreign || foreign->items.size() != 2) {
        CHECK(false);
        return;
    }
    CHECK(foreign->items[0].isPopup);
    CHECK(!foreign->items[0].editable);
    CHECK(renders(*doc, 6));

    // Saving (in place, plus a PDFium import of the loop page) preserves the
    // entries and runs nothing.
    auto saved = saveUnchanged(*engine, *doc, std::size_t{5});
    if (!saved) return;
    CHECK_EQ(saved->info().pageCount, std::size_t{9});
    const auto kept = readPage(*saved, 5);
    if (kept) expectSameShape(*page, *kept);
    const auto copy = readPage(*saved, 8);
    if (copy) {
        CHECK_EQ(copy->annotsCount, page->annotsCount);
        for (const std::size_t i : {std::size_t{5}, std::size_t{6}, std::size_t{7}, std::size_t{8}}) {
            if (i < copy->items.size()) CHECK(!copy->items[i].editable);
        }
    }
    CHECK(renders(*saved, 5));
    CHECK(renders(*saved, 8));
}

// ---------------------------------------------------------------------------
// Text encoding (page 7)
// ---------------------------------------------------------------------------

RIVET_TEST(annotation_limits_unpaired_surrogates_read_as_replacement_characters) {
    auto engine = pdfiumEngine();
    if (!engine) return;
    auto doc = openPath(*engine, fixture("annots-hostile.pdf"));
    if (!doc) return;
    const auto page = readPage(*doc, 7);
    if (!page || page->items.size() != 3) {
        CHECK(false);
        return;
    }
    for (const PdfPageAnnotation& item : page->items) {
        CHECK(item.editable);
        CHECK(isValidUtf8(item.data.contents));
        CHECK(isWritableAnnotation(item.data));
    }
    const std::string fffd = "\xEF\xBF\xBD";
    CHECK_EQ(page->items[0].data.contents, "A" + fffd + "B");
    CHECK_EQ(page->items[1].data.contents, "C" + fffd + "D");
    CHECK_EQ(page->items[2].data.contents, "E\xF0\x9F\x98\x80" "F"); // supplementary pair survives

    // The repaired text round-trips through a save.
    auto saved = saveUnchanged(*engine, *doc);
    if (!saved) return;
    const auto after = readPage(*saved, 7);
    if (!after || after->items.size() != 3) return;
    CHECK_EQ(after->items[0].data.contents, page->items[0].data.contents);
    CHECK_EQ(after->items[2].data.contents, page->items[2].data.contents);
}
