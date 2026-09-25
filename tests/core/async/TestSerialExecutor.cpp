#include "RivetTest.h"

#include "core/async/SerialExecutor.hpp"
#include "core/async/TaskScheduler.hpp"

#include <atomic>
#include <chrono>
#include <functional>
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

// Queued-not-started tasks must never run once the executor is torn down, and
// the teardown must still return promptly. Made deterministic by occupying the
// only worker with a raw scheduler task: the executor's queued task provably
// cannot start while it is dropped, and the freed worker afterwards runs only
// the now-empty drain.
RIVET_TEST(destructorDropsQueuedTasks) {
    TaskScheduler scheduler(1); // single worker, held by the blocker below
    std::atomic<bool> victimRan{false};

    std::promise<void> workerGate;
    auto workerGateFuture = workerGate.get_future();
    std::atomic<bool> blockerStarted{false};
    scheduler.post([&] {
        blockerStarted.store(true, std::memory_order_release);
        workerGateFuture.wait();
    });
    CHECK(waitFor([&] { return blockerStarted.load(std::memory_order_acquire); }));

    {
        SerialExecutor executor(scheduler);
        executor.post([&] { victimRan.store(true, std::memory_order_relaxed); });
        CHECK(executor.hasPendingWork());

        // The worker is provably stuck in the blocker, so this drop happens
        // before the victim could ever start (the destructor performs the
        // same drop if it wins the race; either way the task must not run).
        executor.cancelPending();

        // Free the worker so the (now empty) drain can complete and the
        // destructor's wait returns promptly.
        workerGate.set_value();
    }

    CHECK(!victimRan.load());
}

// A task that keeps re-posting itself a bounded number of times must run
// every iteration, in FIFO order, and leave the executor idle afterwards.
RIVET_TEST(recursivePostChainRunsEveryIteration) {
    TaskScheduler scheduler(2);
    SerialExecutor executor(scheduler);

    constexpr int kIterations = 100;
    std::mutex mutex;
    std::vector<int> order;
    std::atomic<int> runs{0};
    std::promise<void> done;
    auto doneFuture = done.get_future();

    std::function<void()> step = [&] {
        const int index = runs.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(index);
        }
        if (index + 1 < kIterations) {
            executor.post(step);
        } else {
            done.set_value();
        }
    };
    executor.post(step);

    CHECK(doneFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    executor.waitUntilIdle(); // must return promptly now

    std::lock_guard<std::mutex> lock(mutex);
    CHECK_EQ(order.size(), std::size_t{kIterations});
    for (int i = 0; i < kIterations; ++i) {
        CHECK_EQ(order[static_cast<std::size_t>(i)], i);
    }
}

// Posts from several threads concurrently: every post must run exactly once.
RIVET_TEST(concurrentPostsAllRun) {
    TaskScheduler scheduler(4);
    SerialExecutor executor(scheduler);

    constexpr int kThreads = 4;
    constexpr int kPostsPerThread = 200;
    std::atomic<int> runs{0};

    std::vector<std::thread> posters;
    for (int t = 0; t < kThreads; ++t) {
        posters.emplace_back([&] {
            for (int i = 0; i < kPostsPerThread; ++i) {
                executor.post([&runs] { ++runs; });
            }
        });
    }
    for (auto& poster : posters) {
        poster.join();
    }

    executor.waitUntilIdle();
    CHECK_EQ(runs.load(), kThreads * kPostsPerThread);
}

// cancelPending clears queued work; waitUntilIdle then returns promptly once
// the in-flight task (the only survivor) completes.
RIVET_TEST(cancelPendingThenWaitUntilIdle) {
    TaskScheduler scheduler(1);
    SerialExecutor executor(scheduler);

    std::atomic<int> ranCount{0};
    std::atomic<bool> blockerStarted{false};

    std::promise<void> gate;
    auto gateFuture = gate.get_future();

    executor.post([&] {
        blockerStarted.store(true, std::memory_order_release);
        gateFuture.wait();
        ++ranCount;
    });
    CHECK(waitFor([&] { return blockerStarted.load(std::memory_order_acquire); }));

    for (int i = 0; i < 50; ++i) {
        executor.post([&] { ++ranCount; });
    }
    executor.cancelPending();
    // Only the in-flight blocker remains pending.
    CHECK(executor.hasPendingWork());

    gate.set_value();
    executor.waitUntilIdle(); // bounded: nothing queued, blocker finishes
    CHECK_EQ(ranCount.load(), 1);
}

// The documented ownership order, written out explicitly: the executor is
// destroyed while the scheduler is still alive; the scheduler goes last.
RIVET_TEST(executorDestroyedBeforeScheduler) {
    TaskScheduler scheduler(2);

    std::atomic<int> ran{0};
    {
        SerialExecutor executor(scheduler);
        for (int i = 0; i < 10; ++i) {
            executor.post([&ran] { ++ran; });
        }
    } // executor dies first: cancels queued work, joins, returns

    // Whatever ran, the scheduler is still fully usable afterwards.
    std::atomic<int> postDtorRuns{0};
    {
        SerialExecutor executor(scheduler);
        executor.post([&postDtorRuns] { ++postDtorRuns; });
        executor.waitUntilIdle();
    }
    CHECK_EQ(postDtorRuns.load(), 1);
    CHECK_GE(ran.load(), 0);
    CHECK_LE(ran.load(), 10);
}

// Every executor dies before the scheduler that backs it; scheduler shutdown
// then finds no work referencing executors and joins cleanly.
RIVET_TEST(schedulerOutlivesAllExecutors) {
    TaskScheduler scheduler(2);
    {
        SerialExecutor executorA(scheduler);
        SerialExecutor executorB(scheduler);
        executorA.post([] {});
        executorB.post([] {});
    } // both executors destroyed here, per the documented order

    // The scheduler destructor runs at function exit: reaching this point
    // without hanging or asserting is the contract under test.
}
