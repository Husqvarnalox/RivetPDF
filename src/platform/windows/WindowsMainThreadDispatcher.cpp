// SPDX-License-Identifier: MPL-2.0
#include "WindowsMainThreadDispatcher.hpp"

#include "WinCommon.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace rivet::platform {

namespace {

constexpr UINT kDrainMessage = WM_APP + 1;
constexpr UINT kArmTimerMessage = WM_APP + 2;
constexpr UINT_PTR kTimerId = 1;
constexpr wchar_t kClassName[] = L"RivetDispatcherWindow";

LRESULT CALLBACK dispatcherProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == kDrainMessage) {
        auto* dispatcher =
            reinterpret_cast<WindowsMainThreadDispatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (dispatcher != nullptr) dispatcher->drain();
        return 0;
    }
    if (message == kArmTimerMessage || (message == WM_TIMER && wParam == kTimerId)) {
        auto* dispatcher =
            reinterpret_cast<WindowsMainThreadDispatcher*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (dispatcher != nullptr) {
            if (message == WM_TIMER) {
                dispatcher->fireTimers();
            } else {
                dispatcher->armTimer();
            }
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

WindowsMainThreadDispatcher::WindowsMainThreadDispatcher() {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = dispatcherProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kClassName;
    ::RegisterClassExW(&windowClass); // a second registration fails harmlessly
    window_ = ::CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
    if (window_ != nullptr) ::SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
}

WindowsMainThreadDispatcher::~WindowsMainThreadDispatcher() {
    if (window_ != nullptr) {
        ::KillTimer(window_, kTimerId);
        ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
        ::DestroyWindow(window_);
    }
}

void WindowsMainThreadDispatcher::post(std::function<void()> task) {
    if (!task) return;
    bool wasEmpty = false;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wasEmpty = tasks_.empty();
        tasks_.push_back(std::move(task));
    }
    // drain() takes the whole queue, so one message per transition suffices.
    if (wasEmpty) wake();
}

bool WindowsMainThreadDispatcher::postDelayed(std::chrono::milliseconds delay, std::function<void()> task) {
    if (!task) return true;
    const auto deadline = core::DelayedTaskQueue::Clock::now() + std::max(delay, std::chrono::milliseconds(0));
    if (timers_.push(deadline, std::move(task)) && window_ != nullptr) {
        ::PostMessageW(window_, kArmTimerMessage, 0, 0); // SetTimer must run on the window's thread
    }
    return true;
}

void WindowsMainThreadDispatcher::armTimer() {
    if (window_ == nullptr) return;
    // USER_TIMER_MINIMUM (10 ms) is the floor; a due timer fires at once.
    const std::optional<int> wait =
        timers_.millisecondsUntilNext(core::DelayedTaskQueue::Clock::now(), 60 * 60 * 1000);
    if (!wait) {
        ::KillTimer(window_, kTimerId);
        return;
    }
    ::SetTimer(window_, kTimerId, static_cast<UINT>(std::max(*wait, static_cast<int>(USER_TIMER_MINIMUM))), nullptr);
}

void WindowsMainThreadDispatcher::fireTimers() {
    std::vector<std::function<void()>> due = timers_.takeDue(core::DelayedTaskQueue::Clock::now());
    if (!due.empty()) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (auto& task : due) tasks_.push_back(std::move(task));
        }
        drain(); // deferred while a modal scope is active; its end re-wakes
    }
    armTimer();
}

void WindowsMainThreadDispatcher::wake() {
    if (window_ != nullptr) ::PostMessageW(window_, kDrainMessage, 0, 0);
}

std::size_t WindowsMainThreadDispatcher::drain() {
    if (modalDepth_ > 0) return 0; // re-posted when the modal scope ends
    std::deque<std::function<void()>> batch;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(tasks_);
    }
    // Outside the lock: tasks may post (re-entrancy) or open modal dialogs.
    for (auto& task : batch) task();
    return batch.size();
}

WindowsMainThreadDispatcher::ModalScope::ModalScope(WindowsMainThreadDispatcher* dispatcher)
    : dispatcher_(dispatcher) {
    if (dispatcher_ != nullptr) ++dispatcher_->modalDepth_;
}

WindowsMainThreadDispatcher::ModalScope::~ModalScope() {
    if (dispatcher_ == nullptr) return;
    if (--dispatcher_->modalDepth_ > 0) return;
    bool pending = false;
    {
        const std::lock_guard<std::mutex> lock(dispatcher_->mutex_);
        pending = !dispatcher_->tasks_.empty();
    }
    // The wake-up for these tasks may have been consumed (and deferred) by
    // the modal loop.
    if (pending) dispatcher_->wake();
}

} // namespace rivet::platform
