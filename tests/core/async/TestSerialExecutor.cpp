#include "RivetTest.h"

#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using rivet::core::SerialExecutor;
using rivet::core::TaskScheduler;

namespace {

// Polls the predicate every 2 ms for up to ~5 s. Correctness assertions must
// never depend on bare sleeps; this only bounds how long a test waits for an
// expected state before failing.
template <typename Pred>
bool waitFor(Pred&& pred) {
    for (int attempt = 0; attempt < 2500; ++attempt) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

} // namespace

RIVET_TEST(preservesFifoOrder) {
    TaskScheduler scheduler(4);
    SerialExecutor executor(scheduler);

    constexpr int kTaskCount = 200;
    std::mutex mutex;
    std::vector<int> order;
    std::promise<void> drained;
    auto drainedFuture = drained.get_future();

    for (int i = 0; i < kTaskCount; ++i) {
        executor.post([&mutex, &order, &drained, i] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(i);
            }
            if (i == kTaskCount - 1) {
                drained.set_value();
            }
        });
    }

    CHECK(drainedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    std::lock_guard<std::mutex> lock(mutex);
    CHECK_EQ(order.size(), std::size_t{200});
    for (int i = 0; i < kTaskCount; ++i) {
        CHECK_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

RIVET_TEST(neverOverlaps) {
    TaskScheduler scheduler(4);
    SerialExecutor executor(scheduler);

    constexpr int kTaskCount = 50;
    std::atomic<int> inFlight{0};
    std::atomic<int> maxInFlight{0};
    std::atomic<int> completed{0};
    std::atomic<bool> overlapDetected{false};
    std::promise<void> drained;
    auto drainedFuture = drained.get_future();

    for (int i = 0; i < kTaskCount; ++i) {
        executor.post([&] {
            const int current = ++inFlight;
            // CHECK is deliberately not used inside the task: it throws, and
            // an exception escaping into the worker pool would be swallowed
            // there instead of failing the test. Record and assert below.
            int observedMax = maxInFlight.load(std::memory_order_relaxed);
            while (current > observedMax &&
                   !maxInFlight.compare_exchange_weak(
                       observedMax, current, std::memory_order_relaxed)) {
            }
            if (current != 1) {
                overlapDetected.store(true, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            --inFlight;

            if (++completed == kTaskCount) {
                drained.set_value();
            }
        });
    }

    CHECK(drainedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    CHECK(!overlapDetected.load());
    CHECK_EQ(maxInFlight.load(), 1);
}

RIVET_TEST(cancelPendingDropsQueued) {
    TaskScheduler scheduler(2);
    SerialExecutor executor(scheduler);

    std::atomic<int> ranCount{0};
    std::atomic<bool> firstTaskStarted{false};

    std::promise<void> gate;
    auto gateFuture = gate.get_future();

    executor.post([&] {
        firstTaskStarted.store(true, std::memory_order_release);
        gateFuture.wait();
        ++ranCount;
    });
    CHECK(waitFor([&] { return firstTaskStarted.load(std::memory_order_acquire); }));

    for (int i = 0; i < 50; ++i) {
        executor.post([&] { ++ranCount; });
    }
    CHECK(executor.hasPendingWork());

    executor.cancelPending();
    // The in-flight task is still running, so work is still pending.
    CHECK(executor.hasPendingWork());

    gate.set_value();
    CHECK(waitFor([&] { return !executor.hasPendingWork(); }));
    CHECK_EQ(ranCount.load(), 1);
}

RIVET_TEST(destructorWaitsForInFlight) {
    TaskScheduler scheduler(2);
    std::atomic<bool> taskStarted{false};
    std::atomic<bool> taskCompleted{false};

    {
        SerialExecutor executor(scheduler);
        executor.post([&] {
            taskStarted.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            taskCompleted.store(true, std::memory_order_release);
        });
        // Only leave the scope once the task is in flight; otherwise
        // cancelPending() would legitimately drop it before it starts.
        CHECK(waitFor([&] { return taskStarted.load(std::memory_order_acquire); }));
        executor.cancelPending();
    } // destructor must block until the in-flight task has completed

    CHECK(taskCompleted.load());
}

RIVET_TEST(twoExecutorsRunConcurrently) {
    TaskScheduler scheduler(2);
    SerialExecutor executorA(scheduler);
    SerialExecutor executorB(scheduler);

    std::atomic<bool> aStarted{false};
    std::atomic<bool> bStarted{false};
    std::atomic<bool> aSawBStarted{false};
    std::atomic<bool> bSawAStarted{false};
    std::atomic<int> completed{0};
    std::promise<void> done;
    auto doneFuture = done.get_future();

    executorA.post([&] {
        aStarted.store(true, std::memory_order_release);
        // Bounded cross-wait: if executors were serialized against each other
        // this times out instead of deadlocking, and the CHECKs below fail.
        if (waitFor([&] { return bStarted.load(std::memory_order_acquire); })) {
            aSawBStarted.store(true, std::memory_order_relaxed);
        }
        if (++completed == 2) {
            done.set_value();
        }
    });
    executorB.post([&] {
        bStarted.store(true, std::memory_order_release);
        if (waitFor([&] { return aStarted.load(std::memory_order_acquire); })) {
            bSawAStarted.store(true, std::memory_order_relaxed);
        }
        if (++completed == 2) {
            done.set_value();
        }
    });

    CHECK(doneFuture.wait_for(std::chrono::seconds(12)) == std::future_status::ready);
    CHECK(aSawBStarted.load());
    CHECK(bSawAStarted.load());
}

RIVET_TEST(recursivePostRunsAfterCurrentTask) {
    TaskScheduler scheduler(2);
    SerialExecutor executor(scheduler);

    std::mutex mutex;
    std::vector<int> order;
    std::promise<void> done;
    auto doneFuture = done.get_future();

    executor.post([&] {
        // Recursive post from inside a running task must not deadlock and
        // must run after the current task returns.
        executor.post([&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(1);
            }
            done.set_value();
        });
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(0);
        }
    });

    CHECK(doneFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    std::lock_guard<std::mutex> lock(mutex);
    CHECK_EQ(order.size(), std::size_t{2});
    CHECK_EQ(order[0], 0);
    CHECK_EQ(order[1], 1);
}
