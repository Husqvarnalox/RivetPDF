// SPDX-License-Identifier: MPL-2.0
#include "editor/PageRangeParser.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>

namespace rivet::editor {

namespace {

core::Error invalid(std::string message) {
    return core::makeError(core::ErrorCode::InvalidArgument, std::move(message), "editor");
}

bool isSeparator(char c) { return c == ',' || c == ';' || std::isspace(static_cast<unsigned char>(c)) != 0; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Parses a run of digits; false when empty, non-numeric or too large.
enum class NumberStatus { Ok, Invalid, TooLarge };

NumberStatus parseNumber(std::string_view text, std::size_t& out) {
    if (text.empty()) return NumberStatus::Invalid;
    std::size_t value = 0;
    for (const char c : text) {
        if (!isDigit(c)) return NumberStatus::Invalid;
        const std::size_t digit = static_cast<std::size_t>(c - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10) return NumberStatus::TooLarge;
        value = value * 10 + digit;
    }
    out = value;
    return NumberStatus::Ok;
}

} // namespace

core::Result<std::vector<PageRange>> parsePageRanges(std::string_view text, std::size_t pageCount) {
    std::vector<PageRange> ranges;
    std::size_t pos = 0;
    while (pos < text.size()) {
        while (pos < text.size() && isSeparator(text[pos])) ++pos;
        if (pos >= text.size()) break;
        const std::size_t start = pos;
        while (pos < text.size() && !isSeparator(text[pos])) ++pos;
        const std::string_view token = text.substr(start, pos - start);

        const auto dash = token.find('-');
        std::string_view firstText = token.substr(0, dash);
        std::string_view lastText = dash == std::string_view::npos ? firstText : token.substr(dash + 1);
        if (dash != std::string_view::npos && lastText.find('-') != std::string_view::npos) {
            return std::unexpected(invalid(std::format("\"{}\" is not a valid page range", token)));
        }
        PageRange range;
        for (int part = 0; part < 2; ++part) {
            const std::string_view piece = part == 0 ? firstText : lastText;
            std::size_t value = 0;
            if (piece.empty()) {
                return std::unexpected(invalid(std::format(
                    "\"{}\" is incomplete: a range needs a start and an end page (negative pages are not allowed)", token)));
            }
            switch (parseNumber(piece, value)) {
            case NumberStatus::Invalid:
                return std::unexpected(invalid(std::format("\"{}\" is not a valid page range", token)));
            case NumberStatus::TooLarge:
                return std::unexpected(invalid(
                    std::format("\"{}\": the page number is too large (the document has {} pages)", token, pageCount)));
            case NumberStatus::Ok: break;
            }
            if (value == 0) {
                return std::unexpected(invalid(std::format("\"{}\": pages are numbered from 1", token)));
            }
            if (value > pageCount) {
                return std::unexpected(invalid(
                    std::format("\"{}\": page {} is out of range (the document has {} pages)", token, value, pageCount)));
            }
            (part == 0 ? range.first : range.last) = value;
        }
        if (range.last < range.first) {
            return std::unexpected(
                invalid(std::format("\"{}\": the end page is before the start page", token)));
        }
        ranges.push_back(range);
    }
    if (ranges.empty()) return std::unexpected(invalid("Enter at least one page range, for example 1-3, 4-7"));

    // Pairwise disjoint: sort a copy by start and compare neighbours.
    std::vector<PageRange> sorted = ranges;
    std::sort(sorted.begin(), sorted.end(),
              [](const PageRange& a, const PageRange& b) { return a.first < b.first; });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i].first <= sorted[i - 1].last) {
            const auto label = [](const PageRange& r) {
                return r.first == r.last ? std::format("{}", r.first) : std::format("{}-{}", r.first, r.last);
            };
            return std::unexpected(invalid(std::format("Ranges {} and {} overlap; ranges must not share pages",
                                                      label(sorted[i - 1]), label(sorted[i]))));
        }
    }
    return ranges;
}

std::vector<std::filesystem::path> splitOutputPaths(const std::filesystem::path& basePath,
                                                    const std::vector<PageRange>& ranges) {
    std::string stem = basePath.filename().string();
    if (stem.size() >= 4) {
        std::string tail = stem.substr(stem.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (tail == ".pdf") stem.resize(stem.size() - 4);
    }
    const std::filesystem::path directory = basePath.parent_path();
    std::vector<std::filesystem::path> out;
    out.reserve(ranges.size());
    for (const PageRange& range : ranges) {
        const std::string name = range.first == range.last
                                     ? std::format("{}_{}.pdf", stem, range.first)
                                     : std::format("{}_{}-{}.pdf", stem, range.first, range.last);
        out.push_back(directory / name);
    }
    return out;
}

} // namespace rivet::editor
