// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/DelayedTaskQueue.hpp"
#include "core/async/IMainThreadDispatcher.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>

struct HWND__;

namespace rivet::platform {

// Main-thread dispatcher for the Win32 shell: a mutex-guarded FIFO plus a
// message-only window that receives one PostMessage(WM_APP) per
// empty -> non-empty transition. post() is thread-safe and never runs the
// task inline; the window procedure drains the queue on the main thread and
// runs every task OUTSIDE the lock (tasks may post more tasks; those run on
// the next drain). Tasks still queued when the dispatcher is destroyed are
// dropped unrun, like blocks left on the macOS main queue at exit().
//
// Modality: native modal loops (TaskDialog, the common file dialogs,
// PrintDlgEx) keep dispatching messages. Like -[NSAlert runModal] (and the
// X11 shell's modal pump), queued tasks must NOT run while such a dialog is
// up: the platform services hold a ModalScope around every modal call, and
// the drain is deferred until the outermost scope ends.
//
// Construct and destroy on the main thread (it owns an HWND).
class WindowsMainThreadDispatcher final : public core::IMainThreadDispatcher {
public:
    WindowsMainThreadDispatcher();
    ~WindowsMainThreadDispatcher() override;
    WindowsMainThreadDispatcher(const WindowsMainThreadDispatcher&) = delete;
    WindowsMainThreadDispatcher& operator=(const WindowsMainThreadDispatcher&) = delete;

    void post(std::function<void()> task) override;
    // Timers live in a DelayedTaskQueue; the message window keeps one
    // WM_TIMER armed for the earliest deadline (SetTimer is main-thread only,
    // so other threads ask the window to re-arm it). Due tasks join the FIFO
    // and run through drain() (deferred while modal, like posted tasks).
    bool postDelayed(std::chrono::milliseconds delay, std::function<void()> task) override;

    // Main thread (window procedure): moves due timers into the FIFO, drains
    // and re-arms the WM_TIMER for the next deadline.
    void fireTimers();
    void armTimer();

    // Main thread: runs every task queued so far (unless a modal scope is
    // active). Returns how many ran.
    std::size_t drain();

    class ModalScope {
    public:
        explicit ModalScope(WindowsMainThreadDispatcher* dispatcher);
        ~ModalScope();
        ModalScope(const ModalScope&) = delete;
        ModalScope& operator=(const ModalScope&) = delete;

    private:
        WindowsMainThreadDispatcher* dispatcher_;
    };

private:
    void wake();

    HWND__* window_ = nullptr;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    core::DelayedTaskQueue timers_;
    int modalDepth_ = 0; // main thread only
};

} // namespace rivet::platform
