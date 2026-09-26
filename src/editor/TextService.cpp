// SPDX-License-Identifier: MPL-2.0
#include "editor/TextService.hpp"

#include "editor/DocumentSession.hpp"

#include <optional>
#include <string>
#include <utility>

namespace rivet::editor {

TextService::TextService(DocumentSession& session)
    : session_(session), dispatcher_(session.mainDispatcher()), executor_(session.scheduler()) {}

TextService::~TextService() {
    // Deliveries already posted become no-ops; a running range job notices
    // the flag between pages. Queued jobs are cancelled (their callbacks
    // dropped without firing); the in-flight job finishes against the
    // document, which is still alive in the session's destruction order.
    alive_->store(false, std::memory_order_release);
    executor_.cancelPending();
    executor_.waitUntilIdle();
}

void TextService::deliver(std::function<void()> task) {
    if (dispatcher_ == nullptr) {
        task();
        return;
    }
    dispatcher_->post([alive = alive_, task = std::move(task)] {
        if (alive->load(std::memory_order_acquire)) task();
    });
}

std::shared_ptr<const pdf::PdfTextPage> TextService::cachedTextPage(core::PageId pageId) const {
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr) return nullptr;
    return cache_.get(pageId, entry->contentRevision);
}

std::shared_ptr<const pdf::PdfTextPage> TextService::extract(const PageEntry& entry) {
    if (entry.source == nullptr) return nullptr;
    return entry.source->textPage(entry.sourcePageIndex, entry.view).value_or(nullptr);
}

void TextService::ensureTextPage(core::PageId pageId) {
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr) return; // not in the document: nothing to warm
    if (cache_.get(pageId, entry->contentRevision) != nullptr) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& callbacks = pending_[{pageId, entry->contentRevision}]; // creates the coalescing entry
        (void)callbacks;
    }
    scheduleExtraction(*entry);
}

void TextService::requestTextPage(core::PageId pageId, TextCallback onDone) {
    if (!onDone) return;
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    const PageEntry* entry = snapshot->find(pageId);
    if (entry == nullptr) {
        // Not in the document (deleted): delivered like a failed extraction.
        deliver([onDone = std::move(onDone)]() mutable { onDone(nullptr); });
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[{pageId, entry->contentRevision}].push_back(std::move(onDone));
    }
    scheduleExtraction(*entry);
}

// Extraction job (dedicated executor stream). Coalescing: the pending entry
// accumulates every callback registered so far; the job takes them all and
// delivers the single outcome. A job posted twice for the same page (two
// requests racing) delivers once per entry - the second job finds no pending
// entry and becomes a no-op.
void TextService::scheduleExtraction(const PageEntry& entry) {
    // The job captures a COPY of the snapshot entry (source document kept
    // alive by its shared_ptr, page index, view, content revision): it never
    // reads session or page-model state while running.
    executor_.post([this, entry] {
        const std::pair<core::PageId, std::uint64_t> key{entry.id, entry.contentRevision};
        std::vector<TextCallback> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_.find(key);
            if (it == pending_.end()) return; // nothing waiting (already delivered)
            callbacks = std::move(it->second);
            pending_.erase(it);
        }

        // Cache probe again: another job may have produced the page.
        auto page = cache_.get(entry.id, entry.contentRevision);
        if (page == nullptr) {
            page = extract(entry);
            if (page != nullptr) cache_.put(entry.id, entry.contentRevision, page);
        }

        if (callbacks.empty()) return;
        deliver([callbacks = std::move(callbacks), page]() mutable {
            for (auto& callback : callbacks) callback(page);
        });
    });
}

void TextService::requestRangesText(std::vector<TextRange> ranges, RangesTextCallback onDone) {
    if (!onDone) return;
    // Resolve everything the job needs on the calling (main) thread from ONE
    // snapshot; the job never reads session state.
    struct Resolved {
        TextRange range;
        std::optional<PageEntry> entry; // nullopt = no longer in the document
        std::size_t readingPosition = 0;
    };
    const PageSnapshotPtr snapshot = session_.pageSnapshot();
    std::vector<Resolved> resolved;
    resolved.reserve(ranges.size());
    for (const TextRange& range : ranges) {
        const std::size_t index = snapshot->indexOf(range.page);
        Resolved item{range, std::nullopt, index};
        if (index != PageModelSnapshot::kInvalidIndex) item.entry = snapshot->at(index);
        resolved.push_back(std::move(item));
    }
    executor_.post([this, resolved = std::move(resolved), onDone = std::move(onDone),
                    alive = alive_]() mutable {
        std::string out;
        core::Result<std::string> result = std::string{};
        for (std::size_t i = 0; i < resolved.size(); ++i) {
            if (!alive->load(std::memory_order_acquire)) return; // owner is going away
            const Resolved& item = resolved[i];
            std::shared_ptr<const pdf::PdfTextPage> page;
            if (item.entry.has_value()) {
                page = cache_.get(item.range.page, item.entry->contentRevision);
                if (page == nullptr) {
                    page = extract(*item.entry);
                    if (page != nullptr) cache_.put(item.range.page, item.entry->contentRevision, page);
                }
            }
            if (page == nullptr) {
                const std::string where = !item.entry.has_value()
                                              ? std::string("a page that is no longer in the document")
                                              : "page " + std::to_string(item.readingPosition + 1);
                result = std::unexpected(core::Error{core::ErrorCode::InvalidDocument,
                                                     "could not read the text of " + where, "editor"});
                break;
            }
            appendRangeText(*page, item.range.begin, item.range.end, out);
            if (i + 1 < resolved.size()) out.push_back('\n'); // page break
        }
        if (result.has_value()) result = std::move(out);
        deliver([onDone = std::move(onDone), result = std::move(result)]() mutable {
            onDone(std::move(result));
        });
    });
}

std::shared_ptr<const pdf::PdfTextPage> TextService::textPageNow(const PageEntry& entry) {
    if (auto cached = cache_.get(entry.id, entry.contentRevision)) return cached;
    auto page = extract(entry);
    if (page != nullptr) cache_.put(entry.id, entry.contentRevision, page);
    return page;
}

void TextService::evictPages(std::span<const core::PageId> pageIds) {
    for (const core::PageId id : pageIds) cache_.remove(id);
}

} // namespace rivet::editor
