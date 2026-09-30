// SPDX-License-Identifier: MPL-2.0
// Split range parsing and output naming (pure, PDFium-independent).
#include "RivetTest.h"

#include "editor/PageRangeParser.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace {

using rivet::editor::PageRange;
using rivet::editor::parsePageRanges;
using rivet::editor::splitOutputPaths;
using Ranges = std::vector<PageRange>;

bool parsesTo(const char* text, std::size_t pages, const Ranges& expected) {
    const auto result = parsePageRanges(text, pages);
    return result.has_value() && *result == expected;
}

bool rejects(const char* text, std::size_t pages) { return !parsePageRanges(text, pages).has_value(); }

std::string errorOf(const char* text, std::size_t pages) {
    const auto result = parsePageRanges(text, pages);
    return result.has_value() ? std::string() : result.error().message;
}

} // namespace

RIVET_TEST(rangeParserAcceptsValidLists) {
    CHECK((parsesTo("1-3, 4-7, 8-10", 10, Ranges{{1, 3}, {4, 7}, {8, 10}})));
    CHECK((parsesTo("1-3,4-7,8-10", 10, Ranges{{1, 3}, {4, 7}, {8, 10}})));
    CHECK((parsesTo("  1-3 \n4-7\t8-10  ", 10, Ranges{{1, 3}, {4, 7}, {8, 10}})));
    CHECK((parsesTo("5", 10, Ranges{{5, 5}})));
    CHECK((parsesTo("5-5", 10, Ranges{{5, 5}})));
    CHECK((parsesTo("2, 4-5", 5, Ranges{{2, 2}, {4, 5}})));
    // Any order; uncovered pages are simply skipped.
    CHECK((parsesTo("8-10, 1-2", 10, Ranges{{8, 10}, {1, 2}})));
    // Whole document.
    CHECK((parsesTo("1-10", 10, Ranges{{1, 10}})));
}

RIVET_TEST(rangeParserRejectsMalformedInput) {
    CHECK(rejects("", 10));
    CHECK(rejects("  \n ,, ", 10));
    CHECK(rejects("0", 10));
    CHECK(rejects("0-3", 10));
    CHECK(rejects("-3", 10));
    CHECK(rejects("-1", 10));
    CHECK(rejects("3-", 10));
    CHECK(rejects("-", 10));
    CHECK(rejects("abc", 10));
    CHECK(rejects("1-x", 10));
    CHECK(rejects("1.5", 10));
    CHECK(rejects("1-3-5", 10));
    CHECK(rejects("1--3", 10));
    CHECK(rejects("3-1", 10));
    CHECK(rejects("11", 10));
    CHECK(rejects("1-11", 10));
    CHECK(rejects("99999999999999999999999999", 10));
    CHECK(rejects("1-99999999999999999999999999", 10));
}

RIVET_TEST(rangeParserRejectsOverlapsInAnyOrder) {
    CHECK(rejects("1-3,3-5", 10));
    CHECK(rejects("4-6, 1-4", 10));
    CHECK(rejects("2, 2", 10));
    CHECK(rejects("1-10, 5", 10));
    CHECK(!rejects("1-3,4-5", 10));
    CHECK(errorOf("1-3,3-5", 10).find("overlap") != std::string::npos);
}

RIVET_TEST(rangeParserMessagesAreSpecific) {
    CHECK(errorOf("0", 10).find("from 1") != std::string::npos);
    CHECK(errorOf("3-1", 10).find("before") != std::string::npos);
    CHECK(errorOf("12", 10).find("10 pages") != std::string::npos);
    CHECK(errorOf("abc", 10).find("abc") != std::string::npos);
    CHECK(errorOf("", 10).find("at least one") != std::string::npos);
}

RIVET_TEST(splitOutputNaming) {
    namespace fs = std::filesystem;
    const Ranges ranges{{1, 3}, {5, 5}};
    for (const char* base : {"/tmp/out/report.pdf", "/tmp/out/report.PDF", "/tmp/out/report.Pdf",
                             "/tmp/out/report"}) {
        const auto paths = splitOutputPaths(base, ranges);
        CHECK_EQ(paths.size(), 2u);
        if (paths.size() != 2) return;
        CHECK(paths[0] == fs::path("/tmp/out/report_1-3.pdf"));
        CHECK(paths[1] == fs::path("/tmp/out/report_5.pdf"));
    }
    // Only one trailing .pdf is stripped; never doubled.
    const auto doubled = splitOutputPaths("/d/a.pdf.pdf", Ranges{{2, 4}});
    CHECK(doubled[0] == fs::path("/d/a.pdf_2-4.pdf"));
    // Other dots survive.
    const auto dotted = splitOutputPaths("/d/v1.2 final.pdf", Ranges{{1, 2}});
    CHECK(dotted[0] == fs::path("/d/v1.2 final_1-2.pdf"));
    CHECK(splitOutputPaths("/d/x.pdf", {}).empty());
    // Deterministic.
    CHECK(splitOutputPaths("/d/x.pdf", ranges) == splitOutputPaths("/d/x.pdf", ranges));
}
