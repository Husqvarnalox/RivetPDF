// SPDX-License-Identifier: MPL-2.0
// Opt-in perf probe: RIVET_PERF_MARKDOWN=1. Prints timings only (no content).
#include "MarkdownTestKit.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace rivet::markdown;
using namespace rivet::markdown::testkit;

int main() {
    const char* gate = std::getenv("RIVET_PERF_MARKDOWN");
    if (gate == nullptr || gate[0] != '1') {
        std::puts("rivet_perf_markdown: skipped (set RIVET_PERF_MARKDOWN=1)");
        return 0;
    }
    using clock = std::chrono::steady_clock;
    const auto ms = [](clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    for (const std::size_t kb : {10u, 100u, 1024u, 5120u}) {
        const std::string src = generateMarkdown(kb * 1024);
        const auto t0 = clock::now();
        const MarkdownDocument doc = parse(src);
        const auto t1 = clock::now();
        FixedMeasurer m;
        MeasureCache cache;
        const MarkdownLayout l1 = layoutMarkdown(doc, 800, m, nullptr, {}, &cache);
        const auto t2 = clock::now();
        const MarkdownLayout l2 = layoutMarkdown(doc, 640, m, nullptr, {}, &cache);
        const auto t3 = clock::now();
        const PlainText pt = extractPlainText(doc);
        const auto t4 = clock::now();
        std::printf("%5zu KB: parse %8.2f ms | layout cold %8.2f ms | relayout %8.2f ms | plain %7.2f ms | blocks %zu lines %zu->%zu text %zu\n",
                    static_cast<std::size_t>(kb), ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4), static_cast<std::size_t>(doc.blockCount),
                    l1.lines.size(), l2.lines.size(), pt.text.size());
    }
    return 0;
}
