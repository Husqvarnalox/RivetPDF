// SPDX-License-Identifier: MPL-2.0
#include "editor/AnnotationService.hpp"

#include "editor/AnnotationGeometry.hpp"
#include "editor/DocumentSession.hpp"

#include <algorithm>
#include <exception>

namespace rivet::editor {

unsigned annotationCaps(pdf::PdfAnnotationKind kind) {
    switch (kind) {
    case pdf::PdfAnnotationKind::Highlight:
    case pdf::PdfAnnotationKind::Underline:
    case pdf::PdfAnnotationKind::StrikeOut:
        return kCapRestyle | kCapEditContents | kCapDelete;
    case pdf::PdfAnnotationKind::Note:
        return kCapMove | kCapRestyle | kCapEditContents | kCapDelete;
    case pdf::PdfAnnotationKind::Ink:
    case pdf::PdfAnnotationKind::Square:
    case pdf::PdfAnnotationKind::Circle:
    case pdf::PdfAnnotationKind::Line:
    case pdf::PdfAnnotationKind::Arrow:
    case pdf::PdfAnnotationKind::Stamp:
        return kCapMove | kCapResize | kCapRestyle | kCapDelete;
    case pdf::PdfAnnotationKind::Other:
        break;
    }
    return 0;
}

AnnotationService::AnnotationService(DocumentSession& session)
    : session_(session), dispatcher_(session.mainDispatcher()), executor_(session.scheduler()) {}

AnnotationService::~AnnotationService() {
    alive_->store(false, std::memory_order_release);
    executor_.cancelPending();
    executor_.waitUntilIdle();
}

void AnnotationService::setOnChanged(std::function<void(core::PageId)> onChanged) {
    onChanged_ = std::move(onChanged);
}

// --- Originals cache ---------------------------------------------------------

pdf::PdfPageAnnotationsPtr AnnotationService::originalsFor(const PageEntry& entry, bool request) const {
    if (entry.source == nullptr) return nullptr;
    const Key key{entry.source.get(), entry.sourcePageIndex};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            // ABA guard: the cached entry must belong to THIS document, not
            // to a destroyed one whose address was reused.
            if (it->second.document.lock().get() == entry.source.get()) {
                lru_.splice(lru_.begin(), lru_, it->second.lru);
                return it->second.annotations;
            }
            lru_.erase(it->second.lru);
            cache_.erase(it);
        }
    }
    if (request) requestLoad(entry);
    return nullptr;
}

void AnnotationService::requestLoad(const PageEntry& entry) const {
    const Key key{entry.source.get(), entry.sourcePageIndex};
    std::uint64_t token = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(key);
        if (it != pending_.end() && it->second.document.lock().get() == entry.source.get()) {
            auto& pages = it->second.pages;
            if (std::find(pages.begin(), pages.end(), entry.id) == pages.end()) pages.push_back(entry.id);
            return; // coalesced onto the load in flight
        }
        token = ++nextToken_;
        pending_[key] = Pending{token, entry.source, {entry.id}};
    }
    const std::size_t pageIndex = entry.sourcePageIndex;
    // The job owns the document while it reads; the delivery only carries a
    // weak reference, so a document nobody else references any more is dropped.
    const_cast<core::SerialExecutor&>(executor_).post(
        [this, alive = alive_, document = entry.source, pageIndex, key, token]() mutable {
            pdf::PdfPageAnnotationsPtr result;
            try {
                auto loaded = document->annotations(pageIndex);
                if (loaded.has_value()) result = std::move(*loaded);
            } catch (const std::exception&) {
            } catch (...) {
            }
            // Load error / NotAvailable: the page just has no editable originals.
            if (result == nullptr) result = std::make_shared<const pdf::PdfPageAnnotations>();
            const std::weak_ptr<pdf::PdfDocument> weak = document;
            document.reset();
            if (dispatcher_ == nullptr) {
                completeLoad(key, token, weak, std::move(result));
                return;
            }
            dispatcher_->post([this, alive, key, token, weak, result = std::move(result)]() mutable {
                if (!alive->load(std::memory_order_acquire)) return;
                completeLoad(key, token, weak, std::move(result));
            });
        });
}

void AnnotationService::completeLoad(const Key& key, std::uint64_t token,
                                     const std::weak_ptr<pdf::PdfDocument>& document,
                                     pdf::PdfPageAnnotationsPtr annotations) const {
    std::vector<core::PageId> pages;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto pending = pending_.find(key);
        if (pending == pending_.end() || pending->second.token != token) return; // cancelled / superseded
        pages = std::move(pending->second.pages);
        pending_.erase(pending);
        const std::shared_ptr<pdf::PdfDocument> alive = document.lock();
        if (alive == nullptr || alive.get() != key.document) return; // nobody references it any more
        if (const auto old = cache_.find(key); old != cache_.end()) {
            lru_.erase(old->second.lru);
            cache_.erase(old);
        }
        while (cache_.size() >= kMaxCachedPages && !lru_.empty()) {
            cache_.erase(lru_.back());
            lru_.pop_back();
        }
        lru_.push_front(key);
        cache_[key] = Cached{lru_.begin(), document, std::move(annotations)};
    }
    if (!onChanged_) return;
    const auto callback = onChanged_; // may replace itself
    for (const core::PageId page : pages) callback(page);
}

std::size_t AnnotationService::cachedOriginalPages() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

// --- Registry / resolution -----------------------------------------------------

core::AnnotationId AnnotationService::registryId(core::PageId page, std::uint32_t index) const {
    const RegistryKey key{page, index};
    if (const auto it = registry_.find(key); it != registry_.end()) return it->second;
    const core::AnnotationId id = session_.pageModel().mintAnnotationId();
    registry_.emplace(key, id);
    registryById_.emplace(id, key);
    return id;
}

AnnotationService::Resolved AnnotationService::resolve(const PageEntry& entry,
                                                       const pdf::PdfPageAnnotationsPtr& originals) const {
    auto list = std::make_shared<std::vector<AnnotationView>>();
    const PageAnnotationState* state = entry.annotations.get();
    if (originals != nullptr) {
        for (const pdf::PdfPageAnnotation& item : originals->items) {
            if (!item.editable || item.isPopup) continue;
            if (state != nullptr && std::binary_search(state->suppressed.begin(), state->suppressed.end(), item.index)) {
                continue;
            }
            const core::AnnotationId id = registryId(entry.id, item.index);
            list->push_back(geometry::makeAnnotationView(id, item.data, entry.view, false));
        }
    }
    if (state != nullptr) {
        for (const OverlayAnnotation& item : state->overlay) {
            if (item.fileIndex.has_value()) {
                const RegistryKey key{entry.id, *item.fileIndex};
                if (registry_.emplace(key, item.id).second) registryById_.emplace(item.id, key);
            }
            list->push_back(geometry::makeAnnotationView(item.id, item.data, entry.view, true));
        }
    }
    return list;
}

AnnotationService::Resolved AnnotationService::annotations(core::PageId pageId) const {
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr) return std::make_shared<const std::vector<AnnotationView>>();
    const pdf::PdfPageAnnotationsPtr originals = originalsFor(*entry, true);
    const bool loaded = originals != nullptr;
    if (const auto it = resolved_.find(pageId); it != resolved_.end()) {
        const ResolvedEntry& cached = it->second;
        if (cached.state == entry->annotations && cached.contentRevision == entry->contentRevision &&
            cached.originalsLoaded == loaded) {
            return cached.list;
        }
    }
    Resolved list = resolve(*entry, originals);
    resolved_[pageId] = ResolvedEntry{entry->annotations, entry->contentRevision, loaded, list};
    return list;
}

std::optional<AnnotationView> AnnotationService::find(core::PageId pageId, core::AnnotationId id) const {
    const Resolved list = annotations(pageId);
    for (const AnnotationView& view : *list) {
        if (view.id == id) return view;
    }
    return std::nullopt;
}

std::optional<core::AnnotationId> AnnotationService::hitTest(core::PageId pageId, core::Point display,
                                                             double tolerancePoints) const {
    const Resolved list = annotations(pageId);
    for (auto it = list->rbegin(); it != list->rend(); ++it) {
        if (geometry::hitsAnnotation(*it, display, tolerancePoints)) return it->id;
    }
    return std::nullopt;
}

std::optional<AnnotationService::Located> AnnotationService::locate(core::AnnotationId id) const {
    if (!id) return std::nullopt;
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    for (const PageEntry& entry : snapshot->entries()) {
        if (entry.annotations == nullptr) continue;
        const auto& overlay = entry.annotations->overlay;
        for (std::size_t i = 0; i < overlay.size(); ++i) {
            if (overlay[i].id != id) continue;
            Located found;
            found.page = entry.id;
            found.id = id;
            found.overlay = true;
            found.overlayPosition = i;
            found.index = overlay[i].fileIndex;
            found.data = overlay[i].data;
            found.view = entry.view;
            found.state = entry.annotations;
            found.rasterRevision = entry.rasterRevision;
            return found;
        }
    }
    const auto reg = registryById_.find(id);
    if (reg == registryById_.end()) return std::nullopt;
    const PageEntry* entry = snapshot->find(reg->second.page);
    if (entry == nullptr) return std::nullopt;
    const std::uint32_t index = reg->second.index;
    if (entry->annotations != nullptr) {
        const auto& suppressed = entry->annotations->suppressed;
        if (std::binary_search(suppressed.begin(), suppressed.end(), index)) return std::nullopt; // deleted
    }
    const pdf::PdfPageAnnotationsPtr originals = originalsFor(*entry, true);
    if (originals == nullptr) return std::nullopt;
    for (const pdf::PdfPageAnnotation& item : originals->items) {
        if (item.index != index) continue;
        if (!item.editable || item.isPopup) return std::nullopt;
        Located found;
        found.page = entry->id;
        found.id = id;
        found.overlay = false;
        found.index = index;
        found.popupIndex = item.popupIndex;
        found.data = item.data;
        found.view = entry->view;
        found.state = entry->annotations;
        found.rasterRevision = entry->rasterRevision;
        return found;
    }
    return std::nullopt;
}

void AnnotationService::evictPages(std::span<const core::PageId> pageIds) {
    for (const core::PageId id : pageIds) resolved_.erase(id);
}

void AnnotationService::rebased(std::span<const PageRekey> pages) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
        lru_.clear();
        pending_.clear(); // in-flight loads belong to the previous documents
    }
    resolved_.clear();
    for (const PageRekey& rekey : pages) {
        std::vector<std::pair<std::uint32_t, core::AnnotationId>> existing;
        for (auto it = registry_.lower_bound(RegistryKey{rekey.page, 0});
             it != registry_.end() && it->first.page == rekey.page;) {
            existing.emplace_back(it->first.index, it->second);
            registryById_.erase(it->second);
            it = registry_.erase(it);
        }
        if (!rekey.keep) continue;
        for (const auto& [index, id] : existing) {
            const auto below = std::lower_bound(rekey.removed.begin(), rekey.removed.end(), index);
            if (below != rekey.removed.end() && *below == index) continue; // removed from the file
            const auto shift = static_cast<std::uint32_t>(below - rekey.removed.begin());
            const RegistryKey key{rekey.page, index - shift};
            registry_[key] = id;
            registryById_[id] = key;
        }
        for (const auto& [id, index] : rekey.overlayIds) {
            const RegistryKey key{rekey.page, index};
            registry_[key] = id;
            registryById_[id] = key;
        }
    }
}

} // namespace rivet::editor
