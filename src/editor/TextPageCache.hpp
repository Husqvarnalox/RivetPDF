// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/StrongId.hpp"
#include "pdf/PdfText.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace rivet::editor {

// LRU cache of extracted text pages, bounded by an exact byte budget
// (default 64 MiB) so a 10,000-page document never holds every page's
// geometry. Same design philosophy as render::TileCache: exact byte
// accounting, mutex-guarded, revision-stamped.
//
// Revision semantics: entries are stamped with the document revision they
// were extracted for. get() with a different revision is a miss and lazily
// drops the stale entry, so a future document edit invalidates extracted
// text without a full sweep.
//
// Thread-safe: the extraction path runs on worker threads while the UI
// thread probes the cache. Entries are shared_ptr<const PdfTextPage>: a
// returned reference stays alive even if the entry is evicted afterwards.
class TextPageCache {
public:
    explicit TextPageCache(std::size_t maxBytes = 64 * 1024 * 1024); // 64 MiB default

    // Shrinking the budget evicts least-recently-used entries immediately.
    void setMaxBytes(std::size_t maxBytes);
    std::size_t maxBytes() const;

    // Returns false (stores nothing) when page is null or its memoryBytes()
    // alone exceeds maxBytes(). Re-putting an existing key replaces the
    // entry and promotes it to most-recently-used.
    bool put(core::PageId pageId, std::uint64_t revision,
             std::shared_ptr<const pdf::PdfTextPage> page);

    // nullptr on miss or stale revision; a hit is promoted to
    // most-recently-used.
    std::shared_ptr<const pdf::PdfTextPage> get(core::PageId pageId,
                                                std::uint64_t revision) const;

    void remove(core::PageId pageId);
    void clear();

    std::size_t sizeBytes() const; // exact: sum of page->memoryBytes()
    std::size_t count() const;

private:
    struct Entry {
        std::uint64_t revision = 0;
        std::shared_ptr<const pdf::PdfTextPage> page;
    };

    using List = std::list<std::pair<core::PageId, Entry>>; // front = MRU

    void evictBack(); // precondition: non-empty; mutex_ held

    mutable std::mutex mutex_;
    mutable List lru_;
    mutable std::unordered_map<core::PageId, List::iterator> index_;
    std::size_t maxBytes_;
    mutable std::size_t sizeBytes_ = 0; // invariant: sizeBytes_ <= maxBytes_
};

} // namespace rivet::editor
