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
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfNavigation.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include "pdf/PdfText.hpp"
#include "pdf/PdfTypes.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
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
        return renderPageInView(pageIndex, pdf::PdfPageView{core::PageRotation::None, mediaBox(pageIndex)},
                                rect, scale);
    }

    core::Result<std::shared_ptr<const pdf::PdfTextPage>> textPage(std::size_t pageIndex) const override {
        return textPageInView(pageIndex, pdf::PdfPageView{core::PageRotation::None, mediaBox(pageIndex)});
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

    pdf::PdfPageView lastRenderView() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastRenderView_;
    }

    mutable std::atomic<int> renders{0};
    mutable std::atomic<int> extractions{0};
    mutable std::atomic<int> linkLoads{0};

protected:
    core::Result<core::Bitmap> renderPageInView(std::size_t pageIndex, const pdf::PdfPageView& view,
                                                const core::Rect& rect, double scale) override {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        ++renders;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastRenderView_ = view;
        }
        const auto width = static_cast<std::uint32_t>(rect.size.width * scale + 0.5);
        const auto height = static_cast<std::uint32_t>(rect.size.height * scale + 0.5);
        auto bitmap = core::Bitmap::create(width == 0 ? 1 : width, height == 0 ? 1 : height);
        if (bitmap.has_value()) std::memset(bitmap->data(), 0xAB, bitmap->sizeBytes());
        return bitmap;
    }

    core::Result<std::shared_ptr<const pdf::PdfTextPage>> textPageInView(std::size_t pageIndex,
                                                                         const pdf::PdfPageView& view) const override {
        if (pageIndex >= info_.pageCount) {
            return std::unexpected(core::makeError(core::ErrorCode::InvalidArgument, "page", "test"));
        }
        ++extractions;
        return makeText(textFor(pageIndex, view));
    }

    core::Result<std::vector<pdf::PdfPageLink>> pageLinksInView(std::size_t pageIndex,
                                                                const pdf::PdfPageView&) const override {
        ++linkLoads;
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
