// SPDX-License-Identifier: MPL-2.0
#include "app/MarkdownFind.hpp"

#include "app/MarkdownTabState.hpp"

#include <algorithm>
#include <string>

namespace rivet::app {

namespace {

char foldAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

struct Segment {
    std::size_t textStart = 0;
    std::uint32_t run = 0;
    std::uint32_t length = 0;
};

char separatorFor(markdown::LineEnd end) {
    switch (end) {
    case markdown::LineEnd::Wrap: return ' ';
    case markdown::LineEnd::CellEnd: return '\t';
    default: return '\n';
    }
}

} // namespace

std::vector<FindMatch> findInLayout(const markdown::MarkdownLayout& layout, std::string_view query,
                                    std::size_t maxMatches) {
    std::vector<FindMatch> matches;
    if (query.empty() || maxMatches == 0) return matches;
    std::string needle(query);
    std::transform(needle.begin(), needle.end(), needle.begin(), foldAscii);

    std::string text;
    std::vector<Segment> segments;
    for (std::size_t b = 0; b < layout.blocks.size(); ++b) {
        const markdown::LayoutBlock& block = layout.blocks[b];
        if (block.lineEnd <= block.lineBegin) continue;
        text.clear();
        segments.clear();
        for (std::uint32_t l = block.lineBegin; l < block.lineEnd; ++l) {
            const markdown::LayoutLine& line = layout.lines[l];
            for (std::uint32_t r = line.runBegin; r < line.runEnd; ++r) {
                const markdown::LayoutRun& run = layout.runs[r];
                if (run.text.empty()) continue;
                segments.push_back(Segment{text.size(), r, static_cast<std::uint32_t>(run.text.size())});
                for (const char c : run.text) text.push_back(foldAscii(c));
            }
            if (l + 1 < block.lineEnd) text.push_back(separatorFor(line.end));
        }
        if (text.size() < needle.size()) continue;

        // Segment containing byte `at` (a segment owns [textStart, textStart+length)).
        const auto segmentAt = [&](std::size_t at) -> const Segment* {
            auto it = std::upper_bound(segments.begin(), segments.end(), at,
                                       [](std::size_t v, const Segment& s) { return v < s.textStart; });
            if (it == segments.begin()) return nullptr;
            --it;
            return at < it->textStart + it->length ? &*it : nullptr;
        };

        std::size_t from = 0;
        while (from + needle.size() <= text.size()) {
            const std::size_t at = text.find(needle, from);
            if (at == std::string::npos) break;
            from = at + needle.size();
            const Segment* first = segmentAt(at);
            const Segment* last = segmentAt(at + needle.size() - 1);
            if (first == nullptr || last == nullptr) continue; // starts/ends on a separator
            FindMatch m;
            m.begin = markdown::TextPosition{first->run, static_cast<std::uint32_t>(at - first->textStart)};
            m.end = markdown::TextPosition{
                last->run, static_cast<std::uint32_t>(at + needle.size() - last->textStart)};
            m.block = static_cast<std::uint32_t>(b);
            matches.push_back(m);
            if (matches.size() >= maxMatches) return matches;
        }
    }
    return matches;
}

std::vector<TextMatch> findInText(std::string_view text, std::string_view query, std::size_t maxMatches) {
    std::vector<TextMatch> matches;
    if (query.empty() || maxMatches == 0 || text.size() < query.size() || !isValidUtf8(query)) return matches;
    std::string needle(query);
    std::transform(needle.begin(), needle.end(), needle.begin(), foldAscii);
    std::string folded(text);
    std::transform(folded.begin(), folded.end(), folded.begin(), foldAscii);
    std::size_t from = 0;
    while (from + needle.size() <= folded.size()) {
        const std::size_t at = folded.find(needle, from);
        if (at == std::string::npos) break;
        matches.push_back(TextMatch{at, at + needle.size()});
        if (matches.size() >= maxMatches) break;
        from = at + needle.size();
    }
    return matches;
}

} // namespace rivet::app
