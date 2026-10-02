// SPDX-License-Identifier: MPL-2.0
// Opt-in perf probe (RIVET_BUILD_PERF_PROBES): wall-clock timings of the
// content-editing pipeline (Phase 5) on synthetic pages of 100 / 1000 / 10000
// content objects (70 % text words in paragraphs, 20 % images, 10 % paths)
// behind the fake backends. No assertions, no thresholds; prints a table.
// Numbers are only comparable within one build type. Not registered with
// ctest; run the executable directly. PDFium-free: it measures Rivet's own
// layer (reconstruction, resolve, hit test, command build, execute/undo).
#include "RivetTest.h"

#include "fakes/ContentTestSupport.hpp"

#include "editor/TextBlocks.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace rivet;
using namespace rivet::editor;
using namespace rivet::test;

namespace {
// Patches built field by field: GCC's -Wmissing-field-initializers rejects
// designated initializers that leave the other optionals out.
TextBlockPatch textPatch(std::string text) {
    TextBlockPatch patch;
    patch.text = std::move(text);
    return patch;
}

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// `count` objects: paragraphs of 6 two-word lines, 48 lines per column.
pdf::PdfPageContent syntheticPage(std::size_t count) {
    pdf::PdfPageContent content;
    std::size_t line = 0;
    while (content.objects.size() < count) {
        const std::size_t kind = content.objects.size() % 10;
        if (kind < 7 && content.objects.size() + 2 <= count) {
            const std::size_t column = line / 48;
            const std::size_t row = line % 48;
            const double y = 770.0 - static_cast<double>(row) * 14.0 - static_cast<double>(row / 6) * 8.0;
            addTextLine(content, y, 12.0, 10.0 + 150.0 * static_cast<double>(column));
            ++line;
        } else {
            const double base = static_cast<double>(content.objects.size() % 97);
            const double left = 20.0 + base * 5.0;
            const double bottom = 20.0 + static_cast<double>(content.objects.size() % 53) * 13.0;
            content.objects.push_back(kind < 9 ? makeImageObject(left, bottom, left + 40.0, bottom + 30.0)
                                               : makePathObject(left, bottom, left + 40.0, bottom + 30.0));
        }
    }
    return content;
}

void row(std::size_t n, const char* what, double ms, int reps = 1) {
    std::printf("%6zu objects  %-34s %10.3f ms%s\n", n, what, ms / reps, reps > 1 ? "  (mean)" : "");
}

void probe(std::size_t n) {
    // Pure functions.
    const pdf::PdfPageContent page = indexed(syntheticPage(n));
    auto t = Clock::now();
    const auto caps = classifyObjects(page);
    row(n, "classifyObjects", msSince(t));
    t = Clock::now();
    const auto blocks = reconstructTextBlocks(page, caps);
    row(n, "reconstructTextBlocks", msSince(t));
    std::printf("%6zu objects  %-34s %10zu\n", n, "(text blocks)", blocks.size());

    // Through the service.
    ContentFixture f(1);
    f.setContent(0, syntheticPage(n));
    t = Clock::now();
    const bool loaded = f.load(0);
    row(n, "extract + resolve (first view)", msSince(t));
    if (!loaded) {
        std::printf("%6zu objects  load failed\n", n);
        return;
    }
    const PageContentViewPtr view = f.content(0);

    constexpr int kHits = 200;
    std::size_t hits = 0;
    t = Clock::now();
    for (int i = 0; i < kHits; ++i) {
        const core::Point p{15.0 + static_cast<double>(i * 37 % 580), 20.0 + static_cast<double>(i * 53 % 750)};
        hits += f.session->contentService().hitTest(f.id(0), p, 3.0).has_value() ? 1U : 0U;
    }
    row(n, "hitTest", msSince(t), kHits);
    std::printf("%6zu objects  %-34s %10zu\n", n, "(hits of 200)", hits);

    // Commands: build + execute (rebuilds the view), undo. The page must be
    // reloaded between edits (factories refuse while the view is stale).
    core::ObjectId firstBlock;
    core::ObjectId firstImage;
    for (const ContentObjectView& o : view->objects) {
        if (o.type == pdf::PdfContentObjectType::Image && !firstImage) firstImage = o.id;
    }
    if (!view->blocks.empty()) firstBlock = view->blocks.front().id;

    if (static_cast<bool>(firstImage)) {
        t = Clock::now();
        auto edit = moveContent(*f.session, f.id(0), {firstImage}, core::Point{4.0, 4.0});
        row(n, "moveContent (build, 1 object)", msSince(t));
        if (edit) {
            t = Clock::now();
            (void)f.session->execute(std::move(edit->command));
            row(n, "execute move (swap + invalidate)", msSince(t));
            t = Clock::now();
            (void)f.load(0);
            row(n, "re-resolve after the edit", msSince(t));
            t = Clock::now();
            (void)f.session->undo();
            (void)f.load(0);
            row(n, "undo + re-resolve", msSince(t));
        }
    }
    if (static_cast<bool>(firstBlock)) {
        t = Clock::now();
        auto edit = editTextBlock(*f.session, f.id(0), firstBlock, textPatch(std::string("Edited text")));
        row(n, "editTextBlock (build)", msSince(t));
        if (edit) {
            (void)f.session->execute(std::move(edit->command));
            t = Clock::now();
            (void)f.load(0);
            row(n, "re-resolve after text edit", msSince(t));
            (void)f.session->undo();
            (void)f.load(0);
        }
    }
    {
        std::vector<core::ObjectId> everything;
        for (const ContentObjectView& o : view->objects) everything.push_back(o.id);
        t = Clock::now();
        auto edit = moveContent(*f.session, f.id(0), everything, core::Point{1.0, 1.0});
        row(n, "moveContent (build, all objects)", msSince(t));
        (void)edit;
    }
}

} // namespace

RIVET_TEST(perf_content_editing) {
#ifdef NDEBUG
    std::printf("content editing perf probe (release)\n");
#else
    std::printf("content editing perf probe (NON-release build: numbers are indicative only)\n");
#endif
    for (const std::size_t n : {std::size_t{100}, std::size_t{1000}, std::size_t{10000}}) probe(n);
}
