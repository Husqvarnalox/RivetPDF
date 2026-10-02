// SPDX-License-Identifier: MPL-2.0
// Opt-in perf probe (RIVET_BUILD_PERF_PROBES): wall-clock timings of Phase 3
// page editing on generated PDFs. No assertions; prints a table.
//   rivet_perf_page_editing model <pdf>         model ops on an existing doc
//   rivet_perf_page_editing import <pdf> <src>  import all pages of src
//   rivet_perf_page_editing save <pdf>          save path + peak RSS
//   rivet_perf_page_editing extract <pdf>       extract 1 / 10 / 100 pages
//   rivet_perf_page_editing split <pdf> <per>   split into ranges of <per>
//                                               pages (FileController path)
#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/PageCommands.hpp"
#include "editor/PageModel.hpp"
#include "editor/PageRangeParser.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ed = rivet::editor;
namespace core = rivet::core;
using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point a) {
    return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
}
static double rssMb() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_maxrss) / (1024.0 * 1024.0); // bytes on macOS
}
static std::uintmax_t fileSize(const std::filesystem::path& p) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    return ec ? 0 : size;
}
static void row(const char* op, std::size_t n, std::size_t k, const char* phase, double t) {
    std::printf("%-10s N=%-5zu k=%-5zu %-5s %9.3f ms\n", op, n, k, phase, t);
}

template <typename F>
static void timed(ed::DocumentSession& s, const char* op, std::size_t k, F make) {
    const std::size_t n = s.pageCount();
    auto t = Clock::now();
    auto cmd = make();
    auto st = s.execute(std::move(cmd));
    double e = ms(t);
    if (!st.has_value()) { std::printf("%s FAILED: %s\n", op, st.error().message.c_str()); return; }
    row(op, n, k, "do", e);
    t = Clock::now(); s.undo(); row(op, n, k, "undo", ms(t));
    t = Clock::now(); s.redo(); row(op, n, k, "redo", ms(t));
    s.undo();
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[1];
    auto engine = rivet::pdf::createEngine();
    if (!engine->isAvailable()) { std::puts("no PDFium"); return 2; }
    core::TaskScheduler scheduler(2);
    auto t = Clock::now();
    auto created = ed::DocumentSession::create(*engine, scheduler, nullptr, argv[2]);
    if (!created.has_value()) { std::puts("open failed"); return 1; }
    ed::DocumentSession& s = **created;
    const std::size_t n = s.pageCount();
    row("open", n, 0, "", ms(t));
    std::printf("rss after open %.1f MB\n", rssMb());

    if (mode == "model") {
        auto ids = [&](std::size_t k, std::size_t stride) {
            std::vector<core::PageId> v;
            for (std::size_t i = 0; i < k; ++i) v.push_back(s.pageId((i * stride) % n));
            return v;
        };
        auto& m = s.pageModel();
        for (std::size_t k : {std::size_t{1}, std::size_t{100}, n / 4}) {
            if (k == 0 || k >= n) continue;
            timed(s, "move", k, [&] { return std::make_unique<ed::MovePagesCommand>(m, ids(k, 2), n - k); });
            timed(s, "delete", k, [&] { return std::make_unique<ed::DeletePagesCommand>(m, ids(k, 2)); });
            timed(s, "rotate", k, [&] { return std::make_unique<ed::RotatePagesCommand>(m, ids(k, 2), 90); });
            timed(s, "duplicate", k, [&] { return std::make_unique<ed::DuplicatePagesCommand>(m, ids(k, 2)); });
            const auto box = rivet::pdf::PdfBox{50, 50, 500, 700};
            timed(s, "crop", k, [&] { return std::make_unique<ed::CropPagesCommand>(m, ids(k, 2), box); });
        }
    } else if (mode == "import") {
        auto src = engine->openDocument(argv[3]);
        if (!src.has_value()) { std::puts("src open failed"); return 1; }
        std::shared_ptr<rivet::pdf::PdfDocument> other = std::move(*src);
        t = Clock::now();
        auto pages = ed::PageModel::describeAllPages(other);
        row("describe", n, other->info().pageCount, "", ms(t));
        const std::size_t k = pages->size();
        timed(s, "import", k, [&] { return std::make_unique<ed::InsertPagesCommand>(s.pageModel(), *pages, n); });
        s.redo();
        const auto dest = std::filesystem::path(argv[2]).replace_extension(".saved.pdf");
        t = Clock::now();
        auto job = ed::makeSaveJob(s, dest);
        auto r = ed::runDocumentWrite(*engine, *job);
        row("save+imp", s.pageCount(), k, "", ms(t));
        std::printf("  ok=%d\n", r.written.has_value());
        std::filesystem::remove(dest);
    } else if (mode == "save") {
        // edit a bit so the save reorders/rotates/duplicates
        std::vector<core::PageId> ids;
        for (std::size_t i = 0; i < n; i += 2) ids.push_back(s.pageId(i));
        (void)s.execute(std::make_unique<ed::RotatePagesCommand>(s.pageModel(), ids, 90));
        (void)s.execute(std::make_unique<ed::MovePagesCommand>(s.pageModel(), ids, n - ids.size()));
        const auto dest = std::filesystem::path(argv[2]).replace_extension(".saved.pdf");
        for (int pass = 0; pass < 2; ++pass) {
            t = Clock::now();
            auto job = ed::makeSaveJob(s, dest);
            row("savejob", n, 0, "", ms(t));
            t = Clock::now();
            auto r = ed::runDocumentWrite(*engine, *job);
            row("save", n, 0, pass ? "warm" : "cold", ms(t));
            std::printf("  ok=%d bytes=%llu rebase=%d\n", r.written.has_value(),
                        static_cast<unsigned long long>(r.bytesWritten), r.rebase.has_value());
        }
        std::filesystem::remove(dest);
    } else if (mode == "extract") {
        // Same path as File > Extract: main-thread job capture, worker write.
        const auto dest = std::filesystem::path(argv[2]).replace_extension(".extract.pdf");
        for (std::size_t k : {std::size_t{1}, std::size_t{10}, std::size_t{100}}) {
            if (k > n) continue;
            std::vector<core::PageId> ids;
            for (std::size_t i = 0; i < k; ++i) ids.push_back(s.pageId((i * 3) % n));
            t = Clock::now();
            auto job = ed::makeExtractJob(s, ids, dest);
            const double capture = ms(t);
            if (!job.has_value()) { std::puts("extract job failed"); return 1; }
            t = Clock::now();
            auto r = ed::runDocumentWrite(*engine, *job);
            const double write = ms(t);
            row("extract", n, k, "job", capture);
            row("extract", n, k, "write", write);
            std::printf("  ok=%d bytes=%llu rss=%.1f MB\n", r.written.has_value(),
                        static_cast<unsigned long long>(fileSize(dest)), rssMb());
            std::filesystem::remove(dest);
        }
    } else if (mode == "split") {
        // Mirrors FileController::splitByRanges: parse, capture every job on
        // the main thread, then write them sequentially (one worker task).
        const std::size_t per = argc > 3 ? std::stoul(argv[3]) : 10;
        if (per == 0) return 2;
        std::string text;
        for (std::size_t first = 1; first <= n; first += per) {
            if (!text.empty()) text += ",";
            const std::size_t last = std::min(n, first + per - 1);
            text += first == last ? std::to_string(first) : std::to_string(first) + "-" + std::to_string(last);
        }
        const auto outDir = std::filesystem::path(argv[2]).parent_path() / "split-probe";
        std::filesystem::remove_all(outDir);
        std::filesystem::create_directories(outDir);
        const auto total = Clock::now();
        t = Clock::now();
        auto ranges = ed::parsePageRanges(text, n);
        if (!ranges.has_value()) { std::puts("parse failed"); return 1; }
        const auto outputs = ed::splitOutputPaths(outDir / "report.pdf", *ranges);
        std::vector<ed::DocumentWriteJob> jobs;
        for (std::size_t i = 0; i < ranges->size(); ++i) {
            std::vector<core::PageId> pages;
            for (std::size_t p = (*ranges)[i].first; p <= (*ranges)[i].last; ++p) pages.push_back(s.pageId(p - 1));
            auto job = ed::makeExtractJob(s, pages, outputs[i]);
            if (!job.has_value()) { std::puts("split job failed"); return 1; }
            jobs.push_back(std::move(*job));
        }
        row("split", n, jobs.size(), "jobs", ms(t));
        t = Clock::now();
        std::size_t written = 0;
        for (auto& job : jobs) written += ed::runDocumentWrite(*engine, job).written.has_value() ? 1 : 0;
        jobs.clear();
        row("split", n, outputs.size(), "write", ms(t));
        row("split", n, outputs.size(), "total", ms(total));
        std::uintmax_t bytes = 0, minB = UINTMAX_MAX, maxB = 0;
        for (const auto& o : outputs) {
            const auto b = fileSize(o);
            bytes += b;
            minB = std::min(minB, b);
            maxB = std::max(maxB, b);
        }
        std::printf("  outputs=%zu/%zu bytes total=%llu min=%llu max=%llu\n", written, outputs.size(),
                    static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(minB),
                    static_cast<unsigned long long>(maxB));
        std::filesystem::remove_all(outDir);
    }
    std::printf("peak rss %.1f MB\n", rssMb());
    return 0;
}
