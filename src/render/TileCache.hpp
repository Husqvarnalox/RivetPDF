#pragma once

#include "core/Bitmap.hpp"
#include "render/TileKey.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace rivet::render {

// LRU cache of rendered tiles, bounded by an exact byte budget.
//
// Thread-safe: entries may be inserted, looked up and evicted from scheduler
// threads and the main thread concurrently. Byte accounting is exact (the sum
// of Bitmap::sizeBytes()) and the total never exceeds maxBytes(), even for a
// single oversized entry: put() refuses bitmaps whose size alone would not
// fit.
//
// Revision semantics: entries are stamped with the document revision they
// were rendered for. get() with a different revision than the stored one is a
// miss and lazily drops the stale entry (the next successful put re-populates
// it), so a document edit invalidates its tiles without a full sweep.
class TileCache {
public:
    explicit TileCache(std::size_t maxBytes = 256 * 1024 * 1024); // 256 MiB default

    // Shrinking the budget evicts least-recently-used entries immediately.
    void setMaxBytes(std::size_t maxBytes);

    std::size_t maxBytes() const;

    // Returns false (stores nothing) when bitmap is null or invalid, or when
    // its sizeBytes() alone exceeds maxBytes. Re-putting an existing key
    // replaces the entry (byte accounting updated) and promotes it to
    // most-recently-used.
    bool put(const TileKey& key, std::uint64_t revision, std::shared_ptr<const core::Bitmap> bitmap);

    // nullptr on miss or stale revision; a hit is promoted to
    // most-recently-used. The returned shared_ptr keeps the bitmap alive even
    // if the entry is evicted afterwards.
    std::shared_ptr<const core::Bitmap> get(const TileKey& key, std::uint64_t revision) const;

    void remove(const TileKey& key);
    void clear();

    std::size_t sizeBytes() const; // exact: sum of bitmap->sizeBytes()
    std::size_t count() const;

private:
    struct Entry {
        std::uint64_t revision = 0;
        std::shared_ptr<const core::Bitmap> bitmap;
    };

    // Front of the list is the most recently used entry.
    using List = std::list<std::pair<TileKey, Entry>>;

    // Drops the least-recently-used entry. Precondition: the list is not
    // empty and the callers hold mutex_.
    void evictBack();

    mutable std::mutex mutex_;
    mutable List lru_; // front = most recently used (get() promotes entries)
    mutable std::unordered_map<TileKey, List::iterator> index_;
    std::size_t maxBytes_;
    mutable std::size_t sizeBytes_ = 0; // invariant: sizeBytes_ <= maxBytes_
};

} // namespace rivet::render
