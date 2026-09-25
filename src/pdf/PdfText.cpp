// SPDX-License-Identifier: MPL-2.0

#include "pdf/PdfText.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace rivet::pdf {
namespace {

// Appends one code point to a UTF-8 string. Invalid code points (surrogates,
// above U+10FFFF) are encoded as U+FFFD so the output is always valid UTF-8.
void appendUtf8(std::string& out, char32_t codePoint) {
    std::uint32_t cp = static_cast<std::uint32_t>(codePoint);
    if (cp >= 0xD800u && cp <= 0xDFFFu) {
        cp = 0xFFFDu; // surrogate half
    } else if (cp > 0x10FFFFu) {
        cp = 0xFFFDu;
    }

    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// Decodes UTF-8 into code points. Malformed sequences (bad lead bytes, broken
// continuations, overlong encodings, surrogate or out-of-range values) each
// yield one U+FFFD and consume one byte.
std::vector<char32_t> decodeUtf8(std::string_view input) {
    std::vector<char32_t> out;
    out.reserve(input.size());

    const auto* bytes = reinterpret_cast<const unsigned char*>(input.data());
    const std::size_t size = input.size();
    std::size_t i = 0;
    while (i < size) {
        const unsigned char lead = bytes[i];
        std::size_t length = 0;
        char32_t codePoint = 0xFFFD;
        if (lead <= 0x7Fu) {
            codePoint = lead;
            length = 1;
        } else if (lead >= 0xC2u && lead <= 0xDFu) {
            codePoint = lead & 0x1Fu;
            length = 2;
        } else if (lead >= 0xE0u && lead <= 0xEFu) {
            codePoint = lead & 0x0Fu;
            length = 3;
        } else if (lead >= 0xF0u && lead <= 0xF4u) {
            codePoint = lead & 0x07u;
            length = 4;
        }

        bool valid = length > 0 && i + length <= size;
        if (valid) {
            for (std::size_t k = 1; k < length; ++k) {
                const unsigned char cont = bytes[i + k];
                if ((cont & 0xC0u) != 0x80u) {
                    valid = false;
                    break;
                }
                codePoint = (codePoint << 6) | (cont & 0x3Fu);
            }
        }
        const bool overlong = valid &&
            ((length == 2 && codePoint < 0x80u) || (length == 3 && codePoint < 0x800u) ||
             (length == 4 && codePoint < 0x10000u));
        const bool outOfRange = valid && (codePoint > 0x10FFFFu || (codePoint >= 0xD800u && codePoint <= 0xDFFFu));
        if (!valid || overlong || outOfRange) {
            out.push_back(0xFFFD);
            i += 1;
            continue;
        }
        out.push_back(codePoint);
        i += length;
    }
    return out;
}

} // namespace

PdfTextPage::PdfTextPage(std::vector<TextChar> chars) : chars_(std::move(chars)) {
    // UTF-8 needs at most 4 bytes per code point; 1 is the common case, so
    // start with one byte per char and let the string grow if needed.
    text_.reserve(chars_.size());
    for (const TextChar& ch : chars_) {
        appendUtf8(text_, ch.unicode);
    }
    buildRowIndex();
}

std::size_t PdfTextPage::memoryBytes() const {
    return chars_.size() * sizeof(TextChar) + text_.capacity() + rows_.size() * sizeof(Row);
}

void PdfTextPage::buildRowIndex() {
    rows_.clear();
    const std::size_t count = chars_.size();
    std::size_t i = 0;
    while (i < count) {
        // Skip zero-area chars (generated line breaks); they belong to no row.
        if (chars_[i].bounds.isEmpty()) {
            ++i;
            continue;
        }

        // Start a row at the first selectable char.
        std::size_t rowStart = i;
        std::size_t rowLast = i;
        double ySum = chars_[i].bounds.center().y;
        std::size_t yCount = 1;

        // Extend while consecutive selectable chars stay on the same row: a
        // new row starts when the gap between consecutive y-centers exceeds
        // half of the larger of the two chars' font sizes (em sizes).
        //
        // The original spec said "half of the previous char's bounds height",
        // but PDFium char boxes hug the glyph: a space or comma box can be a
        // few hundredths of a point tall, so a height-scaled threshold
        // fragments every visual line at spaces and punctuation (verified
        // against the text fixtures' real extraction data). The em size is
        // the stable measure of text size and delivers the rule's stated
        // goal - robustness across font sizes: intra-line y-center spread
        // stays well under 0.5 em, while real line spacing exceeds it.
        std::size_t j = i + 1;
        while (j < count) {
            if (chars_[j].bounds.isEmpty()) {
                ++j;
                continue;
            }
            const double previousY = chars_[rowLast].bounds.center().y;
            const double y = chars_[j].bounds.center().y;
            const double emSize = std::max(chars_[rowLast].fontSize, chars_[j].fontSize);
            if (std::fabs(y - previousY) > 0.5 * emSize) {
                break;
            }
            rowLast = j;
            ySum += y;
            ++yCount;
            ++j;
        }

        Row row;
        row.yCenter = ySum / static_cast<double>(yCount);
        row.first = chars_[rowStart].index;
        row.last = chars_[rowLast].index;
        rows_.push_back(row);
        i = j;
    }
}

const PdfTextPage::Row* PdfTextPage::rowContainingIndex(std::uint32_t index) const {
    // Rows have disjoint, ascending [first, last] ranges.
    auto it = std::upper_bound(rows_.begin(), rows_.end(), index,
                               [](std::uint32_t value, const Row& row) { return value < row.first; });
    if (it == rows_.begin()) {
        return nullptr;
    }
    --it;
    if (index > it->last) {
        return nullptr;
    }
    return &*it;
}

std::vector<core::Rect> PdfTextPage::rectsForRange(std::uint32_t start, std::uint32_t count) const {
    std::vector<core::Rect> rects;
    const std::uint32_t total = static_cast<std::uint32_t>(chars_.size());
    if (count == 0 || start >= total) {
        return rects;
    }
    const std::uint32_t end = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(static_cast<std::uint64_t>(start) + count, total));

    const Row* currentRow = nullptr;
    core::Rect currentRect;
    auto flush = [&]() {
        if (currentRow != nullptr) {
            rects.push_back(currentRect);
            currentRow = nullptr;
        }
    };

    for (std::uint32_t i = start; i < end; ++i) {
        const Row* row = rowContainingIndex(i);
        if (row == nullptr) {
            // Zero-area char outside any row (e.g. a line break): it closes
            // the current run so rects never span two rows.
            flush();
            continue;
        }
        if (chars_[i].bounds.isEmpty()) {
            // Zero-area char inside a row (rare): contributes nothing and
            // does not split the row's rect.
            continue;
        }
        if (row != currentRow) {
            flush();
            currentRow = row;
            currentRect = chars_[i].bounds;
        } else {
            currentRect = currentRect.united(chars_[i].bounds);
        }
    }
    flush();
    return rects;
}

std::optional<std::uint32_t> PdfTextPage::charIndexAtPoint(const core::Point& point) const {
    if (rows_.empty()) {
        return std::nullopt;
    }

    // Nearest row by y-center; strict comparison keeps the earlier row on ties.
    const Row* bestRow = &rows_.front();
    double bestDistance = std::fabs(bestRow->yCenter - point.y);
    for (const Row& row : rows_) {
        const double distance = std::fabs(row.yCenter - point.y);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestRow = &row;
        }
    }

    // Nearest selectable char by x-center within the row; strict comparison
    // keeps the earlier char on ties.
    std::optional<std::uint32_t> best;
    double bestXDistance = 0.0;
    for (std::uint32_t i = bestRow->first; i <= bestRow->last; ++i) {
        if (i >= chars_.size() || chars_[i].bounds.isEmpty()) {
            continue;
        }
        const double distance = std::fabs(chars_[i].bounds.center().x - point.x);
        if (!best.has_value() || distance < bestXDistance) {
            bestXDistance = distance;
            best = chars_[i].index;
        }
    }
    return best;
}

char32_t foldCodePoint(char32_t codePoint) {
    const std::uint32_t cp = static_cast<std::uint32_t>(codePoint);

    // ASCII.
    if (cp >= 0x41u && cp <= 0x5Au) {
        return codePoint + 0x20;
    }
    // Latin-1 Supplement (uppercase run, multiplication sign excluded).
    if (cp >= 0xC0u && cp <= 0xDEu && cp != 0xD7u) {
        return codePoint + 0x20;
    }
    // Greek (U+03A2 is unassigned; harmless to map).
    if (cp >= 0x391u && cp <= 0x3A9u) {
        return codePoint + 0x20;
    }
    // Cyrillic.
    if (cp >= 0x400u && cp <= 0x40Fu) {
        return codePoint + 0x50;
    }
    if (cp >= 0x410u && cp <= 0x42Fu) {
        return codePoint + 0x20;
    }
    // Paired uppercase/lowercase runs (uppercase is even, except U+04C1..).
    if (cp >= 0x460u && cp <= 0x481u && (cp & 1u) == 0u) {
        return codePoint + 1;
    }
    if (cp >= 0x48Au && cp <= 0x4BFu && (cp & 1u) == 0u) {
        return codePoint + 1;
    }
    if (cp >= 0x4C1u && cp <= 0x4CEu && (cp & 1u) == 1u) {
        return codePoint + 1;
    }
    if (cp >= 0x4D0u && cp <= 0x4FFu && (cp & 1u) == 0u) {
        return codePoint + 1;
    }
    // The obvious Latin pairs outside Latin-1.
    if (cp == 0x178u) {
        return codePoint - 0x79; // Y with diaeresis -> y with diaeresis
    }
    if (cp >= 0x1E00u && cp <= 0x1E95u && (cp & 1u) == 0u) {
        return codePoint + 1;
    }
    if (cp >= 0x1EA0u && cp <= 0x1EFFu && (cp & 1u) == 0u) {
        return codePoint + 1;
    }
    return codePoint;
}

bool isWordChar(char32_t codePoint) {
    const std::uint32_t cp = static_cast<std::uint32_t>(codePoint);

    if (cp == '_') {
        return true;
    }
    // ASCII letters and digits.
    if ((cp >= 0x30u && cp <= 0x39u) || (cp >= 0x41u && cp <= 0x5Au) || (cp >= 0x61u && cp <= 0x7Au)) {
        return true;
    }
    // Latin-1 letters (division sign excluded).
    if (cp >= 0xC0u && cp <= 0xFFu && cp != 0xD7u && cp != 0xF7u) {
        return true;
    }
    // Greek letters.
    if (cp >= 0x391u && cp <= 0x3A9u) {
        return true;
    }
    if (cp >= 0x3B1u && cp <= 0x3C9u && cp != 0x3C2u) {
        return true;
    }
    // Cyrillic letters.
    if (cp >= 0x400u && cp <= 0x4FFu) {
        return true;
    }
    return false;
}

std::vector<TextSearchResult> searchTextPage(const PdfTextPage& page,
                                             std::string_view needle,
                                             const TextSearchOptions& options) {
    std::vector<TextSearchResult> results;
    if (needle.empty()) {
        return results;
    }

    std::vector<char32_t> pattern = decodeUtf8(needle);
    if (pattern.empty() || pattern.size() > page.charCount()) {
        return results;
    }

    // UTF-32 view of the page text: one unit per TextChar, so vector indexes
    // are page-local char indexes.
    std::vector<char32_t> text;
    text.reserve(page.charCount());
    for (const TextChar& ch : page.chars()) {
        text.push_back(ch.unicode);
    }

    if (!options.caseSensitive) {
        std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                       [](char32_t cp) { return foldCodePoint(cp); });
        std::transform(text.begin(), text.end(), text.begin(),
                       [](char32_t cp) { return foldCodePoint(cp); });
    }

    const std::size_t patternSize = pattern.size();
    std::size_t i = 0;
    while (i + patternSize <= text.size()) {
        bool matched = true;
        for (std::size_t k = 0; k < patternSize; ++k) {
            if (text[i + k] != pattern[k]) {
                matched = false;
                break;
            }
        }
        if (!matched) {
            ++i;
            continue;
        }
        if (options.wholeWord) {
            const bool beforeOk = i == 0 || !isWordChar(text[i - 1]);
            const bool afterOk = i + patternSize >= text.size() || !isWordChar(text[i + patternSize]);
            if (!beforeOk || !afterOk) {
                ++i;
                continue;
            }
        }
        results.push_back(TextSearchResult{static_cast<std::uint32_t>(i),
                                           static_cast<std::uint32_t>(patternSize)});
        i += patternSize; // non-overlapping: resume after the match
    }
    return results;
}

} // namespace rivet::pdf
