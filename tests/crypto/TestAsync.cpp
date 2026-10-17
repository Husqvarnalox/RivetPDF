// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "crypto/Async.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

using namespace rivet;
using namespace rivet::crypto;

namespace {

// Queues posted work; the test thread plays the "main thread" by draining it.
class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    // Runs queued tasks on the calling thread until `count` ran or timeout.
    bool drain(std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::size_t ran = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (ran < count) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait_until(lock, deadline, [&] { return !queue_.empty(); })) return false;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task(); // never under the lock
            ++ran;
        }
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};

} // namespace

RIVET_TEST(asyncDeliversResultOnDispatcherThread) {
    core::TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    const auto mainThread = std::this_thread::get_id();
    std::thread::id workThread, doneThread;
    int value = 0;
    const bool started = runCryptoAsync<int>(
        scheduler, dispatcher, scope, CancelToken{},
        [&](const CancelToken&) -> CryptoResult<int> {
            workThread = std::this_thread::get_id();
            return 42;
        },
        [&](CryptoResult<int> r) {
            doneThread = std::this_thread::get_id();
            value = r.value_or(-1);
        });
    CHECK(started);
    CHECK(dispatcher.drain(1));
    CHECK_EQ(value, 42);
    CHECK(workThread != mainThread);
    CHECK(doneThread == mainThread);
    scope.closeAndWait();
}

RIVET_TEST(asyncReportsCancellationAndNeverRunsWork) {
    core::TaskScheduler scheduler(1);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    CancelToken cancel;
    cancel.cancel();
    std::atomic<bool> workRan{false};
    CryptoErrorCode seen = CryptoErrorCode::Internal;
    CHECK(runCryptoAsync<int>(
        scheduler, dispatcher, scope, cancel,
        [&](const CancelToken&) -> CryptoResult<int> {
            workRan = true;
            return 1;
        },
        [&](CryptoResult<int> r) { seen = r ? CryptoErrorCode::Internal : r.error().code; }));
    CHECK(dispatcher.drain(1));
    CHECK(!workRan.load());
    CHECK_EQ(seen, CryptoErrorCode::Cancelled);
    scope.closeAndWait();
}

RIVET_TEST(asyncCancelDuringWorkDiscardsResult) {
    core::TaskScheduler scheduler(1);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    CancelToken cancel;
    std::atomic<bool> inWork{false};
    CryptoErrorCode seen = CryptoErrorCode::Internal;
    CHECK(runCryptoAsync<int>(
        scheduler, dispatcher, scope, cancel,
        [&](const CancelToken& token) -> CryptoResult<int> {
            inWork = true;
            while (!token.cancelled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return 7; // finished its call anyway; must be reported as Cancelled
        },
        [&](CryptoResult<int> r) { seen = r ? CryptoErrorCode::Internal : r.error().code; }));
    while (!inWork.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    cancel.cancel();
    CHECK(dispatcher.drain(1));
    CHECK_EQ(seen, CryptoErrorCode::Cancelled);
    scope.closeAndWait();
}

RIVET_TEST(asyncRefusesWhenScopeClosed) {
    core::TaskScheduler scheduler(1);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    scope.closeAndWait();
    bool called = false;
    const bool started = runCryptoAsync<int>(
        scheduler, dispatcher, scope, CancelToken{}, [](const CancelToken&) -> CryptoResult<int> { return 1; },
        [&](CryptoResult<int>) { called = true; });
    CHECK(!started);
    CHECK(!called);
}
