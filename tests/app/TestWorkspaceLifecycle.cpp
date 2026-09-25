// SPDX-License-Identifier: MPL-2.0
// Deterministic lifecycle tests for the asynchronous open pipeline:
//   - shutting the workspace down while an open is inside the engine waits
//     for it (the borrowed engine/scheduler outlive every use);
//   - produced-but-undelivered sessions are destroyed deterministically;
//   - stale completions never land in another (or a newer) tab.
#include "RivetTest.h"

#include "app/DocumentWorkspace.hpp"
#include "core/Error.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfTypes.hpp"

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
#include <utility>

using rivet::app::DocumentTab;
using rivet::app::DocumentWorkspace;
using rivet::core::Bitmap;
using rivet::core::Error;
using rivet::core::ErrorCode;
using rivet::core::Rect;
using rivet::core::Result;
using rivet::core::Size;
using rivet::core::TaskScheduler;
using rivet::pdf::PdfDocument;
using rivet::pdf::PdfDocumentInfo;
using rivet::pdf::PdfEngine;
using rivet::pdf::PdfPageInfo;

namespace {

std::atomic<int> gLiveDocuments{0};

class CountedDocument final : public PdfDocument {
public:
    explicit CountedDocument(std::string title) {
        info_.pageCount = 1;
        info_.title = std::move(title);
        ++gLiveDocuments;
    }
    ~CountedDocument() override { --gLiveDocuments; }
    const PdfDocumentInfo& info() const override { return info_; }
    Result<PdfPageInfo> pageInfo(std::size_t index) const override {
        if (index != 0) return std::unexpected(Error{ErrorCode::InvalidArgument, "page", "test"});
        return PdfPageInfo{0, Size{100.0, 100.0}, rivet::core::PageRotation::None};
    }
    Result<Bitmap> renderPage(std::size_t, const Rect&, double) override { return Bitmap::create(2, 2); }

private:
    PdfDocumentInfo info_;
};

// Engine whose opens of ONE chosen path park until released. Tracks how many
// opens are inside the engine and whether any ran after destruction began.
class GatedEngine final : public PdfEngine {
public:
    ~GatedEngine() override {
        destroyed_.store(true);
        if (active_.load() != 0) useAfterDestroy_.store(true);
    }

    bool isAvailable() const override { return true; }
    std::string_view backendName() const override { return "gated"; }

    Result<std::unique_ptr<PdfDocument>> openDocument(const std::filesystem::path& path,
                                                      std::string_view password) override {
        ++active_;
        if (destroyed_.load()) useAfterDestroy_.store(true);
        if (path == gatedPath) {
            std::unique_lock<std::mutex> lock(mutex_);
            ++arrived_;
            cv_.notify_all();
            cv_.wait(lock, [this] { return released_; });
        }
        Result<std::unique_ptr<PdfDocument>> result = [&]() -> Result<std::unique_ptr<PdfDocument>> {
            if (failPath == path) return std::unexpected(Error{ErrorCode::InvalidDocument, "corrupt", "test"});
            if (passwordPath == path && password.empty()) {
                return std::unexpected(Error{ErrorCode::PasswordRequired, "locked", "test"});
            }
            return std::unique_ptr<PdfDocument>(std::make_unique<CountedDocument>(path.filename().string()));
        }();
        --active_;
        return result;
    }

    bool waitArrived(int count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(10), [&] { return arrived_ >= count; });
    }
    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }
    int active() const { return active_.load(); }

    std::filesystem::path gatedPath;
    std::filesystem::path failPath;
    std::filesystem::path passwordPath;
    static inline std::atomic<bool> useAfterDestroy_{false};

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int arrived_ = 0;
    bool released_ = false;
    std::atomic<int> active_{0};
    std::atomic<bool> destroyed_{false};
};

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
    std::size_t pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> queue_;
};

std::filesystem::path tempPdf(const char* name) {
    auto path = std::filesystem::temp_directory_path() /
                std::filesystem::path{std::string{"rivet-lifecycle-"} + name + ".pdf"};
    if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
        std::fputs("%PDF-1.4\n", file);
        std::fclose(file);
    }
    return path;
}

template <typename Predicate>
bool pumpUntil(QueueDispatcher& dispatcher, Predicate predicate) {
    for (int i = 0; i < 2000; ++i) {
        dispatcher.pump();
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// Releases the engine from another thread after the main thread started
// blocking in the workspace destructor/shutdown.
std::thread releaseLater(GatedEngine& engine) {
    return std::thread([&engine] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        engine.release();
    });
}

} // namespace

RIVET_TEST(shutdownDuringOpenWaitsForTheEngineAndDropsTheSession) {
    GatedEngine::useAfterDestroy_.store(false);
    gLiveDocuments.store(0);
    {
        TaskScheduler scheduler{2};
        QueueDispatcher dispatcher;
        auto engine = std::make_unique<GatedEngine>();
        engine->gatedPath = tempPdf("shutdown-open");
        auto workspace = std::make_unique<DocumentWorkspace>(*engine, scheduler, &dispatcher);
        workspace->openDocument(engine->gatedPath);
        CHECK(engine->waitArrived(1)); // the open is inside the engine

        std::thread releaser = releaseLater(*engine);
        workspace.reset(); // must block until the open left the engine
        CHECK_EQ(engine->active(), 0);
        // The session created after cancellation was destroyed on the
        // worker; nothing leaked into a dead workspace.
        CHECK_EQ(gLiveDocuments.load(), 0);
        releaser.join();
        engine.reset(); // the shell destroys the engine after the workspace
        dispatcher.pump(); // late deliveries: no-ops
    }
    CHECK(!GatedEngine::useAfterDestroy_.load());
    CHECK_EQ(gLiveDocuments.load(), 0);
}

RIVET_TEST(shutdownDuringPasswordRetryAndCorruptOpenIsSafe) {
    GatedEngine::useAfterDestroy_.store(false);
    gLiveDocuments.store(0);
    {
        TaskScheduler scheduler{3};
        QueueDispatcher dispatcher;
        auto engine = std::make_unique<GatedEngine>();
        const auto locked = tempPdf("shutdown-locked");
        engine->passwordPath = locked;
        engine->failPath = tempPdf("shutdown-corrupt");
        auto workspace = std::make_unique<DocumentWorkspace>(*engine, scheduler, &dispatcher);
        workspace->openDocument(locked);
        CHECK(pumpUntil(dispatcher, [&] {
            return workspace->activeTab()->state() == DocumentTab::State::NeedsPassword;
        }));
        // The retry parks inside the engine; a corrupt open runs alongside.
        engine->gatedPath = locked;
        workspace->retryWithPassword(0, "secret");
        CHECK(engine->waitArrived(1));
        workspace->openDocument(engine->failPath);

        std::thread releaser = releaseLater(*engine);
        workspace.reset();
        CHECK_EQ(engine->active(), 0);
        CHECK_EQ(gLiveDocuments.load(), 0);
        releaser.join();
        engine.reset();
        dispatcher.pump();
    }
    CHECK(!GatedEngine::useAfterDestroy_.load());
}

RIVET_TEST(undeliveredResultsAreDestroyedAtShutdown) {
    gLiveDocuments.store(0);
    TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    GatedEngine engine;
    auto workspace = std::make_unique<DocumentWorkspace>(engine, scheduler, &dispatcher);
    workspace->openDocument(tempPdf("undelivered"));
    // Wait until the worker produced the session and posted the delivery,
    // WITHOUT running it.
    for (int i = 0; i < 2000 && dispatcher.pending() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_EQ(dispatcher.pending(), std::size_t{1});
    CHECK_EQ(gLiveDocuments.load(), 1);
    workspace.reset();
    CHECK_EQ(gLiveDocuments.load(), 0); // destroyed deterministically
    dispatcher.pump();                  // the stale delivery is a no-op
    CHECK_EQ(gLiveDocuments.load(), 0);
}

// open A -> close A before completion -> open B -> deliver A: B untouched.
RIVET_TEST(staleCompletionNeverLandsInANewTab) {
    gLiveDocuments.store(0);
    TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    GatedEngine engine;
    engine.gatedPath = tempPdf("stale-a");
    DocumentWorkspace workspace(engine, scheduler, &dispatcher);

    workspace.openDocument(engine.gatedPath);
    CHECK(engine.waitArrived(1));
    const rivet::app::TabId tabA = workspace.tab(0)->id();
    workspace.closeTab(0);
    CHECK(workspace.isEmpty());

    workspace.openDocument(tempPdf("stale-b"));
    CHECK(pumpUntil(dispatcher, [&] { return workspace.activeTab()->state() == DocumentTab::State::Ready; }));
    DocumentTab* tabB = workspace.activeTab();
    CHECK(tabB->id() != tabA);
    const auto* sessionB = tabB->session();
    CHECK_EQ(sessionB->info().title, std::string("rivet-lifecycle-stale-b.pdf"));

    int activeChanges = 0;
    int tabsChanges = 0;
    workspace.setOnActiveTabChanged([&] { ++activeChanges; });
    workspace.setOnTabsChanged([&] { ++tabsChanges; });

    engine.release(); // A completes now
    for (int i = 0; i < 200; ++i) {
        dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_EQ(workspace.tabCount(), std::size_t{1});
    CHECK_EQ(workspace.activeTab(), tabB);
    CHECK_EQ(workspace.activeTab()->session(), sessionB);
    CHECK(workspace.activeTab()->state() == DocumentTab::State::Ready);
    CHECK_EQ(activeChanges, 0);
    CHECK_EQ(tabsChanges, 0);
    CHECK_EQ(gLiveDocuments.load(), 1); // A's document was dropped
}

// Reopening the SAME path after closing its loading tab creates a new tab
// identity: the old completion is dropped even though the path matches.
RIVET_TEST(staleCompletionForTheSamePathIsDropped) {
    TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    GatedEngine engine;
    const auto path = tempPdf("same-path");
    engine.gatedPath = path;
    DocumentWorkspace workspace(engine, scheduler, &dispatcher);

    workspace.openDocument(path);
    CHECK(engine.waitArrived(1));
    workspace.closeTab(0);
    engine.gatedPath.clear(); // the second open is not gated
    workspace.openDocument(path);
    CHECK(pumpUntil(dispatcher, [&] { return workspace.activeTab()->state() == DocumentTab::State::Ready; }));
    const auto* session = workspace.activeTab()->session();

    engine.release();
    for (int i = 0; i < 200; ++i) {
        dispatcher.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_EQ(workspace.tabCount(), std::size_t{1});
    CHECK_EQ(workspace.activeTab()->session(), session);
}

RIVET_TEST(tabIdsAreNeverReused) {
    TaskScheduler scheduler{2};
    QueueDispatcher dispatcher;
    GatedEngine engine;
    DocumentWorkspace workspace(engine, scheduler, &dispatcher);
    rivet::app::TabId last;
    for (int i = 0; i < 20; ++i) {
        workspace.openDocument(tempPdf("ids"));
        const rivet::app::TabId id = workspace.tab(0)->id();
        CHECK(id.value() > last.value());
        last = id;
        workspace.closeTab(0);
    }
    workspace.shutdown();
    workspace.shutdown(); // idempotent
    // Opening after shutdown is refused with a controlled error state.
    workspace.openDocument(tempPdf("after-shutdown"));
    CHECK(workspace.activeTab()->state() == DocumentTab::State::Error);
}
