// SPDX-License-Identifier: MPL-2.0
// Opt-in perf probe (RIVET_BUILD_PERF_PROBES, PDFium builds): wall-clock
// timings of the content-editing pipeline (Phase 5) through the REAL engine
// on generated pages of ~100 and ~1000 content objects (rectangles plus text
// lines; tests/pdf/PdfFixtures.hpp denseContentPdf): first extraction +
// reconstruction, hit testing, command build/execute and re-extraction after
// an edit, undo, a render with the edits, and the save assembly. No
// assertions, no thresholds; prints a table. Numbers are only comparable
// within one build type (the header line says which). Not registered with
// ctest.
#include "PdfFixtures.hpp"

#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/ContentCommands.hpp"
#include "editor/ContentObjects.hpp"
#include "editor/ContentService.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfSystem.hpp"

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace ed = rivet::editor;
namespace pdf = rivet::pdf;
namespace core = rivet::core;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

void row(const char* what, std::size_t objects, double t, const char* note = "") {
    std::printf("%-46s ~%-5zu objects %10.3f ms  %s\n", what, objects, t, note);
}

// Polls the lazily extracted content of the page until it is loaded.
ed::PageContentViewPtr awaitLoaded(ed::DocumentSession& session, core::PageId page) {
    const auto deadline = Clock::now() + std::chrono::seconds(120);
    while (Clock::now() < deadline) {
        auto view = session.contentService().content(page);
        if (view != nullptr && view->loaded) return view;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return nullptr;
}

void probe(pdf::PdfEngine& engine, core::TaskScheduler& scheduler, const fs::path& dir, std::size_t count) {
    const fs::path path = dir / ("dense-" + std::to_string(count) + ".pdf");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        const std::string bytes = rivet::test::pdffix::denseContentPdf(count);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    auto t = Clock::now();
    auto created = ed::DocumentSession::create(engine, scheduler, nullptr, path);
    if (!created.has_value()) {
        std::puts("open failed");
        return;
    }
    auto session = std::move(*created);
    row("open (session create)", count, ms(t));
    const core::PageId page = session->pageId(0);

    t = Clock::now();
    auto view = awaitLoaded(*session, page);
    if (view == nullptr) {
        std::puts("content load TIMEOUT");
        return;
    }
    const std::size_t objects = view->objects.size();
    char note[80];
    std::snprintf(note, sizeof note, "(%zu objects, %zu blocks)", objects, view->blocks.size());
    row("first extraction + reconstruction + resolve", objects, ms(t), note);

    // Hit testing across the page (display points).
    const auto info = session->documentPtr()->pageInfo(0);
    const core::Size size = info.has_value() ? pdf::displaySize(info->view) : core::Size{520.0, 600.0};
    const int hits = 2000;
    std::size_t found = 0;
    t = Clock::now();
    for (int i = 0; i < hits; ++i) {
        const core::Point at{10.0 + static_cast<double>(i % 50) * (size.width - 20.0) / 50.0,
                             10.0 + static_cast<double>((i / 50) % 40) * (size.height - 20.0) / 40.0};
        if (session->contentService().hitTest(page, at, 3.0).has_value()) ++found;
    }
    std::snprintf(note, sizeof note, "(mean per hit, %zu/%d hit)", found, hits);
    row("hitTest", objects, ms(t) / hits, note);

    // Move one rectangle: build + execute, then the re-extraction.
    core::ObjectId target;
    for (const auto& object : view->objects) {
        if (object.type == pdf::PdfContentObjectType::Path) {
            target = object.id;
            break;
        }
    }
    t = Clock::now();
    auto move = ed::moveContent(*session, page, {target}, core::Point{12.0, 6.0});
    row("moveContent (build command)", objects, ms(t));
    if (!move.has_value()) {
        std::puts("move refused");
        return;
    }
    t = Clock::now();
    const bool executed = session->execute(std::move(move->command)).has_value();
    row("execute (model swap)", objects, ms(t), executed ? "" : "FAILED");
    t = Clock::now();
    view = awaitLoaded(*session, page);
    row("re-extraction + reconstruction after the edit", objects, ms(t), view != nullptr ? "" : "TIMEOUT");

    // Edit one text block.
    if (view != nullptr && !view->blocks.empty()) {
        t = Clock::now();
        auto edit = ed::editTextBlock(*session, page, view->blocks.front().id,
                                      ed::TextBlockPatch{.text = std::string("Edited text")});
        row("editTextBlock (build command)", objects, ms(t), edit.has_value() ? "" : "REFUSED");
        if (edit.has_value()) {
            session->execute(std::move(edit->command));
            t = Clock::now();
            view = awaitLoaded(*session, page);
            row("re-extraction after a text edit", objects, ms(t));
        }
    }

    // Undo both.
    t = Clock::now();
    session->undo();
    session->undo();
    row("undo x2 (model swap)", objects, ms(t));
    t = Clock::now();
    view = awaitLoaded(*session, page);
    row("re-extraction after undo", objects, ms(t));
    session->redo();
    session->redo();
    view = awaitLoaded(*session, page);

    // Render the edited page.
    const ed::PageEntry* entry = session->pageSnapshot()->find(page);
    if (entry != nullptr && info.has_value()) {
        const int reps = 3;
        t = Clock::now();
        for (int i = 0; i < reps; ++i) {
            auto bitmap = session->documentPtr()->renderPage(0, info->view, {}, entry->contentEdits,
                                                              core::Rect(0.0, 0.0, size.width, size.height), 1.0);
            if (!bitmap.has_value()) std::puts("render failed");
        }
        row("render with content edits (mean, scale 1)", objects, ms(t) / reps);
    }

    // Save: assemble + write + reopen + rebase report.
    const fs::path out = dir / ("saved-" + std::to_string(count) + ".pdf");
    auto job = ed::makeSaveJob(*session, out);
    if (!job.has_value()) {
        std::puts("makeSaveJob failed");
        return;
    }
    const auto result = ed::runDocumentWrite(engine, *job);
    std::snprintf(note, sizeof note, "(%llu bytes, written=%s)", static_cast<unsigned long long>(result.bytesWritten),
                  result.written.has_value() ? "ok" : "FAIL");
    row("save: assemble + write (runDocumentWrite)", objects, std::chrono::duration<double, std::milli>(result.elapsed).count(),
        note);
    std::puts("");
}

} // namespace

int main() {
    auto engine = pdf::createEngine();
    if (!engine || !engine->isAvailable()) {
        std::puts("no PDFium");
        return 2;
    }
#ifdef NDEBUG
    const char* build = "optimized (NDEBUG)";
#else
    const char* build = "unoptimized (assertions on)";
#endif
    std::printf("Rivet content perf probe (PDFium) - build: %s\n\n", build);
    core::TaskScheduler scheduler(2);
    const fs::path dir = fs::temp_directory_path() / ("rivet-perf-content-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    for (const std::size_t count : {std::size_t{100}, std::size_t{1000}}) probe(*engine, scheduler, dir, count);
    std::error_code ignored;
    fs::remove_all(dir, ignored);
    return 0;
}
