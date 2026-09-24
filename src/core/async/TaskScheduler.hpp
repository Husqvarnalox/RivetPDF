#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace rivet::core {

// Fixed-size worker pool over std::jthread. FIFO task queue. Used for
// background rasterization; UI work is marshaled back via IMainThreadDispatcher.
//
// Shutdown is intentionally fast: the destructor discards tasks that have not
// started yet and lets the in-flight tasks (at most one per worker) run to
// completion before joining. post() must not be called once the destructor has
// started; a task posted from another task during shutdown is discarded.
class TaskScheduler {
public:
    // threadCount == 0 -> auto: clamp(std::thread::hardware_concurrency() - 1, 2, 8).
    // An explicit threadCount is honored as-is.
    explicit TaskScheduler(unsigned threadCount = 0);

    // Requests stop, DISCARDS not-yet-started tasks, joins all workers.
    ~TaskScheduler();

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    // FIFO; safe to call from any thread; from any task too. Null tasks are
    // ignored (invoking an empty std::function would throw).
    void post(std::function<void()> task);

    // Fixed at construction.
    std::size_t threadCount() const;

    // Queued-not-started tasks; approximate, for tests/diagnostics.
    std::size_t pendingCount() const;

private:
    void workerLoop(std::stop_token stopToken);
    static std::size_t resolveThreadCount(unsigned requested);

    mutable std::mutex mutex_;
    std::condition_variable_any cv_; // any: supports the stop_token wait
    std::deque<std::function<void()>> queue_;

    // Declared last so it is destroyed first: joining the workers must happen
    // while the mutex and queue they touch are still alive.
    std::vector<std::jthread> workers_;
};

} // namespace rivet::core
