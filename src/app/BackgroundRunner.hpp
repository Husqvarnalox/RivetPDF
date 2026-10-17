// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"

#include <atomic>
#include <functional>
#include <memory>

namespace rivet::app {

// One place for the "worker + main-thread completion" lifetime rules the
// security controllers (signing, validation, Set Password) share:
//
//   - `work` runs on the scheduler holding a core::AsyncScope token, so the
//     destructor (closeAndWait) cannot return while a worker is inside it;
//   - `done` is delivered through the main-thread dispatcher and is dropped
//     when the runner is already gone (alive flag), never run under a lock;
//   - a work item whose scope closed before it started is skipped.
//
// Main thread only (post / destructor).
class BackgroundRunner {
public:
    BackgroundRunner(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher);
    ~BackgroundRunner();

    BackgroundRunner(const BackgroundRunner&) = delete;
    BackgroundRunner& operator=(const BackgroundRunner&) = delete;

    // False (nothing runs) while the runner is being destroyed.
    bool post(std::function<void()> work, std::function<void()> done);

    // True once destruction has begun (workers may poll it as a cancel hint).
    bool closing() const { return closing_->load(std::memory_order_acquire); }

private:
    core::TaskScheduler& scheduler_;
    core::IMainThreadDispatcher& dispatcher_;
    std::shared_ptr<std::atomic<bool>> alive_;
    std::shared_ptr<std::atomic<bool>> closing_;
    // Declared last: destroyed first, after the destructor closed it.
    core::AsyncScope scope_;
};

} // namespace rivet::app
