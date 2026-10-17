// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace rivet::core {

// The pending timers of a main-thread dispatcher (portable bookkeeping for
// IMainThreadDispatcher::postDelayed on the Linux and Windows backends): a
// mutex-guarded min-heap of (deadline, task). push() is thread-safe; the
// event loop asks millisecondsUntilNext() how long it may sleep and
// takeDue() for the tasks to run (deadline order, ties in push order).
// Tasks are never run under the lock (takeDue() hands them out).
class DelayedTaskQueue {
public:
    using Clock = std::chrono::steady_clock;

    // Returns true when the new task is now the earliest (the loop must
    // re-arm its timer / recompute its sleep).
    bool push(Clock::time_point deadline, std::function<void()> task);

    // The earliest deadline, if any task is pending.
    std::optional<Clock::time_point> nextDeadline() const;

    // Removes and returns every task due at `now`, earliest first.
    std::vector<std::function<void()>> takeDue(Clock::time_point now);

    std::size_t size() const;

    // Milliseconds from `now` until the earliest deadline, rounded up and
    // clamped to [0, maxMs]; nullopt when nothing is pending.
    std::optional<int> millisecondsUntilNext(Clock::time_point now, int maxMs) const;

private:
    struct Entry {
        Clock::time_point deadline;
        std::uint64_t sequence = 0;
        std::function<void()> task;
    };
    static bool later(const Entry& a, const Entry& b);

    mutable std::mutex mutex_;
    std::vector<Entry> heap_;
    std::uint64_t nextSequence_ = 0;
};

} // namespace rivet::core
