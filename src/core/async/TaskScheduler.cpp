#include "core/async/TaskScheduler.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace rivet::core {

std::size_t TaskScheduler::resolveThreadCount(unsigned requested) {
    if (requested != 0) {
        return requested;
    }
    // Auto: leave one core for the main/UI thread, clamped to a sane range.
    // hardware_concurrency() may return 0 on failure; treat that as one core.
    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned usable = hardware > 0 ? hardware - 1 : 1;
    return std::clamp<std::size_t>(usable, 2, 8);
}

TaskScheduler::TaskScheduler(unsigned threadCount) {
    const std::size_t count = resolveThreadCount(threadCount);
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        workers_.emplace_back([this](std::stop_token stopToken) {
            workerLoop(stopToken);
        });
    }
}

TaskScheduler::~TaskScheduler() {
    {
        // Fast shutdown: drop everything not yet started. The in-flight task
        // of each worker finishes below, before the join completes.
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }
    for (auto& worker : workers_) {
        worker.request_stop();
    }
    cv_.notify_all();
    // workers_ is declared last, so its destructor joins every worker here,
    // before mutex_ and queue_ are destroyed.
}

void TaskScheduler::post(std::function<void()> task) {
    if (!task) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(task));
    }
    cv_.notify_one();
}

std::size_t TaskScheduler::threadCount() const {
    return workers_.size();
}

std::size_t TaskScheduler::pendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void TaskScheduler::workerLoop(std::stop_token stopToken) {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Wake on work, or on stop: waiting with a stop_token is also
            // unblocked automatically by request_stop().
            cv_.wait(lock, stopToken, [this] { return !queue_.empty(); });
            if (stopToken.stop_requested()) {
                return; // discard whatever is still queued
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            task();
        } catch (...) {
            // A task escaping with an exception must not take down its
            // worker; tasks are expected to report failures themselves.
            log::error("TaskScheduler: task exited with an uncaught exception");
        }
    }
}

} // namespace rivet::core
