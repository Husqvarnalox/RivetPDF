// SPDX-License-Identifier: MPL-2.0
// Opt-in perf probe (RIVET_BUILD_PERF_PROBES): wall-clock timings of the
// annotation pipeline (Phase 4) on documents generated in-process through the
// assembly (a blank page of markers-5.pdf + annotation edits). No assertions,
// no thresholds; prints a table. Numbers are only comparable within one build
// type (the header line says which).
//   rivet_perf_annotations [fixture-dir]
#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/AnnotationCommands.hpp"
#include "editor/AnnotationGeometry.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <unistd.h>

#include <chrono>
#include <cmath>
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
#include <vector>

#ifndef RIVET_PERF_FIXTURE_DIR
#define RIVET_PERF_FIXTURE_DIR "tests/pdf/fixtures"
#endif

namespace ed = rivet::editor;
namespace pdf = rivet::pdf;
namespace core = rivet::core;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

double ms(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

void row(const char* what, std::size_t n, double t, const char* note = "") {
    std::printf("%-44s N=%-6zu %10.3f ms  %s\n", what, n, t, note);
}

class MemorySink final : public pdf::IPdfByteSink {
public:
    core::Status write(const void* data, std::size_t size) override {
        bytes.append(static_cast<const char*>(data), size);
        return core::ok();
    }
    std::string bytes;
};

// Deterministic pseudo-random numbers in [0, 1).
class Lcg {
public:
    double next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(state_ >> 11) / 9007199254740992.0;
    }

private:
    std::uint64_t state_ = 0x9E3779B97F4A7C15ULL;
};

// The i-th annotation of a page: a mix of the writable kinds, laid out on a
// grid inside a 612x792 page (user space).
pdf::PdfAnnotationData sample(std::size_t i) {
    const double x = 10.0 + static_cast<double>(i % 20) * 28.0;
    const double y = 10.0 + static_cast<double>((i / 20) % 50) * 15.0;
    pdf::PdfAnnotationData d;
    switch (i % 5) {
    case 0:
        d.kind = pdf::PdfAnnotationKind::Square;
        d.rect = pdf::PdfBox{x, y, x + 20, y + 10};
        break;
    case 1:
        d.kind = pdf::PdfAnnotationKind::Circle;
        d.rect = pdf::PdfBox{x, y, x + 20, y + 10};
        break;
    case 2:
        d.kind = pdf::PdfAnnotationKind::Highlight;
        d.quads = {pdf::PdfQuad{{x, y + 10}, {x + 20, y + 10}, {x, y}, {x + 20, y}}};
        break;
    case 3:
        d.kind = pdf::PdfAnnotationKind::Note;
        d.rect = pdf::PdfBox{x, y, x + 20, y + 20};
        d.contents = "note";
        break;
    default: {
        d.kind = pdf::PdfAnnotationKind::Ink;
        std::vector<pdf::PdfPoint> stroke;
        for (int k = 0; k < 8; ++k) stroke.push_back(pdf::PdfPoint{x + k * 2.0, y + (k % 2) * 8.0});
        d.inkStrokes = {std::move(stroke)};
        break;
    }
    }
    pdf::normalizeAnnotation(d);
    return d;
}

struct Context {
    pdf::PdfEngine& engine;
    core::TaskScheduler& scheduler;
    fs::path fixture;
    fs::path dir;
};

// Writes a document of `pages` copies of page 0 of the fixture, each with
// `perPage` generated annotations, to `path`; returns the generation time.
double generate(Context& ctx, std::size_t pages, std::size_t perPage, const fs::path& path) {
    auto opened = ctx.engine.openDocument(ctx.fixture, {});
    if (!opened.has_value()) {
        std::puts("fixture open failed");
        return -1.0;
    }
    const std::shared_ptr<pdf::PdfDocument> base = std::move(*opened);
    const auto info = base->pageInfo(0);
    if (!info.has_value()) return -1.0;
    pdf::PdfAssemblyRequest request;
    request.mode = pdf::PdfAssemblyRequest::Mode::PreserveBase;
    request.base = base.get();
    for (std::size_t p = 0; p < pages; ++p) {
        std::shared_ptr<pdf::PdfPageAnnotationEdits> edits;
        if (perPage > 0) {
            edits = std::make_shared<pdf::PdfPageAnnotationEdits>();
            for (std::size_t i = 0; i < perPage; ++i) edits->create.push_back(sample(i));
        }
        request.pages.push_back(pdf::PdfAssemblyPage{base.get(), 0, info->view, std::move(edits)});
    }
    const auto start = Clock::now();
    MemorySink sink;
    if (!ctx.engine.assembleDocument(request, sink).has_value()) {
        std::puts("assembly failed");
        return -1.0;
    }
    const double t = ms(start);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(sink.bytes.data(), static_cast<std::streamsize>(sink.bytes.size()));
    return t;
}

std::unique_ptr<ed::DocumentSession> open(Context& ctx, const fs::path& path) {
    auto created = ed::DocumentSession::create(ctx.engine, ctx.scheduler, nullptr, path);
    if (!created.has_value()) {
        std::puts("open failed");
        return nullptr;
    }
    return std::move(*created);
}

bool waitCached(ed::DocumentSession& session, std::size_t pages) {
    const auto deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < deadline) {
        if (session.annotations().cachedOriginalPages() >= pages) return true;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
}

ed::AnnotationDraft squareDraft(core::PageId page, std::size_t i) {
    ed::AnnotationDraft d;
    d.page = page;
    d.kind = pdf::PdfAnnotationKind::Square;
    d.rect = core::Rect{10.0 + static_cast<double>(i % 20) * 28.0, 10.0 + static_cast<double>((i / 20) % 50) * 15.0, 20, 10};
    d.style.color = pdf::PdfColor{0.2F, 0.4F, 0.8F};
    d.style.opacity = 1.0F;
    d.style.borderWidth = 2.0F;
    return d;
}

void readProbes(Context& ctx) {
    for (const std::size_t n : {std::size_t{10}, std::size_t{100}, std::size_t{1000}}) {
        const fs::path path = ctx.dir / ("read-" + std::to_string(n) + ".pdf");
        const double gen = generate(ctx, 1, n, path);
        row("generate 1 page (assembly, in memory)", n, gen);

        auto opened = ctx.engine.openDocument(path, {});
        if (!opened.has_value()) continue;
        auto t = Clock::now();
        const auto first = (*opened)->annotations(0);
        row("PdfDocument::annotations first read", n, ms(t));
        t = Clock::now();
        const auto second = (*opened)->annotations(0);
        row("PdfDocument::annotations second read", n, ms(t), "(cached)");
        if (!first.has_value()) continue;
        std::size_t editable = 0;
        for (const pdf::PdfPageAnnotation& item : (*first)->items) editable += item.editable ? 1 : 0;

        t = Clock::now();
        std::size_t streamBytes = 0;
        for (const pdf::PdfPageAnnotation& item : (*first)->items) {
            if (!item.editable) continue;
            streamBytes += pdf::appearanceContentStream(pdf::buildAppearance(item.data)).size();
        }
        row("buildAppearance + content stream, all", editable, ms(t));
        (void)streamBytes;

        auto session = open(ctx, path);
        if (!session) continue;
        const core::PageId page = session->pageId(0);
        t = Clock::now();
        session->annotations().annotations(page);
        const bool loaded = waitCached(*session, 1);
        row("service first request -> originals cached", n, ms(t), loaded ? "" : "TIMEOUT");
        t = Clock::now();
        const auto resolved = session->annotations().annotations(page);
        row("service resolve (originals cached, 1st)", resolved->size(), ms(t));
        t = Clock::now();
        const auto again = session->annotations().annotations(page);
        row("service resolve (cached resolution)", again->size(), ms(t));

        Lcg rng;
        std::size_t hits = 0;
        t = Clock::now();
        for (int i = 0; i < 1000; ++i) {
            const core::Point p{rng.next() * 612.0, rng.next() * 792.0};
            if (session->annotations().hitTest(page, p, 3.0)) ++hits;
        }
        char note[48];
        std::snprintf(note, sizeof note, "(%zu hits)", hits);
        row("hitTest x1000 random points", n, ms(t), note);
    }
}

void commandProbes(Context& ctx) {
    const fs::path path = ctx.dir / "commands.pdf";
    generate(ctx, 1, 0, path);
    auto session = open(ctx, path);
    if (!session) return;
    const core::PageId page = session->pageId(0);

    // One command creating 100 annotations.
    std::vector<ed::AnnotationDraft> drafts;
    for (std::size_t i = 0; i < 100; ++i) drafts.push_back(squareDraft(page, i));
    auto t = Clock::now();
    auto edit = ed::createAnnotations(*session, std::move(drafts));
    if (!edit.has_value() || !session->execute(std::move(edit->command)).has_value()) {
        std::puts("create failed");
        return;
    }
    row("create 100 (one command): execute", 100, ms(t));
    t = Clock::now();
    session->undo();
    row("create 100 (one command): undo", 100, ms(t));
    t = Clock::now();
    session->redo();
    row("create 100 (one command): redo", 100, ms(t));

    // Delete them all in one command.
    std::vector<core::AnnotationId> ids;
    for (const ed::AnnotationView& view : *session->annotations().annotations(page)) ids.push_back(view.id);
    t = Clock::now();
    auto del = ed::deleteAnnotations(*session, ids);
    if (!del.has_value() || !session->execute(std::move(del->command)).has_value()) {
        std::puts("delete failed");
        return;
    }
    row("delete 100 (one command): execute", ids.size(), ms(t));
    t = Clock::now();
    session->undo();
    row("delete 100 (one command): undo", ids.size(), ms(t));
    t = Clock::now();
    session->redo();
    row("delete 100 (one command): redo", ids.size(), ms(t));
    session->undo();
    session->undo();

    // 100 commands of one annotation each.
    t = Clock::now();
    for (std::size_t i = 0; i < 100; ++i) {
        auto one = ed::createAnnotations(*session, {squareDraft(page, i)});
        if (!one.has_value() || !session->execute(std::move(one->command)).has_value()) {
            std::puts("create failed");
            return;
        }
    }
    row("create 100 (100 commands): execute total", 100, ms(t));
    t = Clock::now();
    for (int i = 0; i < 100; ++i) session->undo();
    row("create 100 (100 commands): undo total", 100, ms(t));
    t = Clock::now();
    for (int i = 0; i < 100; ++i) session->redo();
    row("create 100 (100 commands): redo total", 100, ms(t));
}

void saveProbe(Context& ctx) {
    const fs::path path = ctx.dir / "save100.pdf";
    row("generate 100 blank pages (assembly)", 100, generate(ctx, 100, 0, path));
    auto session = open(ctx, path);
    if (!session) return;
    std::vector<ed::AnnotationDraft> drafts;
    for (std::size_t p = 0; p < session->pageCount(); ++p) {
        for (std::size_t i = 0; i < 10; ++i) drafts.push_back(squareDraft(session->pageId(p), i));
    }
    auto t = Clock::now();
    auto edit = ed::createAnnotations(*session, std::move(drafts));
    if (!edit.has_value() || !session->execute(std::move(edit->command)).has_value()) {
        std::puts("create failed");
        return;
    }
    row("create 10 on each of 100 pages: execute", 1000, ms(t));

    const fs::path out = ctx.dir / "saved100.pdf";
    t = Clock::now();
    auto job = ed::makeSaveJob(*session, out);
    row("makeSaveJob (snapshot + request)", 100, ms(t));
    if (!job.has_value()) return;
    const auto result = ed::runDocumentWrite(ctx.engine, *job);
    char note[64];
    std::snprintf(note, sizeof note, "(%llu bytes, written=%s, rebase=%s)",
                  static_cast<unsigned long long>(result.bytesWritten), result.written.has_value() ? "ok" : "FAIL",
                  (result.rebase && result.rebase->has_value()) ? "ok" : "none");
    row("runDocumentWrite (assemble+write+reopen)", 1000,
        std::chrono::duration<double, std::milli>(result.elapsed).count(), note);

    t = Clock::now();
    auto reopened = open(ctx, out);
    row("reopen saved file (session create)", 100, ms(t));
    if (!reopened) return;
    t = Clock::now();
    for (std::size_t p = 0; p < reopened->pageCount(); ++p) reopened->annotations().annotations(reopened->pageId(p));
    const bool loaded = waitCached(*reopened, reopened->pageCount());
    row("load originals of all 100 pages (worker)", 1000, ms(t), loaded ? "" : "TIMEOUT");
}

void inkProbes(Context& ctx) {
    auto opened = ctx.engine.openDocument(ctx.fixture, {});
    if (!opened.has_value()) return;
    const std::shared_ptr<pdf::PdfDocument> base = std::move(*opened);
    const auto info = base->pageInfo(0);
    if (!info.has_value()) return;
    for (const std::size_t points : {std::size_t{100}, std::size_t{1000}, std::size_t{10000}}) {
        pdf::PdfAnnotationData d;
        d.kind = pdf::PdfAnnotationKind::Ink;
        std::vector<pdf::PdfPoint> stroke;
        for (std::size_t i = 0; i < points; ++i) {
            const double s = static_cast<double>(i) / static_cast<double>(points);
            stroke.push_back(pdf::PdfPoint{20.0 + 560.0 * s, 400.0 + 150.0 * std::sin(s * 40.0)});
        }
        d.inkStrokes = {std::move(stroke)};
        d.borderWidth = 2.0F;
        pdf::normalizeAnnotation(d);

        auto t = Clock::now();
        const pdf::PdfAppearance appearance = pdf::buildAppearance(d);
        row("ink buildAppearance", points, ms(t));
        t = Clock::now();
        const std::string stream = pdf::appearanceContentStream(appearance);
        char note[48];
        std::snprintf(note, sizeof note, "(%zu bytes)", stream.size());
        row("ink appearanceContentStream", points, ms(t), note);

        pdf::PdfAssemblyRequest request;
        request.mode = pdf::PdfAssemblyRequest::Mode::PreserveBase;
        request.base = base.get();
        auto edits = std::make_shared<pdf::PdfPageAnnotationEdits>();
        edits->create.push_back(d);
        request.pages.push_back(pdf::PdfAssemblyPage{base.get(), 0, info->view, std::move(edits)});
        MemorySink sink;
        t = Clock::now();
        const bool ok = ctx.engine.assembleDocument(request, sink).has_value();
        row("ink save (assembly to memory)", points, ms(t), ok ? "" : "FAILED");
    }

    // Stroke simplification: a smooth wave and the adversarial zigzag.
    for (const bool zigzag : {false, true}) {
        std::vector<core::Point> points;
        for (std::size_t i = 0; i < 10000; ++i) {
            const double x = static_cast<double>(i) * 0.05;
            points.push_back(core::Point{x, zigzag ? ((i % 2 == 0) ? 0.0 : 3.0) : 40.0 * std::sin(x / 20.0)});
        }
        const auto t = Clock::now();
        const auto reduced = ed::geometry::reduceStroke(points, zigzag ? 1.0 : 0.5);
        char note[64];
        std::snprintf(note, sizeof note, "(%zu points kept)", reduced.size());
        row(zigzag ? "reduceStroke (zigzag, worst case)" : "reduceStroke (smooth wave)", points.size(), ms(t), note);
    }
}

} // namespace

int main(int argc, char** argv) {
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
    std::printf("Rivet annotation perf probe - build: %s\n", build);
    core::TaskScheduler scheduler(2);
    const fs::path dir = fs::temp_directory_path() / ("rivet-perf-annot-" + std::to_string(::getpid()));
    fs::create_directories(dir);
    const fs::path fixtures = argc > 1 ? fs::path(argv[1]) : fs::path(RIVET_PERF_FIXTURE_DIR);
    Context ctx{*engine, scheduler, fixtures / "markers-5.pdf", dir};

    readProbes(ctx);
    commandProbes(ctx);
    saveProbe(ctx);
    inkProbes(ctx);

    std::error_code ignored;
    fs::remove_all(dir, ignored);
    return 0;
}
