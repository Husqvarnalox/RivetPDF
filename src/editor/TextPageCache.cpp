// SPDX-License-Identifier: MPL-2.0
#include "editor/TextPageCache.hpp"

#include <algorithm>
#include <utility>

namespace rivet::editor {

TextPageCache::TextPageCache(std::size_t maxBytes) : maxBytes_(maxBytes) {}

void TextPageCache::setMaxBytes(std::size_t maxBytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxBytes_ = maxBytes;
    while (sizeBytes_ > maxBytes_ && !lru_.empty()) {
        evictBack();
    }
}

std::size_t TextPageCache::maxBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return maxBytes_;
}

bool TextPageCache::put(core::PageId pageId, std::uint64_t revision,
                        std::shared_ptr<const pdf::PdfTextPage> page) {
    if (page == nullptr) return false;
    const std::size_t bytes = page->memoryBytes();

    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > maxBytes_) return false;
    const auto it = index_.find(pageId);
    if (it != index_.end()) {
        sizeBytes_ -= it->second->second.page->memoryBytes();
        lru_.erase(it->second);
        index_.erase(it);
    }
    while (sizeBytes_ + bytes > maxBytes_ && !lru_.empty()) {
        evictBack();
    }
    lru_.push_front({pageId, Entry{revision, std::move(page)}});
    index_[pageId] = lru_.begin();
    sizeBytes_ += bytes;
    return true;
}

std::shared_ptr<const pdf::PdfTextPage> TextPageCache::get(core::PageId pageId,
                                                           std::uint64_t revision) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(pageId);
    if (it == index_.end()) return nullptr;
    if (it->second->second.revision != revision) {
        // Stale: the document changed under this entry.
        sizeBytes_ -= it->second->second.page->memoryBytes();
        lru_.erase(it->second);
        index_.erase(it);
        return nullptr;
    }
    lru_.splice(lru_.begin(), lru_, it->second); // promote to MRU
    return it->second->second.page;
}

void TextPageCache::remove(core::PageId pageId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = index_.find(pageId);
    if (it == index_.end()) return;
    sizeBytes_ -= it->second->second.page->memoryBytes();
    lru_.erase(it->second);
    index_.erase(it);
}

void TextPageCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
    sizeBytes_ = 0;
}

std::size_t TextPageCache::sizeBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sizeBytes_;
}

std::size_t TextPageCache::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lru_.size();
}

void TextPageCache::evictBack() {
    sizeBytes_ -= lru_.back().second.page->memoryBytes();
    index_.erase(lru_.back().first);
    lru_.pop_back();
}

} // namespace rivet::editor
