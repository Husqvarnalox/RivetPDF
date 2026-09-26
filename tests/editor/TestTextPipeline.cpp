// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "editor/TextSearchController.hpp"
#include "editor/TextService.hpp"
#include "pdf/PdfEngine.hpp"

#include "core/async/TaskScheduler.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::DocumentId;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::PageId;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentSession;
using rivet::editor::TextSearchController;
using rivet::editor::TextService;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfPageInfo;
using rivet::pdf::PdfTextPage;
using rivet::pdf::TextChar;

namespace {

// Three pages with known text; textPage() returns synthetic pages, so this
// exercises the editor pipeline without PDFium.
class FakeTextDocument final : public PdfDocument {
public:
    FakeTextDocument() {
        info_.pageCount = 3;
        info_.isEncrypted = false;
        info_.title = "fake-text";
    }

    const PdfDocumentInfo& info() const override { return info_; }

    Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const override {
        if (pageIndex >= 3) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        }
        return PdfPageInfo{pageIndex, Size{612.0, 792.0}, rivet::core::PageRotation::None};
    }

    Result<Bitmap> renderPage(std::size_t, const Rect&, double) override {
        return Bitmap::create(4, 4);
    }

    Result<std::shared_ptr<const PdfTextPage>> textPage(std::size_t pageIndex) const override {
        if (pageIndex >= 3) {
            return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        }
        return pageText(pageIndex);
    }

    // Builds a one-line PdfTextPage from the ASCII text.
    static std::shared_ptr<const PdfTextPage> makePage(const std::string& text) {
        std::vector<TextChar> chars;
        chars.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            TextChar ch;
            ch.unicode = static_cast<char32_t>(static_cast<unsigned char>(text[i]));
            ch.index = static_cast<std::uint32_t>(i);
            ch.bounds = Rect{static_cast<double>(i) * 8.0, 100.0, 7.0, 12.0};
            ch.fontSize = 12.0;
            chars.push_back(ch);
        }
        return std::make_shared<const PdfTextPage>(std::move(chars));
    }

    mutable std::atomic<int> extractions{0};

private:
    std::shared_ptr<const PdfTextPage> pageText(std::size_t index) const {
        ++extractions;
        switch (index) {
        case 0: return makePage("alpha beta gamma");
        case 1: return makePage("delta alpha epsilon");
        default: return makePage("zeta");
        }
    }

    PdfDocumentInfo info_;
};

class FakeTextEngine final : public PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "fake"; }
    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path&,
                                                      std::string_view) override {
        return std::unique_ptr<PdfDocument>(std::make_unique<FakeTextDocument>());
    }
};

// Inline "main thread" dispatcher is fine here: the test drives everything
// from a single thread and services deliver inline when no dispatcher is
// configured (the DocumentSession contract for tests).
struct Fixture {
    FakeTextEngine engine;
    TaskScheduler scheduler{2};

    std::unique_ptr<DocumentSession> open() {
        std::filesystem::path path = std::filesystem::temp_directory_path() / "rivet-text-fixture.pdf";
        {
            std::FILE* file = std::fopen(path.string().c_str(), "wb");
            std::fputs("%PDF-1.4\n", file);
            std::fclose(file);
        }
        auto session = DocumentSession::create(engine, scheduler, nullptr, path);
        CHECK(session.has_value());
        return std::move(*session);
    }
};

// Bounded wait for a predicate (turns a hang into a test failure).
template <typename Predicate>
bool waitFor(Predicate predicate, int attempts = 500) {
    for (int i = 0; i < attempts; ++i) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

} // namespace

RIVET_TEST(textServiceExtractsAndCaches) {
    Fixture f;
    auto session = f.open();
    TextService& text = session->textService();
    FakeTextDocument& document = *static_cast<FakeTextDocument*>(&session->document());

    const PageId page0 = session->pageId(0);
    // Promise/future: the callback fires on the worker thread (null
    // dispatcher); the future provides the test-thread synchronization TSan
    // requires.
    auto promise0 = std::make_shared<std::promise<std::shared_ptr<const PdfTextPage>>>();
    text.requestTextPage(page0, [promise0](std::shared_ptr<const PdfTextPage> page) {
        promise0->set_value(std::move(page));
    });
    auto delivered = promise0->get_future().get();
    CHECK(delivered != nullptr);
    CHECK_EQ(delivered->text(), "alpha beta gamma");
    CHECK_EQ(document.extractions.load(), 1);

    // Second request: cache hit, no new extraction.
    auto promise1 = std::make_shared<std::promise<std::shared_ptr<const PdfTextPage>>>();
    text.requestTextPage(page0, [promise1](std::shared_ptr<const PdfTextPage> page) {
        promise1->set_value(std::move(page));
    });
    auto second = promise1->get_future().get();
    CHECK_EQ(second.get(), delivered.get());
    CHECK_EQ(document.extractions.load(), 1);
}

RIVET_TEST(textServiceWorkerPathExtractsSynchronously) {
    Fixture f;
    auto session = f.open();
    TextService& text = session->textService();

    const auto page = text.textPageNow(session->pageSnapshot()->at(1));
    CHECK(page != nullptr);
    CHECK_EQ(page->text(), "delta alpha epsilon");
}

RIVET_TEST(searchFindsMatchesAcrossPagesAndNavigates) {
    Fixture f;
    auto session = f.open();
    TextService& text = session->textService();
    TextSearchController search(*session, text);

    std::atomic<int> notifications{0};
    search.setOnResultsChanged([&] { ++notifications; });

    search.start("alpha");
    CHECK(search.searching());
    CHECK(waitFor([&] { return !search.searching() && search.matches().size() == 2; }));
    CHECK_EQ(search.matches().size(), std::size_t{2});
    CHECK_EQ(search.matches()[0].page, session->pageId(0));
    CHECK_EQ(search.matches()[0].startIndex, std::uint32_t{0});
    CHECK_EQ(search.matches()[1].page, session->pageId(1));

    // Case-insensitive search of "ALPHA" finds the same matches.
    search.start("ALPHA");
    CHECK(waitFor([&] { return !search.searching() && search.matches().size() == 2; }));

    // Navigation: next -> 0, next -> 1, wraps to 0.
    search.next();
    CHECK_EQ(search.currentIndex(), std::optional<std::size_t>(0));
    search.next();
    CHECK_EQ(search.currentIndex(), std::optional<std::size_t>(1));
    search.next();
    CHECK_EQ(search.currentIndex(), std::optional<std::size_t>(0));
    search.previous();
    CHECK_EQ(search.currentIndex(), std::optional<std::size_t>(1));

    // Empty query clears.
    search.start("");
    CHECK(!search.searching());
    CHECK(search.matches().empty());
}

RIVET_TEST(searchCancellationReplacesTheWalk) {
    Fixture f;
    auto session = f.open();
    TextService& text = session->textService();
    TextSearchController search(*session, text);

    search.start("alpha");
    // Immediately restart with a different query: the first walk must be
    // invalidated and only the new results survive.
    search.start("zeta");
    CHECK(waitFor([&] { return !search.searching() && search.matches().size() == 1; }));
    CHECK_EQ(search.matches()[0].page, session->pageId(2));
    CHECK_EQ(search.query(), "zeta");
}

RIVET_TEST(searchingMissingTextSkipsThePage) {
    Fixture f;
    auto session = f.open();
    TextService& text = session->textService();
    // Page 2 has text "zeta": searching for something absent yields no
    // matches, and the search completes without hanging.
    TextSearchController search(*session, text);
    search.start("omega");
    CHECK(waitFor([&] { return !search.searching(); }));
    CHECK(search.matches().empty());
    CHECK_EQ(search.currentIndex(), std::nullopt);
}
