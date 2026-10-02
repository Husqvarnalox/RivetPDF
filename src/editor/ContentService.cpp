// SPDX-License-Identifier: MPL-2.0
#include "editor/ContentService.hpp"

#include "editor/ContentGeometry.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/TextBlocks.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_set>

namespace rivet::editor {
namespace {

struct SourceKey {
    core::PageId page;
    std::uint32_t index = 0;
    bool operator<(const SourceKey& o) const { return page != o.page ? page < o.page : index < o.index; }
};

struct CreatedKey {
    core::PageId page;
    std::uint64_t tag = 0;
    std::uint32_t ordinal = 0;
    bool operator<(const CreatedKey& o) const {
        if (page != o.page) return page < o.page;
        if (tag != o.tag) return tag < o.tag;
        return ordinal < o.ordinal;
    }
};

struct CacheKey {
    const pdf::PdfDocument* document = nullptr;
    std::size_t pageIndex = 0;
    const pdf::PdfPageContentEdits* edits = nullptr;
    bool operator==(const CacheKey&) const = default;
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        std::size_t h = std::hash<const void*>{}(key.document);
        h = h * 1000003u ^ std::hash<std::size_t>{}(key.pageIndex);
        h = h * 1000003u ^ std::hash<const void*>{}(key.edits);
        return h;
    }
};

// What a failed or unsupported extraction caches: nothing editable.
pdf::PdfPageContentPtr unavailableContent() {
    auto content = std::make_shared<pdf::PdfPageContent>();
    content->regenerationSafe = false;
    content->regenerationIssue = "the page content is not available";
    return content;
}

core::Point toDisplay(const pdf::PdfPageView& view, const pdf::PdfPoint& p) {
    return pdf::userToDisplay(view, p.x, p.y);
}

} // namespace

struct ContentService::Impl {
    explicit Impl(DocumentSession& s)
        : session(s), dispatcher(s.mainDispatcher()), executor(s.scheduler()) {}

    // --- Extraction cache (mutex-guarded; shared with the worker) ------------

    struct Cached {
        std::list<CacheKey>::iterator lru;
        std::weak_ptr<pdf::PdfDocument> document;
        std::weak_ptr<const pdf::PdfPageContentEdits> edits;
        pdf::PdfPageContentPtr content;
        std::size_t sourceObjectCount = 0;
    };
    struct Pending {
        std::uint64_t token = 0;
        std::weak_ptr<pdf::PdfDocument> document;
        std::weak_ptr<const pdf::PdfPageContentEdits> edits;
        std::vector<core::PageId> pages;
    };
    struct Loaded {
        pdf::PdfPageContentPtr content;
        std::size_t sourceObjectCount = 0;
    };

    DocumentSession& session;
    core::IMainThreadDispatcher* dispatcher;
    core::SerialExecutor executor;
    std::shared_ptr<std::atomic<bool>> liveness = std::make_shared<std::atomic<bool>>(true);
    std::function<void(core::PageId)> onChanged;

    mutable std::mutex mutex;
    mutable std::list<CacheKey> lru;
    mutable std::unordered_map<CacheKey, Cached, CacheKeyHash> cache;
    mutable std::unordered_map<CacheKey, Pending, CacheKeyHash> pending;
    mutable std::uint64_t nextToken = 0;

    // --- Main-thread state --------------------------------------------------

    mutable std::map<SourceKey, core::ObjectId> sourceIds;
    mutable std::map<CreatedKey, core::ObjectId> createdIds;

    struct ResolvedEntry {
        std::weak_ptr<const pdf::PdfPageContentEdits> edits;
        pdf::PdfPageView view;
        pdf::PdfPageContentPtr content; // null while not loaded
        PageContentViewPtr result;
    };
    mutable std::unordered_map<core::PageId, ResolvedEntry> resolved;

    // ------------------------------------------------------------------------

    static CacheKey keyOf(const PageEntry& entry) {
        return CacheKey{entry.source.get(), entry.sourcePageIndex, entry.contentEdits.get()};
    }

    // Caller holds `mutex`.
    bool cachedMatches(const Cached& cached, const CacheKey& key) const {
        return cached.document.lock().get() == key.document && cached.edits.lock().get() == key.edits;
    }

    // Caller holds `mutex`. Touches the LRU on a hit; drops a stale (ABA) entry.
    std::optional<Loaded> lookupLocked(const CacheKey& key) const {
        const auto it = cache.find(key);
        if (it == cache.end()) return std::nullopt;
        if (!cachedMatches(it->second, key)) {
            lru.erase(it->second.lru);
            cache.erase(it);
            return std::nullopt;
        }
        lru.splice(lru.begin(), lru, it->second.lru);
        return Loaded{it->second.content, it->second.sourceObjectCount};
    }

    // Caller holds `mutex`.
    void insertLocked(const CacheKey& key, const std::weak_ptr<pdf::PdfDocument>& document,
                      const std::weak_ptr<const pdf::PdfPageContentEdits>& edits, Loaded loaded) const {
        if (const auto old = cache.find(key); old != cache.end()) {
            lru.erase(old->second.lru);
            cache.erase(old);
        }
        while (cache.size() >= kMaxCachedPages && !lru.empty()) {
            cache.erase(lru.back());
            lru.pop_back();
        }
        lru.push_front(key);
        cache[key] = Cached{lru.begin(), document, edits, std::move(loaded.content), loaded.sourceObjectCount};
    }

    std::optional<Loaded> lookup(const PageEntry& entry, bool request) const {
        if (entry.source == nullptr) return std::nullopt;
        const CacheKey key = keyOf(entry);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (auto hit = lookupLocked(key)) return hit;
        }
        if (request) requestLoad(entry);
        return std::nullopt;
    }

    void requestLoad(const PageEntry& entry) const {
        const CacheKey key = keyOf(entry);
        std::uint64_t token = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = pending.find(key);
            if (it != pending.end() && it->second.document.lock().get() == key.document &&
                it->second.edits.lock().get() == key.edits) {
                auto& pages = it->second.pages;
                if (std::find(pages.begin(), pages.end(), entry.id) == pages.end()) pages.push_back(entry.id);
                return; // coalesced onto the load in flight
            }
            token = ++nextToken;
            pending[key] = Pending{token, entry.source, entry.contentEdits, {entry.id}};
        }
        // The job owns the document and the edits while it reads; the
        // delivery only carries weak references, so a document nobody else
        // references any more is dropped.
        const_cast<core::SerialExecutor&>(executor).post(
            [this, alive = liveness, document = entry.source, edits = entry.contentEdits,
             pageIndex = entry.sourcePageIndex, key, token]() mutable {
                runExtraction(std::move(document), std::move(edits), pageIndex, key, token, alive);
            });
    }

    static pdf::PdfPageContentPtr extractOne(const pdf::PdfDocument& document, std::size_t pageIndex,
                                             const pdf::PdfPageContentEditsPtr& edits) {
        try {
            auto result = document.pageContent(pageIndex, edits);
            if (result.has_value() && *result != nullptr) return *result;
        } catch (const std::exception&) {
        } catch (...) {
        }
        return nullptr;
    }

    static std::size_t objectCountOf(const pdf::PdfPageContent& content) {
        return content.truncated ? 0 : content.objects.size();
    }

    void runExtraction(std::shared_ptr<pdf::PdfDocument> document, pdf::PdfPageContentEditsPtr edits,
                       std::size_t pageIndex, const CacheKey& key, std::uint64_t token,
                       const std::shared_ptr<std::atomic<bool>>& aliveFlag) const {
        Loaded edited;
        pdf::PdfPageContentPtr originalsToCache;
        const CacheKey originalsKey{key.document, key.pageIndex, nullptr};
        if (edits == nullptr || edits->empty()) {
            pdf::PdfPageContentPtr content = extractOne(*document, pageIndex, nullptr);
            if (content != nullptr) {
                edited = Loaded{content, objectCountOf(*content)};
            } else {
                edited = Loaded{unavailableContent(), 0};
            }
        } else {
            std::optional<Loaded> originals;
            {
                std::lock_guard<std::mutex> lock(mutex);
                originals = lookupLocked(originalsKey);
            }
            if (!originals.has_value() || originals->content == nullptr) {
                pdf::PdfPageContentPtr content = extractOne(*document, pageIndex, nullptr);
                if (content != nullptr) {
                    originals = Loaded{content, objectCountOf(*content)};
                    originalsToCache = content;
                } else {
                    originals.reset();
                }
            }
            pdf::PdfPageContentPtr content;
            if (originals.has_value()) content = extractOne(*document, pageIndex, edits);
            edited = content != nullptr ? Loaded{content, originals->sourceObjectCount}
                                        : Loaded{unavailableContent(), 0};
        }
        const std::weak_ptr<pdf::PdfDocument> weakDocument = document;
        const std::weak_ptr<const pdf::PdfPageContentEdits> weakEdits = edits;
        document.reset();
        edits.reset();
        if (dispatcher == nullptr) {
            completeLoad(key, token, weakDocument, weakEdits, std::move(edited), std::move(originalsToCache));
            return;
        }
        dispatcher->post([this, aliveFlag, key, token, weakDocument, weakEdits, edited = std::move(edited),
                          originalsToCache = std::move(originalsToCache)]() mutable {
            if (!aliveFlag->load(std::memory_order_acquire)) return;
            completeLoad(key, token, weakDocument, weakEdits, std::move(edited), std::move(originalsToCache));
        });
    }

    void completeLoad(const CacheKey& key, std::uint64_t token, const std::weak_ptr<pdf::PdfDocument>& document,
                      const std::weak_ptr<const pdf::PdfPageContentEdits>& edits, Loaded edited,
                      pdf::PdfPageContentPtr originals) const {
        std::vector<core::PageId> pages;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto it = pending.find(key);
            if (it == pending.end() || it->second.token != token) return; // cancelled / superseded
            pages = std::move(it->second.pages);
            pending.erase(it);
            const std::shared_ptr<pdf::PdfDocument> keepDocument = document.lock();
            if (keepDocument == nullptr || keepDocument.get() != key.document) return; // nobody references it
            if (key.edits != nullptr) {
                const auto keepEdits = edits.lock();
                if (keepEdits == nullptr || keepEdits.get() != key.edits) return; // superseded edits
            }
            if (originals != nullptr) {
                const CacheKey originalsKey{key.document, key.pageIndex, nullptr};
                const std::size_t count = objectCountOf(*originals);
                insertLocked(originalsKey, document, {}, Loaded{std::move(originals), count});
            }
            insertLocked(key, document, edits, std::move(edited));
        }
        if (!onChanged) return;
        const auto callback = onChanged; // may replace itself
        for (const core::PageId page : pages) callback(page);
    }

    // --- Identity ----------------------------------------------------------

    core::ObjectId sourceId(core::PageId page, std::uint32_t index) const {
        const SourceKey key{page, index};
        if (const auto it = sourceIds.find(key); it != sourceIds.end()) return it->second;
        const core::ObjectId id = session.pageModel().mintObjectId();
        sourceIds.emplace(key, id);
        return id;
    }

    // Created objects: ordinal 0 of an edit's objects is the block's first
    // member and carries the block's id, which IS the tag (ADR-0014); later
    // ordinals get minted ids. `used` holds the ids already taken on the page
    // (a reused source member may already carry the tag's id).
    core::ObjectId createdId(core::PageId page, std::uint64_t tag, std::uint32_t ordinal,
                             std::unordered_set<std::uint64_t>& used) const {
        const CreatedKey key{page, tag, ordinal};
        if (const auto it = createdIds.find(key); it != createdIds.end() && !used.contains(it->second.value())) {
            used.insert(it->second.value());
            return it->second;
        }
        core::ObjectId id = ordinal == 0 ? core::ObjectId{tag} : session.pageModel().mintObjectId();
        if (used.contains(id.value())) id = session.pageModel().mintObjectId();
        createdIds[key] = id;
        used.insert(id.value());
        return id;
    }

    // ids parallel to content.objects.
    std::vector<core::ObjectId> assignIds(core::PageId page, const pdf::PdfPageContent& content) const {
        std::vector<core::ObjectId> ids(content.objects.size());
        std::unordered_set<std::uint64_t> used;
        used.reserve(content.objects.size() * 2);
        for (std::size_t i = 0; i < content.objects.size(); ++i) {
            const pdf::PdfContentObject& o = content.objects[i];
            if (o.origin.kind != pdf::PdfContentOrigin::Kind::Source) continue;
            ids[i] = sourceId(page, o.origin.sourceIndex);
            used.insert(ids[i].value());
        }
        std::map<std::uint64_t, std::uint32_t> ordinals;
        for (std::size_t i = 0; i < content.objects.size(); ++i) {
            const pdf::PdfContentObject& o = content.objects[i];
            if (o.origin.kind != pdf::PdfContentOrigin::Kind::Created) continue;
            const std::uint32_t ordinal = ordinals[o.origin.tag]++;
            ids[i] = createdId(page, o.origin.tag, ordinal, used);
        }
        return ids;
    }

    // --- Resolution ----------------------------------------------------------

    static bool objectEdited(const pdf::PdfContentObject& o, const pdf::PdfPageContentEdits* edits) {
        if (o.origin.kind == pdf::PdfContentOrigin::Kind::Created || o.blockTag != 0) return true;
        if (edits == nullptr) return false;
        const auto it = std::lower_bound(
            edits->objects.begin(), edits->objects.end(), o.origin.sourceIndex,
            [](const pdf::PdfObjectEdit& edit, std::uint32_t index) { return edit.sourceIndex < index; });
        return it != edits->objects.end() && it->sourceIndex == o.origin.sourceIndex;
    }

    PageContentViewPtr resolve(const PageEntry& entry, const Loaded& loaded) const {
        auto view = std::make_shared<PageContentView>();
        const pdf::PdfPageContent& content = *loaded.content;
        view->loaded = true;
        view->truncated = content.truncated;
        view->regenerationSafe = content.regenerationSafe;
        view->regenerationIssue = content.regenerationIssue;
        view->sourceObjectCount = loaded.sourceObjectCount;

        const std::vector<ObjectCapabilityInfo> capabilities = classifyObjects(content);
        const std::vector<core::ObjectId> ids = assignIds(entry.id, content);
        const std::vector<ReconstructedBlock> blocks = reconstructTextBlocks(content, capabilities);
        std::vector<core::ObjectId> blockOf(content.objects.size());
        for (const ReconstructedBlock& block : blocks) {
            const core::ObjectId blockId = ids[block.firstMember()];
            for (const ReconstructedLine& line : block.lines) {
                for (std::uint32_t member : line.members) blockOf[member] = blockId;
            }
        }

        const pdf::PdfPageView& pageView = entry.view;
        view->objects.reserve(content.objects.size());
        for (std::size_t i = 0; i < content.objects.size(); ++i) {
            const pdf::PdfContentObject& o = content.objects[i];
            ContentObjectView ov;
            ov.id = ids[i];
            ov.index = o.index;
            ov.type = o.type;
            ov.bounds = geometry::boxToDisplay(pageView, o.bounds);
            ov.quad = geometry::quadToDisplay(pageView, o.quad);
            ov.capability = capabilities[i].capability;
            ov.capabilityReason = capabilities[i].reason;
            ov.edited = objectEdited(o, entry.contentEdits.get());
            ov.block = blockOf[i];
            ov.pixelWidth = o.pixelWidth;
            ov.pixelHeight = o.pixelHeight;
            ov.source = o;
            view->objects.push_back(std::move(ov));
        }

        view->blocks.reserve(blocks.size());
        for (const ReconstructedBlock& block : blocks) {
            TextBlockView bv;
            bv.id = ids[block.firstMember()];
            bv.tag = block.tag;
            bv.text = block.text;
            bv.bounds = geometry::boxToDisplay(pageView, block.bounds);
            bv.quad = geometry::quadToDisplay(pageView, block.frame);
            const double cosA = std::cos(block.angleRadians);
            const double sinA = std::sin(block.angleRadians);
            bv.rotationDegrees = geometry::displayAngleDegrees(pageView, core::Matrix{cosA, sinA, -sinA, cosA, 0.0, 0.0});
            bv.fontSize = block.fontSize;
            bv.lineAdvance = block.lineAdvance;
            bv.wrapWidth = block.wrapWidth;
            const pdf::PdfContentObject& first = content.objects[block.firstMember()];
            bv.font = first.font;
            bv.color = first.fill.value_or(pdf::PdfColor{});
            bv.fontSubstituted = block.fontSubstituted;
            bv.capability = block.capability;
            bv.capabilityReason = block.capabilityReason;
            bool edited = block.tag != 0;
            for (const ReconstructedLine& line : block.lines) {
                TextBlockLine out;
                out.text = line.text;
                out.baselineStart = toDisplay(pageView, line.baselineStart);
                out.baselineEnd = toDisplay(pageView, line.baselineEnd);
                out.members.reserve(line.members.size());
                for (std::uint32_t member : line.members) {
                    out.members.push_back(ids[member]);
                    edited = edited || view->objects[member].edited;
                }
                bv.lines.push_back(std::move(out));
            }
            bv.edited = edited;
            view->blocks.push_back(std::move(bv));
        }
        return view;
    }

    PageContentViewPtr contentOf(core::PageId pageId) const {
        const PageSnapshotPtr snapshot = session.pageSnapshot();
        const PageEntry* entry = snapshot->find(pageId);
        if (entry == nullptr) return std::make_shared<const PageContentView>();
        const std::optional<Loaded> loaded = lookup(*entry, true);
        const pdf::PdfPageContentPtr contentPtr = loaded.has_value() ? loaded->content : nullptr;
        if (const auto it = resolved.find(pageId); it != resolved.end()) {
            const ResolvedEntry& cached = it->second;
            if (cached.view == entry->view && cached.content == contentPtr &&
                cached.edits.lock().get() == entry->contentEdits.get()) {
                return cached.result;
            }
        }
        PageContentViewPtr result;
        if (loaded.has_value()) {
            result = resolve(*entry, *loaded);
        } else {
            // Not loaded for the CURRENT edits: keep showing the previous
            // objects (no flicker while the backend re-extracts) but refuse
            // edits (loaded = false).
            // (Copy-constructed rather than assigned into a fresh view: GCC's
            // -Wnull-dereference misfires on the inlined vector assignment.)
            std::shared_ptr<PageContentView> stale;
            if (const auto it = resolved.find(pageId); it != resolved.end() && it->second.result != nullptr &&
                it->second.view == entry->view) {
                stale = std::make_shared<PageContentView>(*it->second.result);
            } else {
                stale = std::make_shared<PageContentView>();
            }
            stale->loaded = false;
            result = std::move(stale);
        }
        resolved[pageId] = ResolvedEntry{entry->contentEdits, entry->view, contentPtr, result};
        return result;
    }
};

// ---------------------------------------------------------------------------

ContentService::ContentService(DocumentSession& session) : impl_(std::make_unique<Impl>(session)) {}

ContentService::~ContentService() {
    impl_->liveness->store(false, std::memory_order_release);
    impl_->executor.cancelPending();
    impl_->executor.waitUntilIdle();
}

void ContentService::setOnChanged(std::function<void(core::PageId)> onChanged) {
    impl_->onChanged = std::move(onChanged);
}

PageContentViewPtr ContentService::content(core::PageId pageId) const { return impl_->contentOf(pageId); }

std::optional<ContentObjectView> ContentService::findObject(core::PageId pageId, core::ObjectId id) const {
    const PageContentViewPtr view = content(pageId);
    for (const ContentObjectView& object : view->objects) {
        if (object.id == id) return object;
    }
    return std::nullopt;
}

std::optional<TextBlockView> ContentService::findBlock(core::PageId pageId, core::ObjectId blockId) const {
    const PageContentViewPtr view = content(pageId);
    for (const TextBlockView& block : view->blocks) {
        if (block.id == blockId) return block;
    }
    return std::nullopt;
}

std::optional<ContentHit> ContentService::hitTest(core::PageId pageId, core::Point display, double tolerancePoints,
                                                  bool editableOnly) const {
    const PageContentViewPtr view = content(pageId);
    if (!view->loaded || !display.isFinite()) return std::nullopt;
    const double tolerance = std::isfinite(tolerancePoints) ? std::max(0.0, tolerancePoints) : 0.0;
    const auto blockEditable = [&](core::ObjectId blockId) {
        for (const TextBlockView& block : view->blocks) {
            if (block.id == blockId) return block.capability != ContentCapability::ReadOnly;
        }
        return false;
    };
    for (auto it = view->objects.rbegin(); it != view->objects.rend(); ++it) {
        const ContentObjectView& object = *it;
        if (editableOnly && object.capability == ContentCapability::ReadOnly) continue;
        if (!geometry::pointInConvexQuad(display, object.quad, tolerance)) continue;
        if (object.block) {
            if (editableOnly && !blockEditable(object.block)) continue;
            return ContentHit{object.block, true};
        }
        return ContentHit{object.id, false};
    }
    // Gaps between the lines of a paragraph still select the block.
    for (auto it = view->blocks.rbegin(); it != view->blocks.rend(); ++it) {
        if (editableOnly && it->capability == ContentCapability::ReadOnly) continue;
        if (geometry::pointInConvexQuad(display, it->quad, tolerance)) return ContentHit{it->id, true};
    }
    return std::nullopt;
}

void ContentService::evictPages(std::span<const core::PageId> pageIds) {
    for (const core::PageId id : pageIds) impl_->resolved.erase(id);
}

void ContentService::rebased(std::span<const PageRekey> pages) {
    Impl& impl = *impl_;
    {
        std::lock_guard<std::mutex> lock(impl.mutex);
        impl.cache.clear();
        impl.lru.clear();
        impl.pending.clear(); // in-flight extractions belong to the previous documents
    }
    impl.resolved.clear();
    for (const PageRekey& rekey : pages) {
        std::map<std::uint32_t, core::ObjectId> sources;
        std::map<std::pair<std::uint64_t, std::uint32_t>, core::ObjectId> created;
        for (auto it = impl.sourceIds.lower_bound(SourceKey{rekey.page, 0});
             it != impl.sourceIds.end() && it->first.page == rekey.page;) {
            sources.emplace(it->first.index, it->second);
            it = impl.sourceIds.erase(it);
        }
        for (auto it = impl.createdIds.lower_bound(CreatedKey{rekey.page, 0, 0});
             it != impl.createdIds.end() && it->first.page == rekey.page;) {
            created.emplace(std::make_pair(it->first.tag, it->first.ordinal), it->second);
            it = impl.createdIds.erase(it);
        }
        if (!rekey.keep) continue;
        if (rekey.origins.empty()) {
            // No edits: object i stays object i.
            for (const auto& [index, id] : sources) impl.sourceIds[SourceKey{rekey.page, index}] = id;
            for (const auto& [key, id] : created) {
                impl.createdIds[CreatedKey{rekey.page, key.first, key.second}] = id;
            }
            continue;
        }
        // Two passes like assignIds: source objects first, created second.
        std::unordered_set<std::uint64_t> used;
        std::vector<core::ObjectId> assigned(rekey.origins.size());
        for (std::size_t i = 0; i < rekey.origins.size(); ++i) {
            const pdf::PdfContentOrigin& origin = rekey.origins[i];
            if (origin.kind != pdf::PdfContentOrigin::Kind::Source) continue;
            const auto found = sources.find(origin.sourceIndex);
            if (found == sources.end() || used.contains(found->second.value())) continue;
            assigned[i] = found->second;
            used.insert(found->second.value());
        }
        std::map<std::uint64_t, std::uint32_t> ordinals;
        for (std::size_t i = 0; i < rekey.origins.size(); ++i) {
            const pdf::PdfContentOrigin& origin = rekey.origins[i];
            if (origin.kind != pdf::PdfContentOrigin::Kind::Created) continue;
            const std::uint32_t ordinal = ordinals[origin.tag]++;
            core::ObjectId id;
            if (const auto found = created.find({origin.tag, ordinal}); found != created.end()) {
                id = found->second;
            } else if (ordinal == 0) {
                id = core::ObjectId{origin.tag};
            }
            if (!id || used.contains(id.value())) continue;
            assigned[i] = id;
            used.insert(id.value());
        }
        for (std::size_t i = 0; i < assigned.size(); ++i) {
            if (assigned[i]) impl.sourceIds[SourceKey{rekey.page, static_cast<std::uint32_t>(i)}] = assigned[i];
        }
    }
}

std::size_t ContentService::cachedPages() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->cache.size();
}

std::size_t ContentService::registrySize() const { return impl_->sourceIds.size() + impl_->createdIds.size(); }

} // namespace rivet::editor
