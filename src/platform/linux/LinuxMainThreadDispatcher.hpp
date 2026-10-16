// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/IMainThreadDispatcher.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

namespace rivet::platform {

// Main-thread dispatcher for the X11 shell: a mutex-guarded FIFO plus a
// self-pipe whose read end the event loop polls together with the X
// connection. post() is thread-safe and never runs the task inline; drain()
// runs on the main thread and executes every queued task OUTSIDE the lock
// (tasks may post more tasks; those run on the next drain). Tasks still
// queued when the dispatcher is destroyed are dropped unrun, like blocks left
// on the macOS main queue at exit().
//
// The same pipe also wakes the loop for redraw requests and signals
// (wake()), so the loop has exactly one "something happened" fd.
class LinuxMainThreadDispatcher final : public core::IMainThreadDispatcher {
public:
    LinuxMainThreadDispatcher();
    ~LinuxMainThreadDispatcher() override;
    LinuxMainThreadDispatcher(const LinuxMainThreadDispatcher&) = delete;
    LinuxMainThreadDispatcher& operator=(const LinuxMainThreadDispatcher&) = delete;

    void post(std::function<void()> task) override;

    // Thread-safe and async-signal-safe: makes the loop's poll() return.
    void wake() const;

    // Fd the event loop polls for readability.
    int wakeFd() const { return readFd_; }

    // Main thread: empties the wake pipe (call when wakeFd() is readable).
    void consumeWakeups() const;

    // Main thread: runs every task queued so far. Returns how many ran.
    std::size_t drain();

    // Thread-safe: true while tasks are queued (a modal session may have
    // consumed their wakeup without running them).
    bool hasPending();

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    int readFd_ = -1;
    int writeFd_ = -1;
};

} // namespace rivet::platform
