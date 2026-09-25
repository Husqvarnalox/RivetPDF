#include "core/async/SerialExecutor.hpp"

#include "core/Log.hpp"

namespace rivet::core {

SerialExecutor::SerialExecutor(TaskScheduler& scheduler) : scheduler_(scheduler) {}

SerialExecutor::~SerialExecutor() {
    cancelPending();
    // Wait until the in-flight task (if any) has finished and no drain is
    // scheduled anymore. destroy-from-own-task is not supported (see header).
    waitUntilIdle();
}

void SerialExecutor::waitUntilIdle() {
    // running_ is only ever true while a drain is scheduled or executing, and
    // a drain exits only when the queue is empty, so the predicate means
    // "nothing queued, nothing executing".
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return !running_ && queue_.empty(); });
}

void SerialExecutor::post(std::function<void()> task) {
    if (!task) {
        return;
    }
    bool needSchedule = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needSchedule = !running_;
        running_ = true;
        queue_.push_back(std::move(task));
    }
    if (needSchedule) {
        // Exactly one drain is ever scheduled at a time: while running_ is
        // true, a drain is scheduled or looping, and it picks up everything
        // appended in the meantime (including recursive posts).
        try {
            scheduler_.post([this] { drain(); });
        } catch (...) {
            // Scheduling failed (e.g. allocation failure). running_ is already
            // true, so if nothing were scheduled the destructor would wait
            // forever. TaskScheduler::post either queues the drain or throws
            // without queueing it, and running_ was false before this post -
            // so no drain is in flight and draining inline on this thread
            // preserves FIFO and the one-at-a-time guarantee.
            drain();
        }
    }
}

void SerialExecutor::cancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    // A drain that is scheduled-but-not-started or mid-loop observes the
    // empty queue on its next iteration and goes idle after the in-flight
    // task completes.
}

bool SerialExecutor::hasPendingWork() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ || !queue_.empty();
}

void SerialExecutor::drain() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                running_ = false;
                idle_.notify_all(); // destructor (and any waiters) may proceed
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            task(); // mutex not held: tasks may post() recursively
        } catch (...) {
            // Keep draining and keep the idle promise even if a task escapes
            // with an exception, otherwise the destructor would hang forever.
            log::error("SerialExecutor: task exited with an uncaught exception");
        }
    }
}

} // namespace rivet::core
