#include "RivetTest.h"

#include "core/async/TaskScheduler.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

RIVET_TEST(runsPostedTasks) {
    TaskScheduler scheduler(3);

    constexpr std::size_t kTaskCount = 16;
    std::vector<std::promise<void>> promises(kTaskCount);
    std::vector<std::future<void>> futures;
    futures.reserve(kTaskCount);
    for (auto& promise : promises) {
        futures.push_back(promise.get_future());
    }

    for (std::size_t i = 0; i < kTaskCount; ++i) {
        scheduler.post([&promises, i] { promises[i].set_value(); });
    }

    for (auto& future : futures) {
        CHECK(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }
}

RIVET_TEST(autoThreadCountSane) {
    {
        TaskScheduler scheduler;
        CHECK_GE(scheduler.threadCount(), std::size_t{2});
        CHECK_LE(scheduler.threadCount(), std::size_t{8});
    }
    {
        TaskScheduler scheduler(3);
        CHECK_EQ(scheduler.threadCount(), std::size_t{3});
    }
}

RIVET_TEST(tasksExecuteOnPoolThreads) {
    TaskScheduler scheduler(2);
    const auto mainThreadId = std::this_thread::get_id();

    std::mutex mutex;
    std::vector<std::thread::id> executedOn;
    std::atomic<int> remaining{8};
    std::promise<void> allDone;
    auto allDoneFuture = allDone.get_future();

    for (int i = 0; i < 8; ++i) {
        scheduler.post([&] {
            {
                std::lock_guard<std::mutex> lock(mutex);
                executedOn.push_back(std::this_thread::get_id());
            }
            if (--remaining == 0) {
                allDone.set_value();
            }
        });
    }

    CHECK(allDoneFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

    std::lock_guard<std::mutex> lock(mutex);
    CHECK_EQ(executedOn.size(), std::size_t{8});
    for (const auto threadId : executedOn) {
        CHECK(threadId != mainThreadId);
    }
}

RIVET_TEST(destructorDiscardsPending) {
    std::atomic<int> ranCount{0};
    std::atomic<bool> firstTaskStarted{false};

    // Heap-allocated so the destructor can run on a helper thread while the
    // in-flight task is still blocked. Owned by the unique_ptr for exception
    // safety, but observed through the raw pointer below: unique_ptr::reset()
    // nulls its pointer BEFORE running the destructor, which would turn the
    // destructor-progress observation below into a null dereference.
    auto owned = std::make_unique<TaskScheduler>(1);
    TaskScheduler* scheduler = owned.get();

    std::promise<void> gate;
    auto gateFuture = gate.get_future();

    scheduler->post([&] {
        firstTaskStarted.store(true, std::memory_order_release);
        gateFuture.wait();
        ++ranCount;
    });
    CHECK(waitFor([&] { return firstTaskStarted.load(std::memory_order_acquire); }));

    for (int i = 0; i < 100; ++i) {
        scheduler->post([&] { ++ranCount; });
    }
    // The single worker is blocked in the first task, so all 100 are queued.
    CHECK_EQ(scheduler->pendingCount(), std::size_t{100});

    std::promise<void> destroyed;
    auto destroyedFuture = destroyed.get_future();
    std::thread destroyer([&owned, &destroyed] {
        owned.reset(); // discards queued tasks, joins the in-flight one
        destroyed.set_value();
    });

    // The destructor empties the queue before joining the workers, so
    // pendingCount() reaching zero is the signal that destruction has begun.
    // Dereferencing the raw pointer is safe here: the object stays alive
    // (its destructor is blocked joining the worker) until the gate below
    // opens, and the pointer variable itself is never written after init.
    CHECK(waitFor([&] { return scheduler->pendingCount() == 0; }));
    gate.set_value();

    CHECK(destroyedFuture.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    destroyer.join();
    CHECK_EQ(ranCount.load(), 1);
}
