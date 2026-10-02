// SPDX-License-Identifier: MPL-2.0
#pragma once

// A fake engine that can WRITE: assembleDocument serializes the request as a
// small text format (one line per page: marker, rotation, crop box) and
// openDocument reads such files back (other paths open as FakePageDocument
// fixtures). Lets the save / rebase / import / extract pipelines run
// end-to-end without a PDF backend. Optional gate: assemblies park inside
// the engine until released (deterministic "save in flight" tests).

#include "FakePageDocument.hpp"

#include "core/Error.hpp"
#include "core/geometry/Rotation.hpp"
#include "pdf/PdfAssembly.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfPageGeometry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rivet::test {

// A document read back from a fake-written file.
class FakeFileDocument final : public pdf::PdfDocument {
public:
    struct Page {
        std::string marker;
        pdf::PdfPageView view;
    };

    explicit FakeFileDocument(std::vector<Page> pages) : pages_(std::move(pages)) {
        info_.pageCount = pages_.size();
        info_.title = "fake-file";
    }

    const pdf::PdfDocumentInfo& info() const override { return info_; }

    static pdf::PdfBox media() { return pdf::PdfBox{0.0, 0.0, 612.0, 792.0}; }

    core::Result<pdf::PdfPageInfo> pageInfo(std::size_t index) const override {
        if (index >= pages_.size()) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        pdf::PdfPageInfo page;
        page.index = index;
        page.mediaBox = media();
        page.view = pages_[index].view;
        page.rotation = pages_[index].view.rotation;
        page.sizePoints = pdf::displaySize(page.view);
        return page;
    }

    core::Result<core::Bitmap> renderPage(std::size_t, const core::Rect&, double) override {
        return core::Bitmap::create(1, 1);
    }

    core::Result<std::shared_ptr<const pdf::PdfTextPage>> textPage(std::size_t index) const override {
        if (index >= pages_.size()) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        return FakePageDocument::makeText(pages_[index].marker);
    }

    const std::vector<Page>& pages() const { return pages_; }

    // Page content of the written file (Phase 5): set by the engine from its
    // `reopenedContents` before the document is shared.
    std::map<std::size_t, pdf::PdfPageContent> contents;

    core::Result<pdf::PdfPageContentPtr> pageContent(std::size_t index,
                                                     const pdf::PdfPageContentEditsPtr&) const override {
        if (index >= pages_.size()) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        pdf::PdfPageContent content;
        if (const auto it = contents.find(index); it != contents.end()) content = it->second;
        for (std::size_t i = 0; i < content.objects.size(); ++i) {
            content.objects[i].index = static_cast<std::uint32_t>(i);
            content.objects[i].origin = {pdf::PdfContentOrigin::Kind::Source, static_cast<std::uint32_t>(i), 0};
        }
        return std::make_shared<const pdf::PdfPageContent>(std::move(content));
    }

private:
    pdf::PdfDocumentInfo info_;
    std::vector<Page> pages_;
};

// The marker a page carries into a fake-written file: the source's own
// text for fake-file pages, "<prefix><n>" for FakePageDocument pages.
inline std::string fakeMarker(const pdf::PdfDocument& source, std::size_t index) {
    if (const auto* file = dynamic_cast<const FakeFileDocument*>(&source); file != nullptr) {
        return file->pages().at(index).marker;
    }
    auto text = source.textPage(index);
    if (!text.has_value()) return "?";
    std::string all = (*text)->text();
    const auto at = all.find('@');
    return at == std::string::npos ? all : all.substr(0, at);
}

class FakeWritableEngine final : public pdf::PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "fake-writable"; }

    core::Result<std::unique_ptr<pdf::PdfDocument>> openDocument(const std::filesystem::path& path,
                                                                 std::string_view password) override {
        ++opens;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failOpen.count(path.filename().string()) != 0) {
                return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument, "corrupt", "test"));
            }
            if (lockedPaths.count(path.filename().string()) != 0 && password != "secret") {
                return std::unexpected(core::makeError(core::ErrorCode::PasswordRequired, "locked", "test"));
            }
            lastPassword = std::string(password);
        }
        std::ifstream in(path, std::ios::binary);
        std::string magic;
        if (in && (in >> magic) && magic == "RIVETFAKE") {
            std::size_t count = 0;
            in >> count;
            std::vector<FakeFileDocument::Page> pages;
            for (std::size_t i = 0; i < count; ++i) {
                FakeFileDocument::Page page;
                int degrees = 0;
                in >> page.marker >> degrees >> page.view.cropBox.left >> page.view.cropBox.bottom >>
                    page.view.cropBox.right >> page.view.cropBox.top;
                page.view.rotation = core::rotationFromQuarterTurns(degrees / 90);
                pages.push_back(std::move(page));
            }
            if (!in) return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument, "truncated", "test"));
            auto file = std::make_unique<FakeFileDocument>(std::move(pages));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                file->contents = reopenedContents;
            }
            return std::unique_ptr<pdf::PdfDocument>(std::move(file));
        }
        // Any other path: an n-page FakePageDocument ("<stem>-" markers),
        // n from pageCounts (default 5).
        std::size_t count = 5;
        std::string prefix = path.stem().string() + "-";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (const auto it = pageCounts.find(path.filename().string()); it != pageCounts.end()) count = it->second;
        }
        if (count == 0) return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument, "no pages", "test"));
        return std::unique_ptr<pdf::PdfDocument>(std::make_unique<FakePageDocument>(count, prefix));
    }

    core::Result<std::unique_ptr<pdf::PdfDocument>> reopenWithCredentialsOf(
        const pdf::PdfDocument&, const std::filesystem::path& path) override {
        ++reopens;
        return openDocument(path, reopenPassword);
    }

    core::Status assembleDocument(const pdf::PdfAssemblyRequest& request, pdf::IPdfByteSink& sink,
                                  std::vector<pdf::PdfAssembledPageAnnotations>* annotationReport = nullptr,
                                  std::vector<pdf::PdfAssembledPageContent>* contentReport = nullptr) override {
        const int ordinal = ++assemblies;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastEdits.clear();
            lastContentEdits.clear();
            for (const pdf::PdfAssemblyPage& page : request.pages) {
                lastEdits.push_back(page.annotationEdits != nullptr ? *page.annotationEdits
                                                                    : pdf::PdfPageAnnotationEdits{});
                lastContentEdits.push_back(page.contentEdits != nullptr ? *page.contentEdits
                                                                        : pdf::PdfPageContentEdits{});
            }
        }
        waitAtGate();
        if (failAssembly.load() || (failAssemblyFrom.load() > 0 && ordinal >= failAssemblyFrom.load())) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidDocument, "assembly failed", "test"));
        }
        std::ostringstream out;
        out << "RIVETFAKE " << request.pages.size() << "\n";
        for (const pdf::PdfAssemblyPage& page : request.pages) {
            out << fakeMarker(*page.source, page.sourcePageIndex) << ' ' << core::rotationDegrees(page.view.rotation)
                << ' ' << page.view.cropBox.left << ' ' << page.view.cropBox.bottom << ' '
                << page.view.cropBox.right << ' ' << page.view.cropBox.top << "\n";
        }
        if (annotationReport != nullptr && reportAnnotations.load()) {
            annotationReport->clear();
            if (reportOverride.has_value()) {
                *annotationReport = *reportOverride;
            } else {
                // Like the real writer: removals first, then the creations appended.
                for (const pdf::PdfAssemblyPage& page : request.pages) {
                    std::uint32_t count = 0;
                    if (auto originals = page.source->annotations(page.sourcePageIndex); originals.has_value()) {
                        count = (*originals)->annotsCount;
                    }
                    pdf::PdfAssembledPageAnnotations entry;
                    if (page.annotationEdits != nullptr) {
                        count -= static_cast<std::uint32_t>(page.annotationEdits->removeIndices.size());
                        for (std::size_t k = 0; k < page.annotationEdits->create.size(); ++k) {
                            entry.createdIndices.push_back(count++);
                        }
                    }
                    entry.annotsCount = count;
                    annotationReport->push_back(std::move(entry));
                }
            }
        }
        if (contentReport != nullptr) {
            // Like a backend without edits would: empty entries (object i =
            // source object i). Tests needing origins override the report.
            contentReport->clear();
            if (contentReportOverride.has_value()) {
                *contentReport = *contentReportOverride;
            } else {
                contentReport->assign(request.pages.size(), pdf::PdfAssembledPageContent{});
            }
        }
        const std::string bytes = out.str();
        // Several chunks, like a real backend streaming its output.
        const std::size_t chunk = 7;
        for (std::size_t at = 0; at < bytes.size(); at += chunk) {
            const std::size_t size = std::min(chunk, bytes.size() - at);
            if (core::Status status = sink.write(bytes.data() + at, size); !status) return status;
        }
        return core::ok();
    }

    // --- Gate: assemblies block until release() ------------------------
    void closeGate() {
        std::lock_guard<std::mutex> lock(mutex_);
        gated_ = true;
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        gated_ = false;
        cv_.notify_all();
    }
    bool waitParked(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(10), [&] { return parked_ >= count; });
    }

    // Configuration (set before use; filenames, not full paths).
    std::set<std::string> failOpen;
    std::set<std::string> lockedPaths;
    std::map<std::string, std::size_t> pageCounts;
    std::atomic<bool> failAssembly{false};
    // > 0: assemblies numbered >= this value fail (1-based; partial failures).
    std::atomic<int> failAssemblyFrom{0};
    std::string reopenPassword;
    std::string lastPassword;

    // Annotation report: filled like a real writer unless disabled (a
    // backend without report support) or overridden.
    std::atomic<bool> reportAnnotations{true};
    std::optional<std::vector<pdf::PdfAssembledPageAnnotations>> reportOverride;
    // The annotation edits of the last assembly, one per page (empty = none).
    std::vector<pdf::PdfPageAnnotationEdits> lastEdits;
    // The content edits of the last assembly, one per page (empty = none).
    std::vector<pdf::PdfPageContentEdits> lastContentEdits;
    // When set, returned as the content report of every assembly (else one
    // empty entry per page).
    std::optional<std::vector<pdf::PdfAssembledPageContent>> contentReportOverride;
    // Page content of documents read back from fake-written files, by page
    // index (set before the save; objects are renumbered Source(i)).
    std::map<std::size_t, pdf::PdfPageContent> reopenedContents;

    std::atomic<int> opens{0};
    std::atomic<int> reopens{0};
    std::atomic<int> assemblies{0};

private:
    void waitAtGate() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!gated_) return;
        ++parked_;
        cv_.notify_all();
        cv_.wait(lock, [this] { return !gated_; });
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    bool gated_ = false;
    int parked_ = 0;
};

// Reads a fake-written file's page markers (empty when unreadable).
inline std::vector<std::string> fakeFileMarkers(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    std::size_t count = 0;
    std::vector<std::string> markers;
    if (!(in >> magic >> count) || magic != "RIVETFAKE") return markers;
    for (std::size_t i = 0; i < count; ++i) {
        std::string marker;
        int degrees = 0;
        double l = 0, b = 0, r = 0, t = 0;
        in >> marker >> degrees >> l >> b >> r >> t;
        markers.push_back(marker);
    }
    return markers;
}

} // namespace rivet::test
