#pragma once

#include "core/async/TaskScheduler.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace rivet::core {

// Serializes jobs belonging to one logical stream (one document) while
// executing them on the SHARED TaskScheduler. Exactly one task of a given
// SerialExecutor runs at any moment; tasks run in FIFO post order. There is
// NO dedicated OS thread per stream - idle executors cost nothing.
//
// Lifetime rules:
//   - The referenced TaskScheduler must outlive the executor; destroy
//     executors before the scheduler.
//   - Destroying a SerialExecutor from inside one of its own tasks is not
//     supported: the destructor waits for the in-flight task, i.e. itself.
//   - Destruction must not race with post()/cancelPending().
class SerialExecutor {
public:
    explicit SerialExecutor(TaskScheduler& scheduler);

    // Cancels queued tasks, then waits for the in-flight task to complete.
    ~SerialExecutor();

    SerialExecutor(const SerialExecutor&) = delete;
    SerialExecutor& operator=(const SerialExecutor&) = delete;

    // FIFO; one-at-a-time; safe from any thread; tasks may post recursively.
    void post(std::function<void()> task);

    // Drops queued-not-started tasks; the in-flight task continues.
    void cancelPending();

    // The shared pool this executor serializes onto (outlives the executor
    // per the lifetime rules above).
    TaskScheduler& scheduler() const { return scheduler_; }

    // Blocks until no task of this executor is queued or running. The
    // destructor uses it; tests and owners (DocumentRenderer teardown) may
    // call it to join the stream deterministically instead of polling.
    void waitUntilIdle();

    // True when a task is queued or currently running.
    bool hasPendingWork() const;

private:
    void drain();

    TaskScheduler& scheduler_;

    mutable std::mutex mutex_;
    std::condition_variable idle_; // signaled when running_ becomes false
    std::deque<std::function<void()>> queue_;
    // Guarded by mutex_: true while a drain is scheduled on the scheduler or
    // executing on one of its workers.
    bool running_ = false;
};

} // namespace rivet::core
