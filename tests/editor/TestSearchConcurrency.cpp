// SPDX-License-Identifier: MPL-2.0
// Regression tests for the search thread/lifetime contract:
//   - worker results reach the callback ONLY through the main dispatcher;
//   - the callback never runs under the controller's mutex (re-entrancy);
//   - request tokens are never reused (cancel/restart ABA);
//   - destruction drains queued walkers (no walker after the destructor).
#include "RivetTest.h"

#include "editor/DocumentSession.hpp"
#include "editor/TextSearchController.hpp"
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
#include <string>
#include <thread>
#include <vector>

using rivet::core::Bitmap;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::editor::DocumentSession;
using rivet::editor::TextSearchController;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfPageInfo;
using rivet::pdf::PdfTextPage;
using rivet::pdf::TextChar;

namespace {

std::shared_ptr<const PdfTextPage> makePage(const std::string& text) {
    std::vector<TextChar> chars;
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

// Gate that a worker can be parked on: arrive() blocks the caller until
// open() is called; the test waits for arrivals deterministically.
class Gate {
public:
    void arriveAndWait() {
        std::unique_lock<std::mutex> lock(mutex_);
        ++arrived_;
        cv_.notify_all();
        cv_.wait(lock, [this] { return open_; });
    }
    bool waitForArrivals(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(10), [&] { return arrived_ >= count; });
    }
    void open() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int arrived_ = 0;
    bool open_ = false;
};

// Page texts: "alpha one", "alpha two", "zeta". textPage() of page 0 can be
// parked on a gate (armed by the test) to hold a walker in flight.
class GatedTextDocument final : public PdfDocument {
public:
    GatedTextDocument() {
        info_.pageCount = 3;
        info_.title = "gated";
    }
    const PdfDocumentInfo& info() const override { return info_; }
    Result<PdfPageInfo> pageInfo(std::size_t pageIndex) const override {
        if (pageIndex >= 3) return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        return PdfPageInfo{pageIndex, Size{612.0, 792.0}, rivet::core::PageRotation::None};
    }
    Result<Bitmap> renderPage(std::size_t, const Rect&, double) override { return Bitmap::create(4, 4); }
    Result<std::shared_ptr<const PdfTextPage>> textPage(std::size_t pageIndex) const override {
        if (pageIndex == 0) {
            std::shared_ptr<Gate> gate;
            {
                std::lock_guard<std::mutex> lock(gateMutex_);
                gate = gate_;
            }
            if (gate) gate->arriveAndWait();
        }
        switch (pageIndex) {
        case 0: return makePage("alpha one");
        case 1: return makePage("alpha two");
        case 2: return makePage("zeta");
        default: return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        }
    }
    void armGate(std::shared_ptr<Gate> gate) const {
        std::lock_guard<std::mutex> lock(gateMutex_);
        gate_ = std::move(gate);
    }

private:
    PdfDocumentInfo info_;
    mutable std::mutex gateMutex_;
    mutable std::shared_ptr<Gate> gate_;
};

class GatedEngine final : public PdfEngine {
public:
    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "gated"; }
    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path&,
                                                      std::string_view) override {
        return std::unique_ptr<PdfDocument>(std::make_unique<GatedTextDocument>());
    }
};

// Queue dispatcher pumped by the test thread (the "main thread"). Records
// every post so tests can prove the worker never invoked the callback.
class QueueDispatcher final : public rivet::core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    std::size_t pump() {
        std::deque<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            run.swap(queue_);
        }
        for (auto& task : run) task();
        return run.size();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

struct Fixture {
    GatedEngine engine;
    TaskScheduler scheduler{3};
    QueueDispatcher dispatcher;
    std::unique_ptr<DocumentSession> session;

    Fixture() {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "rivet-search-concurrency.pdf";
        if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
            std::fputs("%PDF-1.4\n", file);
            std::fclose(file);
        }
        auto created = DocumentSession::create(engine, scheduler, &dispatcher, path);
        CHECK(created.has_value());
        session = std::move(*created);
    }
    const GatedTextDocument& document() {
        return *static_cast<const GatedTextDocument*>(&session->document());
    }
};

// Pumps the dispatcher until the predicate holds (bounded).
template <typename Predicate>
bool pumpUntil(QueueDispatcher& dispatcher, Predicate predicate) {
    for (int i = 0; i < 2000; ++i) {
        dispatcher.pump();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace

RIVET_TEST(searchWorkerNotificationsArriveOnlyThroughTheDispatcher) {
    Fixture f;
    TextSearchController search(*f.session, f.session->textService());
    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<int> offMainCalls{0};
    int calls = 0;
    search.setOnResultsChanged([&] {
        if (std::this_thread::get_id() != mainThread) ++offMainCalls;
        ++calls;
    });

    search.start("alpha");
    // start() itself notifies synchronously on the calling (main) thread.
    CHECK_EQ(calls, 1);
    CHECK(pumpUntil(f.dispatcher, [&] { return !search.searching(); }));
    CHECK_EQ(search.matchCount(), std::size_t{2});
    CHECK(calls >= 2); // at least one worker-originated notification, pumped
    CHECK_EQ(offMainCalls.load(), 0);
}

RIVET_TEST(searchCallbackMayReenterTheController) {
    Fixture f;
    TextSearchController search(*f.session, f.session->textService());
    // The callback re-enters every accessor and mutator. With a callback run
    // under the internal mutex this would self-deadlock.
    bool stepped = false;
    std::size_t lastCount = 0;
    search.setOnResultsChanged([&] {
        lastCount = search.matches().size();
        (void)search.currentIndex();
        (void)search.query();
        (void)search.searching();
        if (!stepped && lastCount > 0 && !search.searching()) {
            stepped = true;
            search.next(); // nested notification, outside any lock
        }
    });
    search.start("alpha");
    CHECK(pumpUntil(f.dispatcher, [&] { return stepped; }));
    CHECK_EQ(search.currentIndex(), std::optional<std::size_t>(0));
    search.setCurrentIndex(1);
    search.previous();
    search.cancel();
    search.start(""); // empty-query path notifies too
    CHECK_EQ(lastCount, std::size_t{0});
}

// The ABA scenario from the review: A = token 1, cancel, B. With a scheme
// that resets the token to 0 on cancel, B would reuse token 1 and the parked
// walker A would publish its "alpha" matches into B's results.
RIVET_TEST(searchRequestTokensAreNeverReused) {
    Fixture f;
    TextSearchController search(*f.session, f.session->textService());
    auto gate = std::make_shared<Gate>();
    f.document().armGate(gate);

    search.start("alpha");
    const std::uint64_t tokenA = search.activeRequestForTesting();
    CHECK(gate->waitForArrivals(1)); // walker A is parked inside page 0
    search.cancel();
    const std::uint64_t tokenCancel = search.activeRequestForTesting();
    search.start("zeta");
    const std::uint64_t tokenB = search.activeRequestForTesting();
    CHECK(tokenCancel > tokenA);
    CHECK(tokenB > tokenCancel);

    f.document().armGate(nullptr);
    gate->open(); // walker A resumes with a stale token and must exit
    CHECK(pumpUntil(f.dispatcher, [&] { return !search.searching(); }));
    const auto matches = search.matches();
    CHECK_EQ(matches.size(), std::size_t{1});
    CHECK_EQ(matches[0].page, f.session->pageId(2)); // only "zeta"
    CHECK_EQ(search.query(), "zeta");
}

RIVET_TEST(searchCancelKeepsTokensMonotonicAcrossManyRestarts) {
    Fixture f;
    TextSearchController search(*f.session, f.session->textService());
    std::uint64_t last = search.activeRequestForTesting();
    for (int i = 0; i < 50; ++i) {
        search.start(i % 2 == 0 ? "alpha" : "zeta");
        CHECK(search.activeRequestForTesting() > last);
        last = search.activeRequestForTesting();
        search.cancel();
        CHECK(search.activeRequestForTesting() > last);
        last = search.activeRequestForTesting();
    }
    search.start("alpha");
    CHECK(pumpUntil(f.dispatcher, [&] { return !search.searching(); }));
    CHECK_EQ(search.matchCount(), std::size_t{2});
}

// Destruction while a walker is parked AND another is queued behind it: the
// destructor must drop the queued walker and wait for the parked one; no
// walker may run afterwards (ASan/TSan catch any touch of the dead object).
RIVET_TEST(searchDestructionDrainsQueuedWalkers) {
    Fixture f;
    auto gate = std::make_shared<Gate>();
    f.document().armGate(gate);
    auto search = std::make_unique<TextSearchController>(*f.session, f.session->textService());
    int calls = 0;
    search->setOnResultsChanged([&] { ++calls; });
    search->start("alpha");
    CHECK(gate->waitForArrivals(1));
    // Queue more walkers behind the parked one (each start drops the
    // previously queued walker, one stays queued).
    search->start("zeta");
    search->start("alpha");

    std::thread releaser([gate] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        gate->open();
    });
    search.reset(); // blocks until the parked walker exits
    releaser.join();
    f.document().armGate(nullptr);
    const int callsAtDestruction = calls;
    // Notifications posted before destruction are no-ops now.
    f.dispatcher.pump();
    CHECK_EQ(calls, callsAtDestruction);
}

RIVET_TEST(searchTeardownStress) {
    Fixture f;
    for (int round = 0; round < 200; ++round) {
        auto search = std::make_unique<TextSearchController>(*f.session, f.session->textService());
        search->setOnResultsChanged([] {});
        search->start(round % 3 == 0 ? "alpha" : "zeta");
        if (round % 2 == 0) search->start("alpha");
        if (round % 5 == 0) search->cancel();
        if (round % 7 == 0) f.dispatcher.pump();
        search.reset();
    }
    f.dispatcher.pump(); // stale notifications of dead controllers: no-ops
}
