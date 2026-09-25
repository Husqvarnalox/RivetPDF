// SPDX-License-Identifier: MPL-2.0
// Copy correctness: selection normalization, exact text assembly, and the
// asynchronous ranges pipeline (cold cache, failures, teardown).
#include "RivetTest.h"

#include "editor/DocumentSession.hpp"
#include "editor/SelectionText.hpp"
#include "editor/TextService.hpp"
#include "pdf/PdfEngine.hpp"

#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::PageId;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentSession;
using rivet::editor::TextPosition;
using rivet::editor::TextRange;
using rivet::editor::TextSelection;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfPageInfo;
using rivet::pdf::PdfTextPage;
using rivet::pdf::TextChar;

namespace {

// Page from UTF-32 text; '\r' and '\n' become generated (empty-bounds)
// characters like PDFium's line breaks.
std::shared_ptr<const PdfTextPage> makePage(const std::u32string& text) {
    std::vector<TextChar> chars;
    for (std::size_t i = 0; i < text.size(); ++i) {
        TextChar ch;
        ch.unicode = text[i];
        ch.index = static_cast<std::uint32_t>(i);
        if (text[i] != U'\r' && text[i] != U'\n') ch.bounds = Rect{static_cast<double>(i) * 8.0, 100.0, 7.0, 12.0};
        ch.fontSize = 12.0;
        chars.push_back(ch);
    }
    return std::make_shared<const PdfTextPage>(std::move(chars));
}

class TextDocument final : public PdfDocument {
public:
    explicit TextDocument(std::vector<std::u32string> pages) : pages_(std::move(pages)) {
        info_.pageCount = pages_.size();
    }
    const PdfDocumentInfo& info() const override { return info_; }
    Result<PdfPageInfo> pageInfo(std::size_t index) const override {
        if (index >= pages_.size()) return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        return PdfPageInfo{index, Size{612.0, 792.0}, rivet::core::PageRotation::None};
    }
    Result<Bitmap> renderPage(std::size_t, const Rect&, double) override { return Bitmap::create(2, 2); }
    Result<std::shared_ptr<const PdfTextPage>> textPage(std::size_t index) const override {
        ++extractions;
        if (parkPage.has_value() && *parkPage == index) {
            std::unique_lock<std::mutex> lock(mutex);
            parked = true;
            cv.notify_all();
            cv.wait(lock, [this] { return released; });
        }
        if (failPage.has_value() && *failPage == index) {
            return std::unexpected(Error{ErrorCode::InvalidDocument, "broken page", "test"});
        }
        if (index >= pages_.size()) return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        return makePage(pages_[index]);
    }

    std::optional<std::size_t> failPage;
    std::optional<std::size_t> parkPage;
    mutable std::atomic<int> extractions{0};
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    mutable bool parked = false;
    bool released = false;

private:
    std::vector<std::u32string> pages_;
    PdfDocumentInfo info_;
};

class Engine final : public PdfEngine {
public:
    explicit Engine(std::vector<std::u32string> pages) : pages_(std::move(pages)) {}
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "text"; }
    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path&, std::string_view) override {
        return std::unique_ptr<PdfDocument>(std::make_unique<TextDocument>(pages_));
    }

private:
    std::vector<std::u32string> pages_;
};

class QueueDispatcher final : public rivet::core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    void pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

struct Fixture {
    explicit Fixture(std::vector<std::u32string> pages) : engine(std::move(pages)) {
        const auto path = std::filesystem::temp_directory_path() / "rivet-selection-text.pdf";
        if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::fputs("%PDF-1.4\n", file);
            std::fclose(file);
        }
        auto created = DocumentSession::create(engine, scheduler, &dispatcher, path);
        CHECK(created.has_value());
        session = std::move(*created);
    }
    TextDocument& document() { return *static_cast<TextDocument*>(&session->document()); }

    std::vector<TextRange> ranges(const TextSelection& selection) {
        DocumentSession* s = session.get();
        return rivet::editor::orderedSelectionRanges(
            selection, [s](PageId id) { return s->pageIndexFor(id); },
            [s](std::size_t index) { return s->pageId(index); }, DocumentSession::kInvalidPage);
    }

    // Runs a copy and pumps until it delivers.
    std::optional<Result<std::string>> copy(const TextSelection& selection) {
        std::optional<Result<std::string>> out;
        session->textService().requestRangesText(ranges(selection),
                                                  [&out](Result<std::string> text) { out = std::move(text); });
        for (int i = 0; i < 2000 && !out.has_value(); ++i) {
            dispatcher.pump();
            if (!out.has_value()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }

    Engine engine;
    TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    std::unique_ptr<DocumentSession> session;
};

TextSelection select(PageId anchorPage, std::uint32_t anchorChar, PageId focusPage, std::uint32_t focusChar) {
    return TextSelection{TextPosition{anchorPage, anchorChar}, TextPosition{focusPage, focusChar}};
}

} // namespace

RIVET_TEST(selectionRangesForwardBackwardAndSamePage) {
    Fixture f({U"alpha one", U"bravo two", U"charlie three"});
    const PageId p0 = f.session->pageId(0);
    const PageId p1 = f.session->pageId(1);
    const PageId p2 = f.session->pageId(2);

    const auto forward = f.ranges(select(p0, 6, p2, 7));
    CHECK_EQ(forward.size(), std::size_t{3});
    CHECK(forward[0] == (TextRange{p0, 6, TextRange::kToPageEnd}));
    CHECK(forward[1] == (TextRange{p1, 0, TextRange::kToPageEnd}));
    CHECK(forward[2] == (TextRange{p2, 0, 7}));

    // Backward drag: identical ranges.
    CHECK(f.ranges(select(p2, 7, p0, 6)) == forward);

    // Same page, backward.
    const auto same = f.ranges(select(p1, 5, p1, 1));
    CHECK_EQ(same.size(), std::size_t{1});
    CHECK(same[0] == (TextRange{p1, 1, 5}));

    // Empty and unknown ends yield nothing.
    CHECK(f.ranges(select(p1, 3, p1, 3)).empty());
    CHECK(f.ranges(select(PageId{999}, 0, p1, 3)).empty());
}

RIVET_TEST(rangeTextHandlesLineBreaksCyrillicAndClamping) {
    const auto page = makePage(U"Привет\r\nмир�!");
    std::string out;
    rivet::editor::appendRangeText(*page, 0, TextRange::kToPageEnd, out);
    // "\r\n" collapses to one '\n'; U+FFFD with geometry is kept as text.
    CHECK_EQ(out, std::string("Привет\nмир\xEF\xBF\xBD!"));

    std::string partial;
    rivet::editor::appendRangeText(*page, 2, 4, partial);
    CHECK_EQ(partial, std::string("ив"));

    std::string clamped;
    rivet::editor::appendRangeText(*page, 11, 1000, clamped);
    CHECK_EQ(clamped, std::string("\xEF\xBF\xBD!"));
}

RIVET_TEST(copyExtractsColdPagesInsteadOfSkippingThem) {
    Fixture f({U"alpha one", U"bravo два", U"charlie three"});
    // A zero budget: nothing is ever cached, every page is cold.
    f.session->textService().cache().setMaxBytes(0);
    const PageId p0 = f.session->pageId(0);
    const PageId p2 = f.session->pageId(2);

    const auto forward = f.copy(select(p0, 6, p2, 7));
    CHECK(forward.has_value() && forward->has_value());
    CHECK_EQ(**forward, std::string("one\nbravo два\ncharlie"));
    CHECK(f.document().extractions.load() >= 3);

    const auto backward = f.copy(select(p2, 7, p0, 6));
    CHECK(backward.has_value() && backward->has_value());
    CHECK_EQ(**backward, **forward);
}

RIVET_TEST(copyReportsAFailedPageInsteadOfAPartialText) {
    Fixture f({U"alpha", U"bravo", U"charlie"});
    f.document().failPage = 1;
    const auto result = f.copy(select(f.session->pageId(0), 0, f.session->pageId(2), 3));
    CHECK(result.has_value());
    CHECK(!result->has_value());
    CHECK(result->error().message.find("page 2") != std::string::npos);
}

RIVET_TEST(copyCallbackIsDroppedWhenTheSessionCloses) {
    Fixture f({U"alpha", U"bravo", U"charlie"});
    f.session->textService().cache().setMaxBytes(0);
    f.document().parkPage = 1;
    bool delivered = false;
    f.session->textService().requestRangesText(
        f.ranges(select(f.session->pageId(0), 0, f.session->pageId(2), 3)),
        [&delivered](Result<std::string>) { delivered = true; });
    {
        std::unique_lock<std::mutex> lock(f.document().mutex);
        CHECK(f.document().cv.wait_for(lock, std::chrono::seconds(10), [&] { return f.document().parked; }));
    }
    TextDocument* document = &f.document();
    std::thread releaser([document] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard<std::mutex> lock(document->mutex);
        document->released = true;
        document->cv.notify_all();
    });
    f.session.reset(); // waits for the parked job; the job stops at the next page
    releaser.join();
    f.dispatcher.pump();
    CHECK(!delivered);
}

namespace {

class OutlineDocument final : public PdfDocument {
public:
    OutlineDocument() { info_.pageCount = 1; }
    const PdfDocumentInfo& info() const override { return info_; }
    Result<PdfPageInfo> pageInfo(std::size_t) const override {
        return PdfPageInfo{0, Size{100.0, 100.0}, rivet::core::PageRotation::None};
    }
    Result<Bitmap> renderPage(std::size_t, const Rect&, double) override { return Bitmap::create(2, 2); }
    Result<std::optional<rivet::pdf::PdfOutlineNode>> outline() const override {
        outlineThread = std::this_thread::get_id();
        ++outlineCalls;
        rivet::pdf::PdfOutlineNode root;
        rivet::pdf::PdfOutlineNode child;
        child.title = "Chapter";
        root.children.push_back(child);
        return std::optional<rivet::pdf::PdfOutlineNode>(root);
    }
    mutable std::thread::id outlineThread;
    mutable std::atomic<int> outlineCalls{0};

private:
    PdfDocumentInfo info_;
};

class OutlineEngine final : public PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "outline"; }
    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path&, std::string_view) override {
        return std::unique_ptr<PdfDocument>(std::make_unique<OutlineDocument>());
    }
};

} // namespace

// The outline walk is a backend call: it must run on a worker, be delivered
// on the main thread, and be loaded once.
RIVET_TEST(outlineLoadsOffTheMainThreadOnce) {
    OutlineEngine engine;
    TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    const auto path = std::filesystem::temp_directory_path() / "rivet-outline-async.pdf";
    if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
        std::fputs("%PDF-1.4\n", file);
        std::fclose(file);
    }
    auto session = DocumentSession::create(engine, scheduler, &dispatcher, path);
    CHECK(session.has_value());
    auto& links = (*session)->linkService();
    CHECK(links.cachedOutline() == nullptr);

    const std::thread::id mainThread = std::this_thread::get_id();
    int deliveries = 0;
    std::thread::id deliveredOn;
    for (int round = 0; round < 2; ++round) {
        links.requestOutline([&](rivet::editor::LinkService::Outline outline) {
            ++deliveries;
            deliveredOn = std::this_thread::get_id();
            CHECK(outline != nullptr && outline->has_value());
            CHECK_EQ((*outline)->children.front().title, std::string("Chapter"));
        });
        for (int i = 0; i < 2000 && deliveries <= round; ++i) {
            dispatcher.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK_EQ(deliveries, 2);
    CHECK(deliveredOn == mainThread);
    const auto& document = *static_cast<const OutlineDocument*>(&(*session)->document());
    CHECK(document.outlineThread != mainThread);
    CHECK_EQ(document.outlineCalls.load(), 1);
    CHECK(links.cachedOutline() != nullptr);
}
