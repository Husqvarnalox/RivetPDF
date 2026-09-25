// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "editor/TextPageCache.hpp"
#include "pdf/PdfText.hpp"

#include <memory>
#include <utility>
#include <vector>

using rivet::core::PageId;
using rivet::editor::TextPageCache;
using rivet::pdf::PdfTextPage;
using rivet::pdf::TextChar;

namespace {

std::shared_ptr<const PdfTextPage> makePage(std::size_t charCount, double bytesHint) {
    std::vector<TextChar> chars;
    chars.reserve(charCount);
    for (std::size_t i = 0; i < charCount; ++i) {
        TextChar ch;
        ch.unicode = static_cast<char32_t>(U'a' + (i % 26));
        ch.index = static_cast<std::uint32_t>(i);
        ch.bounds = rivet::core::Rect{static_cast<double>(i) * 10.0, 0.0, 8.0, 12.0};
        ch.fontSize = 12.0;
        chars.push_back(ch);
    }
    auto page = std::make_shared<const PdfTextPage>(std::move(chars));
    (void)bytesHint;
    return page;
}

} // namespace

RIVET_TEST(textPageCachePutGetAndEviction) {
    auto pageA = makePage(100, 0);
    const std::size_t entryBytes = pageA->memoryBytes();
    // Budget fits exactly three entries.
    TextPageCache cache(entryBytes * 3);

    CHECK_EQ(cache.put(PageId{1}, 1, pageA), true);
    CHECK_EQ(cache.put(PageId{2}, 1, makePage(100, 0)), true);
    CHECK_EQ(cache.put(PageId{3}, 1, makePage(100, 0)), true);
    CHECK_EQ(cache.count(), std::size_t{3});
    CHECK_EQ(cache.get(PageId{1}, 1).get(), pageA.get());
    CHECK_EQ(cache.sizeBytes(), entryBytes * 3);

    // LRU: A was just promoted; the MRU order is 1, 3, 2 - a fourth insert
    // evicts page 2 (least recently used) and keeps 1 and 3.
    CHECK_EQ(cache.put(PageId{4}, 1, makePage(100, 0)), true);
    CHECK_EQ(cache.get(PageId{2}, 1), nullptr);           // evicted
    CHECK_EQ(cache.get(PageId{1}, 1).get(), pageA.get()); // promoted, kept
    CHECK_EQ(cache.get(PageId{3}, 1) != nullptr, true);   // kept
    CHECK_EQ(cache.count(), std::size_t{3});
}

RIVET_TEST(textPageCacheRevisionStampsEntries) {
    TextPageCache cache(1024 * 1024);
    auto page = makePage(10, 0);
    CHECK_EQ(cache.put(PageId{1}, 1, page), true);
    CHECK_EQ(cache.get(PageId{1}, 1).get(), page.get());
    // Different revision: miss + stale entry dropped.
    CHECK_EQ(cache.get(PageId{1}, 2), nullptr);
    CHECK_EQ(cache.count(), std::size_t{0});
}

RIVET_TEST(textPageCacheRejectsOversizedAndNullEntries) {
    TextPageCache cache(64);
    CHECK_EQ(cache.put(PageId{1}, 1, nullptr), false);
    auto huge = makePage(100000, 0); // memoryBytes >> 64
    CHECK_EQ(cache.put(PageId{1}, 1, huge), false);
    CHECK_EQ(cache.count(), std::size_t{0});
}

RIVET_TEST(textPageCacheShrinkingBudgetEvicts) {
    TextPageCache cache(1024 * 1024);
    auto page = makePage(100, 0);
    for (std::size_t i = 0; i < 10; ++i) {
        CHECK_EQ(cache.put(PageId{i}, 1, makePage(100, 0)), true);
    }
    const std::size_t bytes = page->memoryBytes();
    cache.setMaxBytes(bytes); // keep exactly one entry
    CHECK_LE(cache.sizeBytes(), bytes);
    CHECK_EQ(cache.count(), std::size_t{1});
}
