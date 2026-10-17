// SPDX-License-Identifier: MPL-2.0
#include "app/BackgroundRunner.hpp"

#include <optional>
#include <utility>

namespace rivet::app {

BackgroundRunner::BackgroundRunner(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher)
    : scheduler_(scheduler), dispatcher_(dispatcher), alive_(std::make_shared<std::atomic<bool>>(true)),
      closing_(std::make_shared<std::atomic<bool>>(false)) {}

BackgroundRunner::~BackgroundRunner() {
    closing_->store(true, std::memory_order_release);
    // Undelivered completions are dropped; workers finish (or skip) their step.
    alive_->store(false, std::memory_order_release);
    scope_.closeAndWait();
}

bool BackgroundRunner::post(std::function<void()> work, std::function<void()> done) {
    std::optional<core::AsyncScope::Token> entered = scope_.enter();
    if (!entered.has_value()) return false;
    auto token = std::make_shared<core::AsyncScope::Token>(std::move(*entered));
    scheduler_.post([this, token = std::move(token), work = std::move(work), done = std::move(done)]() mutable {
        if (token->cancelled()) return;
        if (work) work();
        if (token->cancelled()) return;
        dispatcher_.post([alive = alive_, done = std::move(done)] {
            if (alive->load(std::memory_order_acquire) && done) done();
        });
    });
    return true;
}

} // namespace rivet::app
