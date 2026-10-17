// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/geometry/Rect.hpp"
#include "editor/PageModel.hpp"
#include "editor/TextDiff.hpp"
#include "pdf/PdfText.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rivet::editor {

class TextService;

// Text comparison of two documents (ADR-0029). Pages are paired by index
// (page i of the left document with page i of the right one); the text of a
// pair is tokenized into words with their char boxes (displayed-page
// points) and diffed with Myers (TextDiff). Pure functions over extracted
// text pages plus one driver that pulls text through TextService; no UI.

// One word of a page: [firstChar, lastChar] are page-local char indexes.
struct WordToken {
    std::string text;
    std::uint32_t firstChar = 0;
    std::uint32_t lastChar = 0; // inclusive
    std::uint32_t line = 0;
};

// Words are maximal runs of word characters (pdf::isWordChar); every other
// non-space character is a word of its own. Whitespace separates; line
// breaks (LF, CR, U+2028/2029) start a new line.
std::vector<WordToken> tokenizeWords(const pdf::PdfTextPage& page);

enum class ChangeKind : std::uint8_t {
    Replaced,     // words differ on both sides
    Deleted,      // words only in the left document
    Inserted,     // words only in the right document
    PageDeleted,  // the left document has extra pages at the end
    PageInserted, // the right document has extra pages at the end
};

// One difference. Page indexes are positions in the respective document.
// For PageDeleted/PageInserted, `pageCount` consecutive pages starting at the
// page index are meant and the rects are empty.
struct ComparisonChange {
    ChangeKind kind = ChangeKind::Replaced;
    std::size_t leftPage = 0;
    std::size_t rightPage = 0;
    std::size_t pageCount = 1;
    // Bounding boxes of the differing words (displayed-page points); empty
    // when that side has no words in this change.
    core::Rect leftRect;
    core::Rect rightRect;
    // Short UI excerpts of the differing words. Document text: never log.
    std::string leftSnippet;
    std::string rightSnippet;
};

// Highlights of one page index in both documents.
struct PageComparison {
    std::vector<core::Rect> deletedRects;  // left page, rect per text row
    std::vector<core::Rect> insertedRects; // right page
    DiffGranularity granularity = DiffGranularity::Word;
};

struct ComparisonResult {
    std::size_t leftPageCount = 0;
    std::size_t rightPageCount = 0;
    // One entry per index < min(left, right) page count.
    std::vector<PageComparison> pages;
    // Reading order: by page, then by position; extra pages come last.
    std::vector<ComparisonChange> changes;
    // Pages whose text could not be extracted (compared as empty text).
    std::size_t extractionFailures = 0;
    // Pages diffed per line or as one region (bounds, see DiffLimits).
    std::size_t approximatePages = 0;
    bool cancelled = false;

    bool identical() const { return changes.empty() && !cancelled; }
};

// Diffs one page pair. A null page counts as having no text. Appends the
// page's changes to `changes`; fills the page's highlights.
PageComparison comparePageText(const pdf::PdfTextPage* left, const pdf::PdfTextPage* right,
                               std::size_t pageIndex, std::vector<ComparisonChange>& changes,
                               const DiffLimits& limits = {},
                               const std::function<bool()>& cancelled = {});

// Compares two documents. Worker-thread entry point: extracts through the
// services' synchronous path (textPageNow), so the snapshots' documents and
// the services must stay alive until it returns. `cancelled` is polled
// between pages and inside the diff; `progress(done, total)` (optional) is
// called after each page pair.
ComparisonResult compareDocuments(const PageSnapshotPtr& left, TextService& leftText,
                                  const PageSnapshotPtr& right, TextService& rightText,
                                  const std::function<bool()>& cancelled = {},
                                  const std::function<void(std::size_t, std::size_t)>& progress = {},
                                  const DiffLimits& limits = {});

} // namespace rivet::editor
