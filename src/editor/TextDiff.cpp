// SPDX-License-Identifier: MPL-2.0
#include "editor/TextDiff.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>

namespace rivet::editor {

namespace {

constexpr std::size_t kCancelPollMask = 63; // poll every 64 rounds

using Index = std::ptrdiff_t;

// Myers over integer ids. Returns the hunks (indexes relative to the given
// spans), nullopt when the edit distance exceeds maxD or on cancellation
// (`cancelledOut` tells which).
std::optional<std::vector<DiffHunk>> myers(std::span<const std::uint32_t> a,
                                           std::span<const std::uint32_t> b, std::size_t maxD,
                                           const std::function<bool()>& cancelled,
                                           bool& cancelledOut) {
    const auto n = static_cast<Index>(a.size());
    const auto m = static_cast<Index>(b.size());
    if (n == 0 && m == 0) return std::vector<DiffHunk>{};
    const Index limit = std::min<Index>(n + m, static_cast<Index>(maxD));
    const Index offset = limit + 1;
    std::vector<Index> v(static_cast<std::size_t>(2 * limit + 3), 0);
    const auto at = [&v, offset](Index k) -> Index& { return v[static_cast<std::size_t>(offset + k)]; };
    // trace[d][(k + d) / 2] = v[k] after round d (diagonals k = -d..d step 2).
    std::vector<std::vector<Index>> trace;

    Index found = -1;
    for (Index d = 0; d <= limit && found < 0; ++d) {
        if ((static_cast<std::size_t>(d) & kCancelPollMask) == 0 && cancelled && cancelled()) {
            cancelledOut = true;
            return std::nullopt;
        }
        std::vector<Index> snapshot;
        snapshot.reserve(static_cast<std::size_t>(d) + 1);
        for (Index k = -d; k <= d; k += 2) {
            Index x = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? at(k + 1) : at(k - 1) + 1;
            Index y = x - k;
            while (x < n && y < m && a[static_cast<std::size_t>(x)] == b[static_cast<std::size_t>(y)]) {
                ++x;
                ++y;
            }
            at(k) = x;
            snapshot.push_back(x);
            if (x >= n && y >= m) found = d;
        }
        trace.push_back(std::move(snapshot));
    }
    if (found < 0) return std::nullopt;

    const auto get = [&trace](Index d, Index k) {
        return trace[static_cast<std::size_t>(d)][static_cast<std::size_t>((k + d) / 2)];
    };
    std::vector<bool> deleted(static_cast<std::size_t>(n), false);
    std::vector<bool> inserted(static_cast<std::size_t>(m), false);
    Index x = n;
    Index y = m;
    for (Index d = found; d > 0; --d) {
        const Index k = x - y;
        const bool down = k == -d || (k != d && get(d - 1, k - 1) < get(d - 1, k + 1));
        const Index prevK = down ? k + 1 : k - 1;
        const Index prevX = get(d - 1, prevK);
        const Index prevY = prevX - prevK;
        if (down) {
            inserted[static_cast<std::size_t>(prevY)] = true;
        } else {
            deleted[static_cast<std::size_t>(prevX)] = true;
        }
        x = prevX;
        y = prevY;
    }

    std::vector<DiffHunk> hunks;
    std::size_t i = 0;
    std::size_t j = 0;
    const auto un = static_cast<std::size_t>(n);
    const auto um = static_cast<std::size_t>(m);
    while (i < un || j < um) {
        if (i < un && j < um && !deleted[i] && !inserted[j]) {
            ++i;
            ++j;
            continue;
        }
        DiffHunk hunk;
        hunk.aBegin = i;
        hunk.bBegin = j;
        while (i < un && deleted[i]) ++i;
        while (j < um && inserted[j]) ++j;
        hunk.aEnd = i;
        hunk.bEnd = j;
        hunks.push_back(hunk);
    }
    return hunks;
}

struct Interner {
    std::unordered_map<std::string, std::uint32_t> ids;
    std::uint32_t intern(const std::string& text) {
        return ids.try_emplace(text, static_cast<std::uint32_t>(ids.size())).first->second;
    }
};

// Common prefix/suffix trimmed, then Myers on the middle. Hunks are in
// absolute indexes.
std::optional<std::vector<DiffHunk>> diffIds(std::span<const std::uint32_t> a,
                                             std::span<const std::uint32_t> b, std::size_t maxD,
                                             const std::function<bool()>& cancelled,
                                             bool& cancelledOut) {
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }
    const auto midA = a.subspan(prefix, a.size() - prefix - suffix);
    const auto midB = b.subspan(prefix, b.size() - prefix - suffix);
    if (midA.empty() && midB.empty()) return std::vector<DiffHunk>{};
    if (midA.empty() || midB.empty()) {
        return std::vector<DiffHunk>{DiffHunk{prefix, prefix + midA.size(), prefix, prefix + midB.size()}};
    }
    auto relative = myers(midA, midB, maxD, cancelled, cancelledOut);
    if (!relative.has_value()) return std::nullopt;
    for (DiffHunk& hunk : *relative) {
        hunk.aBegin += prefix;
        hunk.aEnd += prefix;
        hunk.bBegin += prefix;
        hunk.bEnd += prefix;
    }
    return relative;
}

struct LineSequence {
    std::vector<std::uint32_t> ids;
    std::vector<std::size_t> start; // first token of each line; back() == token count
};

LineSequence groupLines(std::span<const DiffToken> tokens, Interner& interner) {
    LineSequence lines;
    std::size_t i = 0;
    while (i < tokens.size()) {
        std::size_t end = i + 1;
        while (end < tokens.size() && tokens[end].line == tokens[i].line) ++end;
        std::string joined;
        for (std::size_t t = i; t < end; ++t) {
            if (t != i) joined.push_back('\x1f');
            joined += tokens[t].text;
        }
        lines.start.push_back(i);
        lines.ids.push_back(interner.intern(joined));
        i = end;
    }
    lines.start.push_back(tokens.size());
    return lines;
}

} // namespace

TextDiffResult diffTokens(std::span<const DiffToken> a, std::span<const DiffToken> b,
                          const DiffLimits& limits, const std::function<bool()>& cancelled) {
    TextDiffResult result;
    bool cancelledOut = false;

    if (a.size() <= limits.maxTokens && b.size() <= limits.maxTokens) {
        Interner interner;
        std::vector<std::uint32_t> idsA;
        std::vector<std::uint32_t> idsB;
        idsA.reserve(a.size());
        idsB.reserve(b.size());
        for (const DiffToken& token : a) idsA.push_back(interner.intern(token.text));
        for (const DiffToken& token : b) idsB.push_back(interner.intern(token.text));
        auto hunks = diffIds(idsA, idsB, limits.maxEditDistance, cancelled, cancelledOut);
        if (cancelledOut) {
            result.cancelled = true;
            return result;
        }
        if (hunks.has_value()) {
            result.hunks = std::move(*hunks);
            return result;
        }
    }

    // Per-line fallback.
    Interner interner;
    const LineSequence linesA = groupLines(a, interner);
    const LineSequence linesB = groupLines(b, interner);
    if (linesA.ids.size() <= limits.maxTokens && linesB.ids.size() <= limits.maxTokens) {
        auto lineHunks = diffIds(linesA.ids, linesB.ids, limits.maxEditDistance, cancelled, cancelledOut);
        if (cancelledOut) {
            result.cancelled = true;
            return result;
        }
        if (lineHunks.has_value()) {
            result.granularity = DiffGranularity::Line;
            for (const DiffHunk& hunk : *lineHunks) {
                result.hunks.push_back(DiffHunk{linesA.start[hunk.aBegin], linesA.start[hunk.aEnd],
                                                linesB.start[hunk.bBegin], linesB.start[hunk.bEnd]});
            }
            return result;
        }
    }

    // Coarse: the common prefix/suffix (by text) stays, the middle is one hunk.
    result.granularity = DiffGranularity::Coarse;
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix].text == b[prefix].text) ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix].text == b[b.size() - 1 - suffix].text) {
        ++suffix;
    }
    if (prefix + suffix < a.size() || prefix + suffix < b.size()) {
        result.hunks.push_back(DiffHunk{prefix, a.size() - suffix, prefix, b.size() - suffix});
    }
    return result;
}

} // namespace rivet::editor
