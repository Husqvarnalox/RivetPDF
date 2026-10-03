// SPDX-License-Identifier: MPL-2.0
#pragma once

// Portable fake backend for page-model tests: an in-memory PdfDocument with
// N pages whose view-aware hooks RECORD the view they were asked for, so
// tests can verify that rendering / text / links go through the page
// model's views. No PDFium.

#include "core/Bitmap.hpp"
#include "core/Error.hpp"
#include "core/geometry/Rect.hpp"
#include "core/geometry/Rotation.hpp"
#include "core/geometry/Size.hpp"
#include "pdf/PdfAnnotation.hpp"
#include "pdf/PdfContent.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfText.hpp"
#include "pdf/PdfTypes.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rivet::test {

class FakePageDocument final : public pdf::PdfDocument {
public:
    // Page i: media box 0,0 612x792 (or `small` = 300x400 for indices in
    // smallPages), native view = whole media box, no rotation. Text of page
    // i is "<prefix><i+1>@<view rotation degrees>".
    explicit FakePageDocument(std::size_t pageCount, std::string prefix = "PAGE-",
                              std::vector<std::size_t> smallPages = {})
        : prefix_(std::move(prefix)), smallPages_(std::move(smallPages)) {
        info_.pageCount = pageCount;
        info_.title = "fake-pages";
    }

    const pdf::PdfDocumentInfo& info() const override { return info_; }

    core::Result<pdf::PdfPageInfo> pageInfo(std::size_t pageIndex) const override {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        pdf::PdfPageInfo page;
        page.index = pageIndex;
        page.mediaBox = mediaBox(pageIndex);
        page.view = pdf::PdfPageView{core::PageRotation::None, page.mediaBox};
        page.rotation = core::PageRotation::None;
        page.sizePoints = pdf::displaySize(page.view);
        return page;
    }

    pdf::PdfBox mediaBox(std::size_t pageIndex) const {
        for (const std::size_t small : smallPages_) {
            if (small == pageIndex) return pdf::PdfBox{0.0, 0.0, 300.0, 400.0};
        }
        return pdf::PdfBox{0.0, 0.0, 612.0, 792.0};
    }

    core::Result<std::string> pageLabel(std::size_t pageIndex) const override {
        return "L" + std::to_string(pageIndex + 1);
    }

    core::Result<core::Bitmap> renderPage(std::size_t pageIndex, const core::Rect& rect,
                                          double scale) override {
        return renderPageInView(pageIndex, pdf::PdfPageView{core::PageRotation::None, mediaBox(pageIndex)}, {},
                                nullptr, rect, scale);
    }

    core::Result<std::shared_ptr<const pdf::PdfTextPage>> textPage(std::size_t pageIndex) const override {
        return textPageInView(pageIndex, pdf::PdfPageView{core::PageRotation::None, mediaBox(pageIndex)}, nullptr);
    }

    core::Result<std::vector<pdf::PdfPageLink>> pageLinks(std::size_t pageIndex) const override {
        return pageLinksInView(pageIndex, pdf::PdfPageView{core::PageRotation::None, mediaBox(pageIndex)});
    }

    static std::shared_ptr<const pdf::PdfTextPage> makeText(const std::string& text) {
        std::vector<pdf::TextChar> chars;
        for (std::size_t i = 0; i < text.size(); ++i) {
            pdf::TextChar ch;
            ch.unicode = static_cast<char32_t>(static_cast<unsigned char>(text[i]));
            ch.index = static_cast<std::uint32_t>(i);
            ch.bounds = core::Rect{static_cast<double>(i) * 8.0, 100.0, 7.0, 12.0};
            ch.fontSize = 12.0;
            chars.push_back(ch);
        }
        return std::make_shared<const pdf::PdfTextPage>(std::move(chars));
    }

    std::string textFor(std::size_t pageIndex, const pdf::PdfPageView& view) const {
        return prefix_ + std::to_string(pageIndex + 1) + "@" + std::to_string(core::rotationDegrees(view.rotation));
    }

    // Links returned for a page (all pages: none by default).
    void setLinks(std::size_t pageIndex, std::vector<pdf::PdfPageLink> links) {
        std::lock_guard<std::mutex> lock(mutex_);
        links_[pageIndex] = std::move(links);
    }

    // --- Annotations ----------------------------------------------------
    // Originals returned by annotations(pageIndex) (default: an empty list).
    // `annotsCount` defaults to the item count.
    void setAnnotations(std::size_t pageIndex, std::vector<pdf::PdfPageAnnotation> items,
                        std::optional<std::uint32_t> annotsCount = std::nullopt) {
        auto page = std::make_shared<pdf::PdfPageAnnotations>();
        page->annotsCount = annotsCount.value_or(static_cast<std::uint32_t>(items.size()));
        page->items = std::move(items);
        std::lock_guard<std::mutex> lock(mutex_);
        annotations_[pageIndex] = std::move(page);
    }
    // annotations() reports NotAvailable (like a backend without support).
    void setAnnotationsUnavailable(bool unavailable) { annotationsUnavailable_ = unavailable; }
    // Annotation loads park inside annotations() until released.
    void closeAnnotationGate() {
        std::lock_guard<std::mutex> lock(mutex_);
        annotationGated_ = true;
    }
    void releaseAnnotationGate() {
        std::lock_guard<std::mutex> lock(mutex_);
        annotationGated_ = false;
        cv_.notify_all();
    }
    bool waitAnnotationParked(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(10), [&] { return annotationParked_ >= count; });
    }

    core::Result<pdf::PdfPageAnnotationsPtr> annotations(std::size_t pageIndex) const override {
        std::unique_lock<std::mutex> lock(mutex_);
        ++annotationLoads;
        if (annotationGated_) {
            ++annotationParked_;
            cv_.notify_all();
            cv_.wait(lock, [this] { return !annotationGated_; });
        }
        if (annotationsUnavailable_ || pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::NotAvailable, "no annotations", "test"));
        }
        const auto it = annotations_.find(pageIndex);
        if (it == annotations_.end()) return std::make_shared<const pdf::PdfPageAnnotations>();
        return it->second;
    }

    std::vector<std::uint32_t> lastHiddenAnnotations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastHidden_;
    }

    pdf::PdfPageView lastRenderView() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastRenderView_;
    }

    // --- Content editing (Phase 5) -----------------------------------------
    // Page content returned by pageContent(): configure per page BEFORE the
    // document is shared (not synchronized). Pages without an entry have an
    // empty, regeneration-safe content.
    std::map<std::size_t, pdf::PdfPageContent> pageContents;

    // pageContent(page, null/empty) returns the configured content as is.
    // With edits it returns the configured content with the removed objects
    // dropped and the transforms applied to matrix / bounds / quad (the
    // remaining objects are renumbered, origin = Source(original index)).
    // Text blocks are simulated minimally: the members are dropped and every
    // non-empty line of the text becomes a created, tagged text object on top
    // (matrix = placement, one lineAdvance per line, width 0.5 x size per
    // character). Image replacement is not simulated.
    core::Result<pdf::PdfPageContentPtr> pageContent(std::size_t pageIndex,
                                                     const pdf::PdfPageContentEditsPtr& edits) const override {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        ++contentLoads;
        { std::lock_guard<std::mutex> gate(contentGate); }
        pdf::PdfPageContent content;
        if (const auto it = pageContents.find(pageIndex); it != pageContents.end()) content = it->second;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastContentEdits_ = edits;
        }
        if (edits == nullptr || edits->empty()) {
            for (std::size_t i = 0; i < content.objects.size(); ++i) {
                content.objects[i].index = static_cast<std::uint32_t>(i);
                content.objects[i].origin = {pdf::PdfContentOrigin::Kind::Source, static_cast<std::uint32_t>(i), 0};
            }
            return std::make_shared<const pdf::PdfPageContent>(std::move(content));
        }
        std::vector<pdf::PdfContentObject> out;
        std::set<std::uint32_t> replaced;
        for (const pdf::PdfTextBlockEdit& block : edits->textBlocks) replaced.insert(block.members.begin(), block.members.end());
        for (const pdf::PdfContentObject& source : content.objects) {
            if (replaced.count(source.index) != 0) continue;
            const pdf::PdfObjectEdit* edit = nullptr;
            for (const pdf::PdfObjectEdit& candidate : edits->objects) {
                if (candidate.sourceIndex == source.index) edit = &candidate;
            }
            if (edit != nullptr && edit->remove) continue;
            pdf::PdfContentObject object = source;
            object.origin = {pdf::PdfContentOrigin::Kind::Source, source.index, 0};
            if (edit != nullptr && edit->transform.has_value()) {
                const core::Matrix& m = *edit->transform;
                object.matrix = m * object.matrix;
                pdf::PdfBox box{1e300, 1e300, -1e300, -1e300};
                const core::Point corners[4] = {{source.bounds.left, source.bounds.bottom},
                                                {source.bounds.right, source.bounds.bottom},
                                                {source.bounds.left, source.bounds.top},
                                                {source.bounds.right, source.bounds.top}};
                for (const core::Point& corner : corners) {
                    const core::Point mapped = m.map(corner);
                    box.left = std::min(box.left, mapped.x);
                    box.bottom = std::min(box.bottom, mapped.y);
                    box.right = std::max(box.right, mapped.x);
                    box.top = std::max(box.top, mapped.y);
                }
                object.bounds = box;
                for (pdf::PdfPoint& corner : object.quad) {
                    const core::Point mapped = m.map(core::Point{corner.x, corner.y});
                    corner = pdf::PdfPoint{mapped.x, mapped.y};
                }
            }
            out.push_back(std::move(object));
        }
        // Text blocks: one created text object per non-empty line (tagged),
        // appended on top; the members they replace are gone.
        for (const pdf::PdfTextBlockEdit& block : edits->textBlocks) {
            std::size_t lineNo = 0;
            std::size_t from = 0;
            while (from <= block.text.size()) {
                std::size_t to = block.text.find('\n', from);
                if (to == std::string::npos) to = block.text.size();
                const std::string line = block.text.substr(from, to - from);
                const std::size_t k = lineNo++;
                from = to + 1;
                if (line.empty()) continue;
                pdf::PdfContentObject o;
                o.type = pdf::PdfContentObjectType::Text;
                o.origin = {pdf::PdfContentOrigin::Kind::Created, 0, block.tag};
                o.blockTag = block.tag;
                o.matrix = block.placement * core::Matrix::translation(0.0, -static_cast<double>(k) * block.lineAdvance);
                o.text = line;
                o.fontSize = block.fontSize;
                o.font.embedded = true;
                o.font.baseName = "Bundled";
                o.fill = block.color;
                o.fontSubstituted = block.font.kind == pdf::PdfFontRef::Kind::Bundled;
                const double width = 0.5 * block.fontSize * static_cast<double>(line.size());
                const core::Point local[4] = {{0.0, -0.2 * block.fontSize},
                                              {width, -0.2 * block.fontSize},
                                              {width, 0.8 * block.fontSize},
                                              {0.0, 0.8 * block.fontSize}};
                pdf::PdfBox box{1e300, 1e300, -1e300, -1e300};
                for (std::size_t c = 0; c < 4; ++c) {
                    const core::Point p = o.matrix.map(local[c]);
                    o.quad[c] = pdf::PdfPoint{p.x, p.y};
                    box.left = std::min(box.left, p.x);
                    box.bottom = std::min(box.bottom, p.y);
                    box.right = std::max(box.right, p.x);
                    box.top = std::max(box.top, p.y);
                }
                o.bounds = box;
                out.push_back(std::move(o));
            }
        }
        for (std::size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<std::uint32_t>(i);
        content.objects = std::move(out);
        return std::make_shared<const pdf::PdfPageContent>(std::move(content));
    }

    // Dry run: counted, then answered by the hook (ok when unset). Set the
    // hook BEFORE the document is shared (not synchronized).
    core::Status checkContentEdits(std::size_t pageIndex, const pdf::PdfPageContentEdits& edits) const override {
        ++checkContentEditsCalls;
        if (checkContentEditsHook) return checkContentEditsHook(pageIndex, edits);
        return core::ok();
    }
    std::function<core::Status(std::size_t, const pdf::PdfPageContentEdits&)> checkContentEditsHook;
    mutable std::atomic<int> checkContentEditsCalls{0};

    // The content edits seen by the last render / text / pageContent call.
    pdf::PdfPageContentEditsPtr lastRenderContentEdits() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastRenderContent_;
    }
    pdf::PdfPageContentEditsPtr lastTextContentEdits() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastTextContent_;
    }
    pdf::PdfPageContentEditsPtr lastPageContentEdits() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastContentEdits_;
    }
    mutable std::atomic<int> contentLoads{0};
    // Held by a test to block pageContent() calls (after contentLoads is bumped).
    mutable std::mutex contentGate;

    mutable std::atomic<int> renders{0};
    mutable std::atomic<int> extractions{0};
    mutable std::atomic<int> linkLoads{0};
    // Held by a test to block pageLinks() calls (after linkLoads is bumped).
    mutable std::mutex linkGate;
    mutable std::atomic<int> annotationLoads{0};

protected:
    core::Result<core::Bitmap> renderPageInView(std::size_t pageIndex, const pdf::PdfPageView& view,
                                                std::span<const std::uint32_t> hiddenAnnotations,
                                                const pdf::PdfPageContentEditsPtr& content,
                                                const core::Rect& rect, double scale) override {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        ++renders;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastRenderView_ = view;
            lastHidden_.assign(hiddenAnnotations.begin(), hiddenAnnotations.end());
            lastRenderContent_ = content;
        }
        const auto width = static_cast<std::uint32_t>(rect.size.width * scale + 0.5);
        const auto height = static_cast<std::uint32_t>(rect.size.height * scale + 0.5);
        auto bitmap = core::Bitmap::create(width == 0 ? 1 : width, height == 0 ? 1 : height);
        if (bitmap.has_value()) std::memset(bitmap->data(), 0xAB, bitmap->sizeBytes());
        return bitmap;
    }

    core::Result<std::shared_ptr<const pdf::PdfTextPage>> textPageInView(
        std::size_t pageIndex, const pdf::PdfPageView& view,
        const pdf::PdfPageContentEditsPtr& content) const override {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        ++extractions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastTextContent_ = content;
        }
        return makeText(textFor(pageIndex, view));
    }

    core::Result<std::vector<pdf::PdfPageLink>> pageLinksInView(std::size_t pageIndex,
                                                                const pdf::PdfPageView&) const override {
        ++linkLoads;
        { std::lock_guard<std::mutex> gate(linkGate); } // tests hold it to park a link load
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = links_.find(pageIndex);
        if (it == links_.end()) return std::vector<pdf::PdfPageLink>{};
        return it->second;
    }

private:
    pdf::PdfDocumentInfo info_;
    std::string prefix_;
    std::vector<std::size_t> smallPages_;
    mutable std::mutex mutex_;
    std::map<std::size_t, std::vector<pdf::PdfPageLink>> links_;
    pdf::PdfPageView lastRenderView_;
    std::vector<std::uint32_t> lastHidden_;
    pdf::PdfPageContentEditsPtr lastRenderContent_;
    mutable pdf::PdfPageContentEditsPtr lastTextContent_;
    mutable pdf::PdfPageContentEditsPtr lastContentEdits_;
    std::map<std::size_t, pdf::PdfPageAnnotationsPtr> annotations_;
    mutable std::condition_variable cv_;
    bool annotationGated_ = false;
    mutable int annotationParked_ = 0;
    std::atomic<bool> annotationsUnavailable_{false};
};

// Opens FakePageDocuments of a fixed size; remembers the last one.
class FakePageEngine final : public pdf::PdfEngine {
public:
    explicit FakePageEngine(std::size_t pageCount = 5) : pageCount_(pageCount) {}

    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "fake"; }

    core::Result<std::unique_ptr<pdf::PdfDocument>> openDocument(const std::filesystem::path&,
                                                                 std::string_view) override {
        auto document = std::make_unique<FakePageDocument>(pageCount_);
        lastDocument = document.get();
        return std::unique_ptr<pdf::PdfDocument>(std::move(document));
    }

    FakePageDocument* lastDocument = nullptr;

private:
    std::size_t pageCount_;
};

} // namespace rivet::test
