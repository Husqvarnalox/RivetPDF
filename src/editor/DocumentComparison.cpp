// SPDX-License-Identifier: MPL-2.0
#include "editor/DocumentComparison.hpp"

#include "editor/TextService.hpp"
#include "pdf/PdfTextLayout.hpp"

#include <algorithm>
#include <span>
#include <utility>

namespace rivet::editor {

namespace {

constexpr std::size_t kMaxSnippetWords = 6;
constexpr std::size_t kMaxSnippetBytes = 48;

bool isLineBreak(char32_t c) { return c == U'\n' || c == U'\r' || c == 0x2028 || c == 0x2029; }

bool isSpace(char32_t c) {
    return c <= 0x20 || c == 0x7F || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200B) ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF || isLineBreak(c);
}

// Rects of the chars of tokens [begin, end) (empty for an empty range).
std::vector<core::Rect> rectsFor(const pdf::PdfTextPage& page, const std::vector<WordToken>& tokens,
                                 std::size_t begin, std::size_t end) {
    if (begin >= end) return {};
    const std::uint32_t first = tokens[begin].firstChar;
    const std::uint32_t last = tokens[end - 1].lastChar;
    return page.rectsForRange(first, last - first + 1);
}

core::Rect boundsOf(const std::vector<core::Rect>& rects) {
    core::Rect box;
    for (const core::Rect& rect : rects) box = box.united(rect);
    return box;
}

std::string snippetFor(const std::vector<WordToken>& tokens, std::size_t begin, std::size_t end) {
    std::string out;
    std::size_t words = 0;
    for (std::size_t i = begin; i < end; ++i) {
        if (words == kMaxSnippetWords || out.size() >= kMaxSnippetBytes) {
            out += "\xE2\x80\xA6";
            break;
        }
        if (!out.empty()) out.push_back(' ');
        out += tokens[i].text;
        ++words;
    }
    return out;
}

} // namespace

std::vector<WordToken> tokenizeWords(const pdf::PdfTextPage& page) {
    std::vector<WordToken> tokens;
    std::u32string word;
    std::uint32_t wordFirst = 0;
    std::uint32_t wordLast = 0;
    std::uint32_t line = 0;

    const auto flush = [&] {
        if (word.empty()) return;
        tokens.push_back(WordToken{pdf::encodeUtf8(word), wordFirst, wordLast, line});
        word.clear();
    };

    for (const pdf::TextChar& ch : page.chars()) {
        if (isSpace(ch.unicode)) {
            flush();
            if (isLineBreak(ch.unicode)) ++line;
            continue;
        }
        if (pdf::isWordChar(ch.unicode)) {
            if (word.empty()) wordFirst = ch.index;
            word.push_back(ch.unicode);
            wordLast = ch.index;
            continue;
        }
        flush();
        tokens.push_back(WordToken{pdf::encodeUtf8(std::u32string(1, ch.unicode)), ch.index, ch.index, line});
    }
    flush();
    return tokens;
}

PageComparison comparePageText(const pdf::PdfTextPage* left, const pdf::PdfTextPage* right,
                               std::size_t pageIndex, std::vector<ComparisonChange>& changes,
                               const DiffLimits& limits, const std::function<bool()>& cancelled) {
    PageComparison result;
    const pdf::PdfTextPage empty;
    const pdf::PdfTextPage& leftPage = left != nullptr ? *left : empty;
    const pdf::PdfTextPage& rightPage = right != nullptr ? *right : empty;
    const std::vector<WordToken> leftTokens = tokenizeWords(leftPage);
    const std::vector<WordToken> rightTokens = tokenizeWords(rightPage);

    const auto toDiff = [](const std::vector<WordToken>& tokens) {
        std::vector<DiffToken> out;
        out.reserve(tokens.size());
        for (const WordToken& token : tokens) out.push_back(DiffToken{token.text, token.line});
        return out;
    };
    const std::vector<DiffToken> a = toDiff(leftTokens);
    const std::vector<DiffToken> b = toDiff(rightTokens);
    const TextDiffResult diff = diffTokens(a, b, limits, cancelled);
    if (diff.cancelled) return result;
    result.granularity = diff.granularity;

    for (const DiffHunk& hunk : diff.hunks) {
        std::vector<core::Rect> deleted = rectsFor(leftPage, leftTokens, hunk.aBegin, hunk.aEnd);
        std::vector<core::Rect> inserted = rectsFor(rightPage, rightTokens, hunk.bBegin, hunk.bEnd);

        ComparisonChange change;
        change.kind = hunk.aBegin == hunk.aEnd   ? ChangeKind::Inserted
                      : hunk.bBegin == hunk.bEnd ? ChangeKind::Deleted
                                                 : ChangeKind::Replaced;
        change.leftPage = pageIndex;
        change.rightPage = pageIndex;
        change.leftRect = boundsOf(deleted);
        change.rightRect = boundsOf(inserted);
        change.leftSnippet = snippetFor(leftTokens, hunk.aBegin, hunk.aEnd);
        change.rightSnippet = snippetFor(rightTokens, hunk.bBegin, hunk.bEnd);
        changes.push_back(std::move(change));

        result.deletedRects.insert(result.deletedRects.end(), deleted.begin(), deleted.end());
        result.insertedRects.insert(result.insertedRects.end(), inserted.begin(), inserted.end());
    }
    return result;
}

ComparisonResult compareDocuments(const PageSnapshotPtr& left, TextService& leftText,
                                  const PageSnapshotPtr& right, TextService& rightText,
                                  const std::function<bool()>& cancelled,
                                  const std::function<void(std::size_t, std::size_t)>& progress,
                                  const DiffLimits& limits) {
    ComparisonResult result;
    result.leftPageCount = left != nullptr ? left->size() : 0;
    result.rightPageCount = right != nullptr ? right->size() : 0;
    const std::size_t common = std::min(result.leftPageCount, result.rightPageCount);
    const std::size_t total = std::max(result.leftPageCount, result.rightPageCount);
    result.pages.reserve(common);

    for (std::size_t i = 0; i < common; ++i) {
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        const std::shared_ptr<const pdf::PdfTextPage> leftPage = leftText.textPageNow(left->at(i));
        const std::shared_ptr<const pdf::PdfTextPage> rightPage = rightText.textPageNow(right->at(i));
        if (leftPage == nullptr) ++result.extractionFailures;
        if (rightPage == nullptr) ++result.extractionFailures;
        PageComparison page =
            comparePageText(leftPage.get(), rightPage.get(), i, result.changes, limits, cancelled);
        if (cancelled && cancelled()) {
            result.cancelled = true;
            return result;
        }
        if (page.granularity != DiffGranularity::Word) ++result.approximatePages;
        result.pages.push_back(std::move(page));
        if (progress) progress(i + 1, total);
    }

    if (result.leftPageCount > common) {
        ComparisonChange change;
        change.kind = ChangeKind::PageDeleted;
        change.leftPage = common;
        change.rightPage = common;
        change.pageCount = result.leftPageCount - common;
        result.changes.push_back(change);
    }
    if (result.rightPageCount > common) {
        ComparisonChange change;
        change.kind = ChangeKind::PageInserted;
        change.leftPage = common;
        change.rightPage = common;
        change.pageCount = result.rightPageCount - common;
        result.changes.push_back(change);
    }
    if (progress) progress(total, total);
    return result;
}

} // namespace rivet::editor
