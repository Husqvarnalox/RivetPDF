#include "render/TileCache.hpp"

#include <iterator>
#include <utility>

namespace rivet::render {

TileCache::TileCache(std::size_t maxBytes) : maxBytes_(maxBytes) {}

void TileCache::setMaxBytes(std::size_t maxBytes) {
    std::lock_guard lock(mutex_);
    maxBytes_ = maxBytes;
    while (sizeBytes_ > maxBytes_) {
        evictBack();
    }
}

std::size_t TileCache::maxBytes() const {
    std::lock_guard lock(mutex_);
    return maxBytes_;
}

bool TileCache::put(const TileKey& key, std::uint64_t revision,
                    std::shared_ptr<const core::Bitmap> bitmap) {
    if (!bitmap || !bitmap->isValid()) return false;
    const std::size_t bytes = bitmap->sizeBytes();

    std::lock_guard lock(mutex_);
    // Hard byte bound: a single entry may never exceed the budget.
    if (bytes > maxBytes_) return false;

    if (const auto it = index_.find(key); it != index_.end()) {
        const List::iterator node = it->second;
        sizeBytes_ -= node->second.bitmap->sizeBytes();
        lru_.splice(lru_.begin(), lru_, node);
        node->second.revision = revision;
        node->second.bitmap = std::move(bitmap);
        // The new bytes are not counted yet, so this can only evict the
        // other entries; the node being replaced is at the front and the
        // loop stops before the list runs empty (bytes <= maxBytes_).
        while (bytes > maxBytes_ - sizeBytes_) {
            evictBack();
        }
        sizeBytes_ += bytes;
        return true;
    }

    while (bytes > maxBytes_ - sizeBytes_) {
        evictBack();
    }
    lru_.emplace_front(key, Entry{revision, std::move(bitmap)});
    index_.emplace(key, lru_.begin());
    sizeBytes_ += bytes;
    return true;
}

std::shared_ptr<const core::Bitmap> TileCache::get(const TileKey& key, std::uint64_t revision) const {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) return nullptr;

    const List::iterator node = it->second;
    if (node->second.revision != revision) {
        // Stale: rendered for a different document revision. Miss and drop.
        sizeBytes_ -= node->second.bitmap->sizeBytes();
        lru_.erase(node);
        index_.erase(it);
        return nullptr;
    }

    lru_.splice(lru_.begin(), lru_, node); // promote to most-recently-used
    return node->second.bitmap;
}

void TileCache::remove(const TileKey& key) {
    std::lock_guard lock(mutex_);
    const auto it = index_.find(key);
    if (it == index_.end()) return;

    const List::iterator node = it->second;
    sizeBytes_ -= node->second.bitmap->sizeBytes();
    lru_.erase(node);
    index_.erase(it);
}

void TileCache::clear() {
    std::lock_guard lock(mutex_);
    lru_.clear();
    index_.clear();
    sizeBytes_ = 0;
}

std::size_t TileCache::sizeBytes() const {
    std::lock_guard lock(mutex_);
    return sizeBytes_;
}

std::size_t TileCache::count() const {
    std::lock_guard lock(mutex_);
    return index_.size();
}

void TileCache::evictBack() {
    const List::iterator last = std::prev(lru_.end());
    sizeBytes_ -= last->second.bitmap->sizeBytes();
    index_.erase(last->first);
    lru_.erase(last);
}

} // namespace rivet::render
