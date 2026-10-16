// SPDX-License-Identifier: MPL-2.0
#include "markdown/MarkdownCaret.hpp"

#include "markdown/Grapheme.hpp"
#include "markdown/Utf8.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rivet::markdown {

namespace {

constexpr std::size_t kFar = std::numeric_limits<std::size_t>::max();

std::size_t distance(std::size_t a, std::size_t b) { return a > b ? a - b : b - a; }

bool isWordCodePoint(char32_t cp) {
    if (cp < 0x80) {
        return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
    }
    if (cp == 0xA0 || (cp >= 0x2000 && cp <= 0x206F) || (cp >= 0x3000 && cp <= 0x303F) || cp == 0xFEFF) return false;
    if (cp >= 0x2190 && cp <= 0x2BFF) return false; // arrows, math operators, symbols
    return true;
}

bool isWordText(std::string_view s) { return !s.empty() && isWordCodePoint(utf8::decode(s, 0).codePoint); }

} // namespace

CaretMap::CaretMap(const MarkdownLayout& layout) : layout_(&layout) {
    slot_.assign(layout.runs.size(), kNoSlot);
    for (std::size_t bi = 0; bi < layout.blocks.size(); ++bi) {
        const LayoutBlock& block = layout.blocks[bi];
        if (block.readOnlyIsland()) continue;
        for (std::uint32_t l = block.lineBegin; l < block.lineEnd && l < layout.lines.size(); ++l) {
            const LayoutLine& line = layout.lines[l];
            for (std::uint32_t r = line.runBegin; r < line.runEnd && r < layout.runs.size(); ++r) {
                const LayoutRun& run = layout.runs[r];
                if (run.spanEnd <= run.spanBegin || run.spanEnd > layout.runSpans.size()) continue;
                const auto slot = static_cast<std::uint32_t>(runs_.size());
                const std::size_t start = layout.runSpans[run.spanBegin].source;
                runs_.push_back(r);
                runBlock_.push_back(static_cast<std::uint32_t>(bi));
                runSource_.push_back(runSource_.empty() ? start : std::max(runSource_.back(), start));
                slot_[r] = slot;
                if (lines_.empty() || lines_.back().line != l) {
                    lines_.push_back(LineInfo{l, slot, slot});
                } else {
                    lines_.back().lastSlot = slot;
                }
            }
        }
    }
}

// ------------------------------------------------------------------ helpers

bool CaretMap::joinsPrevious(std::uint32_t slot) const {
    if (slot == 0 || slot >= runs_.size()) return false;
    const std::uint32_t r = runs_[slot];
    return runs_[slot - 1] + 1 == r && layout_->runs[r - 1].line == layout_->runs[r].line;
}

bool CaretMap::validOffset(std::uint32_t run, std::size_t offset) const {
    const LayoutRun& r = layout_->runs[run];
    const std::string_view text = r.text;
    if (offset > text.size()) return false;
    if (offset == 0 || offset == text.size()) return true;
    if ((static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) return false;
    const auto begin = layout_->runSpans.begin() + r.spanBegin;
    const auto end = layout_->runSpans.begin() + r.spanEnd;
    const auto it = std::upper_bound(begin, end, offset,
                                     [](std::size_t v, const TextSpan& sp) { return v < sp.textEnd(); });
    if (it != end && it->text < offset && !it->exact()) return false; // inside an atomic piece
    return grapheme::isBoundary(text, offset);
}

std::size_t CaretMap::nextBoundary(std::uint32_t run, std::size_t offset) const {
    const std::string_view text = layout_->runs[run].text;
    std::size_t j = grapheme::next(text, offset);
    while (j < text.size() && !validOffset(run, j)) j = grapheme::next(text, j);
    return j;
}

std::size_t CaretMap::previousBoundary(std::uint32_t run, std::size_t offset) const {
    const std::string_view text = layout_->runs[run].text;
    std::size_t j = grapheme::previous(text, offset);
    while (j > 0 && !validOffset(run, j)) j = grapheme::previous(text, j);
    return j;
}

std::size_t CaretMap::sourceOf(TextPosition pos) const {
    if (layout_ == nullptr || pos.run >= layout_->runs.size()) return 0;
    const LayoutRun& r = layout_->runs[pos.run];
    if (r.spanEnd <= r.spanBegin) return layout_->sourceOffsetAt(pos);
    const auto begin = layout_->runSpans.begin() + r.spanBegin;
    const auto end = layout_->runSpans.begin() + r.spanEnd;
    const std::size_t k = pos.offset;
    const auto it =
        std::upper_bound(begin, end, k, [](std::size_t v, const TextSpan& sp) { return v < sp.textEnd(); });
    if (it == end) return std::prev(end)->sourceEnd();
    if (k <= it->text) return it->source;
    if (it->exact()) return it->source + (k - it->text);
    return it->sourceEnd();
}

std::size_t CaretMap::charSourceStart(std::uint32_t run, std::size_t offset) const {
    const LayoutRun& r = layout_->runs[run];
    const auto begin = layout_->runSpans.begin() + r.spanBegin;
    const auto end = layout_->runSpans.begin() + r.spanEnd;
    const auto it =
        std::upper_bound(begin, end, offset, [](std::size_t v, const TextSpan& sp) { return v < sp.textEnd(); });
    if (it != end && it->text <= offset) return it->exact() ? it->source + (offset - it->text) : it->source;
    return sourceOf({run, static_cast<std::uint32_t>(offset)});
}

std::size_t CaretMap::charSourceEnd(std::uint32_t run, std::size_t offset) const {
    if (offset == 0) return sourceOf({run, 0});
    const LayoutRun& r = layout_->runs[run];
    const auto begin = layout_->runSpans.begin() + r.spanBegin;
    const auto end = layout_->runSpans.begin() + r.spanEnd;
    const std::size_t last = offset - 1;
    const auto it =
        std::upper_bound(begin, end, last, [](std::size_t v, const TextSpan& sp) { return v < sp.textEnd(); });
    if (it != end && it->text <= last) return it->exact() ? it->source + (offset - it->text) : it->sourceEnd();
    return sourceOf({run, static_cast<std::uint32_t>(offset)});
}

std::size_t CaretMap::lineInfoIndex(std::uint32_t line) const {
    const auto it = std::lower_bound(lines_.begin(), lines_.end(), line,
                                     [](const LineInfo& info, std::uint32_t v) { return info.line < v; });
    if (it == lines_.end()) return lines_.empty() ? 0 : lines_.size() - 1;
    return static_cast<std::size_t>(it - lines_.begin());
}

// ---------------------------------------------------------------- conversions

bool CaretMap::isStop(TextPosition pos) const {
    if (layout_ == nullptr) return false;
    const std::uint32_t slot = slotOf(pos.run);
    if (slot == kNoSlot || !validOffset(pos.run, pos.offset)) return false;
    return !(pos.offset == 0 && joinsPrevious(slot));
}

std::size_t CaretMap::blockOf(TextPosition pos) const {
    const std::uint32_t slot = slotOf(pos.run);
    if (slot != kNoSlot) return runBlock_[slot];
    return layout_ != nullptr ? layout_->blockIndexAtSource(sourceOf(pos)) : 0;
}

std::uint32_t CaretMap::lineOf(TextPosition pos) const {
    if (layout_ == nullptr || pos.run >= layout_->runs.size()) return 0;
    return layout_->runs[pos.run].line;
}

std::optional<TextPosition> CaretMap::nearestInRun(std::uint32_t slot, std::size_t offset, std::size_t* dist) const {
    const std::uint32_t run = runs_[slot];
    const LayoutRun& r = layout_->runs[run];
    const auto begin = layout_->runSpans.begin() + r.spanBegin;
    const auto end = layout_->runSpans.begin() + r.spanEnd;
    std::size_t bestK = 0;
    std::size_t best = kFar;
    const auto consider = [&](std::size_t k, std::size_t d) {
        if (d < best || (d == best && k < bestK)) {
            best = d;
            bestK = k;
        }
    };
    // Spans are ascending in source: only the ones around `offset` matter.
    auto it = std::lower_bound(begin, end, offset,
                               [](const TextSpan& sp, std::size_t v) { return sp.sourceEnd() < v; });
    if (it != begin) --it;
    for (int n = 0; it != end && n < 3; ++it, ++n) {
        if (offset >= it->source && offset <= it->sourceEnd() && it->exact()) {
            const std::size_t k = it->text + (offset - it->source);
            if (validOffset(run, k)) {
                consider(k, 0);
            } else {
                const std::size_t lo = previousBoundary(run, k);
                const std::size_t hi = nextBoundary(run, k);
                consider(lo, distance(charSourceStart(run, lo), offset));
                consider(hi, distance(sourceOf({run, static_cast<std::uint32_t>(hi)}), offset));
            }
        } else {
            consider(it->text, distance(it->source, offset));
            consider(it->textEnd(), distance(it->sourceEnd(), offset));
        }
    }
    if (best == kFar) return std::nullopt;
    if (!validOffset(run, bestK)) bestK = previousBoundary(run, bestK);
    *dist = best;
    return TextPosition{run, static_cast<std::uint32_t>(bestK)};
}

std::optional<TextPosition> CaretMap::positionOf(std::size_t offset) const {
    if (empty()) return std::nullopt;
    const auto it = std::upper_bound(runSource_.begin(), runSource_.end(), offset);
    const std::size_t s = it == runSource_.begin() ? 0 : static_cast<std::size_t>(it - runSource_.begin()) - 1;
    std::optional<TextPosition> best;
    std::size_t bestDist = kFar;
    const std::size_t from = s > 0 ? s - 1 : 0;
    for (std::size_t slot = from; slot < std::min(runs_.size(), s + 2); ++slot) {
        std::size_t d = kFar;
        const std::optional<TextPosition> p = nearestInRun(static_cast<std::uint32_t>(slot), offset, &d);
        if (p && d < bestDist) {
            best = p;
            bestDist = d;
        }
    }
    if (!best) return first();
    return snap(*best);
}

std::optional<TextPosition> CaretMap::snap(TextPosition pos) const {
    if (empty()) return std::nullopt;
    const std::uint32_t slot = slotOf(pos.run);
    if (slot == kNoSlot) return positionOf(sourceOf(pos));
    const LayoutRun& r = layout_->runs[pos.run];
    std::size_t k = std::min<std::size_t>(pos.offset, r.text.size());
    if (!validOffset(pos.run, k)) k = previousBoundary(pos.run, k);
    if (k == 0 && joinsPrevious(slot)) {
        const std::uint32_t prev = runs_[slot - 1];
        return TextPosition{prev, static_cast<std::uint32_t>(layout_->runs[prev].text.size())};
    }
    return TextPosition{pos.run, static_cast<std::uint32_t>(k)};
}

// ------------------------------------------------------------------- movement

std::optional<TextPosition> CaretMap::first() const {
    if (empty()) return std::nullopt;
    return TextPosition{runs_.front(), 0};
}

std::optional<TextPosition> CaretMap::last() const {
    if (empty()) return std::nullopt;
    return TextPosition{runs_.back(), static_cast<std::uint32_t>(layout_->runs[runs_.back()].text.size())};
}

std::optional<TextPosition> CaretMap::next(TextPosition pos) const {
    const std::uint32_t slot = slotOf(pos.run);
    if (slot == kNoSlot) return std::nullopt;
    const std::size_t size = layout_->runs[pos.run].text.size();
    if (pos.offset < size) {
        return TextPosition{pos.run, static_cast<std::uint32_t>(nextBoundary(pos.run, pos.offset))};
    }
    if (slot + 1 >= runs_.size()) return std::nullopt;
    const std::uint32_t r2 = runs_[slot + 1];
    if (joinsPrevious(slot + 1) && !layout_->runs[r2].text.empty()) {
        return TextPosition{r2, static_cast<std::uint32_t>(nextBoundary(r2, 0))};
    }
    return TextPosition{r2, 0};
}

std::optional<TextPosition> CaretMap::previous(TextPosition pos) const {
    const std::uint32_t slot = slotOf(pos.run);
    if (slot == kNoSlot) return std::nullopt;
    if (pos.offset > 0) {
        const std::size_t j = previousBoundary(pos.run, pos.offset);
        if (j == 0 && joinsPrevious(slot)) {
            const std::uint32_t prev = runs_[slot - 1];
            return TextPosition{prev, static_cast<std::uint32_t>(layout_->runs[prev].text.size())};
        }
        return TextPosition{pos.run, static_cast<std::uint32_t>(j)};
    }
    if (slot == 0) return std::nullopt;
    const std::uint32_t prev = runs_[slot - 1];
    const auto prevEnd = static_cast<std::uint32_t>(layout_->runs[prev].text.size());
    if (joinsPrevious(slot)) return previous(TextPosition{prev, prevEnd});
    return TextPosition{prev, prevEnd};
}

TextPosition CaretMap::lineStart(TextPosition pos) const {
    if (empty()) return pos;
    const LineInfo& info = lines_[lineInfoIndex(lineOf(pos))];
    return TextPosition{runs_[info.firstSlot], 0};
}

TextPosition CaretMap::lineEnd(TextPosition pos) const {
    if (empty()) return pos;
    const LineInfo& info = lines_[lineInfoIndex(lineOf(pos))];
    const std::uint32_t r = runs_[info.lastSlot];
    return TextPosition{r, static_cast<std::uint32_t>(layout_->runs[r].text.size())};
}

std::string_view CaretMap::charTextAfter(TextPosition pos) const {
    const std::uint32_t slot = slotOf(pos.run);
    if (slot == kNoSlot) return {};
    const std::string_view text = layout_->runs[pos.run].text;
    if (pos.offset < text.size()) {
        return text.substr(pos.offset, nextBoundary(pos.run, pos.offset) - pos.offset);
    }
    if (slot + 1 < runs_.size() && joinsPrevious(slot + 1)) {
        const std::uint32_t r2 = runs_[slot + 1];
        return std::string_view(layout_->runs[r2].text).substr(0, nextBoundary(r2, 0));
    }
    return {};
}

std::string_view CaretMap::charTextBefore(TextPosition pos) const {
    const std::uint32_t slot = slotOf(pos.run);
    if (slot == kNoSlot) return {};
    const std::string_view text = layout_->runs[pos.run].text;
    if (pos.offset > 0) {
        const std::size_t j = previousBoundary(pos.run, pos.offset);
        return text.substr(j, pos.offset - j);
    }
    if (joinsPrevious(slot)) {
        const std::uint32_t prev = runs_[slot - 1];
        return charTextBefore(TextPosition{prev, static_cast<std::uint32_t>(layout_->runs[prev].text.size())});
    }
    return {};
}

std::optional<TextPosition> CaretMap::wordRight(TextPosition pos) const {
    if (!isStop(pos)) {
        const auto s = snap(pos);
        if (!s) return std::nullopt;
        pos = *s;
    }
    if (charTextAfter(pos).empty()) return next(pos);
    TextPosition p = pos;
    while (!charTextAfter(p).empty() && !isWordText(charTextAfter(p))) p = *next(p);
    while (isWordText(charTextAfter(p))) p = *next(p);
    return p;
}

std::optional<TextPosition> CaretMap::wordLeft(TextPosition pos) const {
    if (!isStop(pos)) {
        const auto s = snap(pos);
        if (!s) return std::nullopt;
        pos = *s;
    }
    if (charTextBefore(pos).empty()) return previous(pos);
    TextPosition p = pos;
    while (!charTextBefore(p).empty() && !isWordText(charTextBefore(p))) p = *previous(p);
    while (isWordText(charTextBefore(p))) p = *previous(p);
    return p;
}

std::pair<TextPosition, TextPosition> CaretMap::wordAt(TextPosition pos) const {
    if (const auto s = snap(pos)) pos = *s;
    TextPosition a = pos;
    TextPosition b = pos;
    if (isWordText(charTextAfter(pos)) || isWordText(charTextBefore(pos))) {
        while (isWordText(charTextAfter(b))) b = *next(b);
        while (isWordText(charTextBefore(a))) a = *previous(a);
    } else if (!charTextAfter(pos).empty()) {
        b = *next(pos);
    }
    return {a, b};
}

TextPosition CaretMap::onLineAtX(std::uint32_t line, double x, const ITextMeasurer& measurer) const {
    const LineInfo& info = lines_[lineInfoIndex(line)];
    std::uint32_t slot = info.firstSlot;
    for (std::uint32_t s = info.firstSlot; s <= info.lastSlot; ++s) {
        if (layout_->runs[runs_[s]].x <= x) slot = s;
    }
    const std::uint32_t run = runs_[slot];
    const LayoutRun& r = layout_->runs[run];
    std::size_t bestK = 0;
    double best = std::numeric_limits<double>::max();
    if (x >= r.x + r.width) {
        bestK = r.text.size();
    } else if (x > r.x) {
        // Binary search over valid boundaries by prefix width.
        std::vector<std::size_t> bounds{0};
        for (std::size_t k = 0; k < r.text.size();) {
            k = nextBoundary(run, k);
            bounds.push_back(k);
        }
        std::size_t lo = 0;
        std::size_t hi = bounds.size() - 1;
        const auto xAt = [&](std::size_t i) {
            return layout_->xForPosition(TextPosition{run, static_cast<std::uint32_t>(bounds[i])}, measurer);
        };
        while (lo < hi) {
            const std::size_t mid = (lo + hi + 1) / 2;
            if (xAt(mid) <= x) lo = mid; else hi = mid - 1;
        }
        bestK = bounds[lo];
        best = std::abs(xAt(lo) - x);
        if (lo + 1 < bounds.size() && std::abs(xAt(lo + 1) - x) < best) bestK = bounds[lo + 1];
    }
    const std::optional<TextPosition> s = snap(TextPosition{run, static_cast<std::uint32_t>(bestK)});
    return s ? *s : TextPosition{run, static_cast<std::uint32_t>(bestK)};
}

TextPosition CaretMap::vertical(TextPosition pos, double x, long deltaLines, const ITextMeasurer& measurer) const {
    if (empty()) return pos;
    const auto here = static_cast<long>(lineInfoIndex(lineOf(pos)));
    const long target = std::clamp(here + deltaLines, 0L, static_cast<long>(lines_.size()) - 1);
    if (target == here && deltaLines != 0) {
        // Already on the first/last line: go to its start/end.
        return deltaLines < 0 ? lineStart(pos) : lineEnd(pos);
    }
    return onLineAtX(lines_[static_cast<std::size_t>(target)].line, x, measurer);
}

// ----------------------------------------------------------------- characters

std::optional<SourceRange> CaretMap::charBefore(TextPosition pos) const {
    const std::uint32_t slot = slotOf(pos.run);
    if (slot == kNoSlot) return std::nullopt;
    if (pos.offset > 0) {
        const std::size_t j = previousBoundary(pos.run, pos.offset);
        return SourceRange{charSourceStart(pos.run, j), charSourceEnd(pos.run, pos.offset)};
    }
    if (slot == 0) return std::nullopt;
    const std::uint32_t prev = runs_[slot - 1];
    const auto prevEnd = static_cast<std::uint32_t>(layout_->runs[prev].text.size());
    if (joinsPrevious(slot)) return charBefore(TextPosition{prev, prevEnd});
    if (runBlock_[slot - 1] != runBlock_[slot]) return std::nullopt;
    const std::size_t a = sourceOf(TextPosition{prev, prevEnd});
    const std::size_t b = sourceOf(pos);
    return SourceRange{std::min(a, b), std::max(a, b)};
}

} // namespace rivet::markdown
