// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <chrono>
#include <functional>

namespace rivet::core {

// Abstraction for marshaling work to the platform main/UI thread.
// Implemented by the platform layer (e.g. dispatch to the macOS main queue).
//
// Implementations must be safe to call from any thread and must keep the
// process alive until posted tasks have run (or drop them only at shutdown).
struct IMainThreadDispatcher {
    virtual ~IMainThreadDispatcher() = default;

    virtual void post(std::function<void()> task) = 0;

    // Runs `task` on the main thread no earlier than `delay` from now (caret
    // blink, tooltip delays). A main-thread timer: no thread is occupied
    // while waiting. Thread-safe like post(); tasks still pending at shutdown
    // are dropped unrun. Tasks cannot be cancelled: guard them with a
    // liveness token. Returns false when the dispatcher has no timers (the
    // default, e.g. test doubles); callers then skip the delayed work.
    //
    // Platform implementations: macOS dispatch_after on the main queue,
    // Windows a WM_TIMER on the dispatcher's message window, Linux the event
    // loop's poll() timeout (both over core::DelayedTaskQueue).
    virtual bool postDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
        (void)delay;
        (void)task;
        return false;
    }
};

} // namespace rivet::core
