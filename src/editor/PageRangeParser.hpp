// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::editor {

// An inclusive, 1-based page range as the user types it ("4-7", "5").
struct PageRange {
    std::size_t first = 1;
    std::size_t last = 1;

    std::size_t size() const { return last - first + 1; }
    friend bool operator==(const PageRange&, const PageRange&) = default;
};

// Parses a split specification such as "1-3, 4-7 8-10" (ranges separated by
// commas, whitespace or newlines; "5" is the single page 5). Pages are
// 1-based and must lie in [1, pageCount]. Ranges must be pairwise disjoint
// (they may appear in any order; uncovered pages are simply not exported).
// Order of the result = order of appearance. Every rejection is an
// InvalidArgument error with a specific, user-presentable message.
core::Result<std::vector<PageRange>> parsePageRanges(std::string_view text, std::size_t pageCount);

// Output file names for a split: `<stem>_<a>-<b>.pdf` (`<stem>_<a>.pdf` for a
// single-page range), in the directory of `basePath`. The stem is the base
// file name without one trailing ".pdf" (any case), so ".pdf.pdf" never
// occurs. Deterministic; parallel to `ranges`.
std::vector<std::filesystem::path> splitOutputPaths(const std::filesystem::path& basePath,
                                                    const std::vector<PageRange>& ranges);

} // namespace rivet::editor
