// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "crypto/Error.hpp"
#include "crypto/Types.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace rivet::crypto {

// Runs `work` on the scheduler and delivers its result to `done` on the main
// thread through the dispatcher.
//
//  - `done` is called exactly once on the main thread, never under a lock.
//    A cancelled operation completes with CryptoErrorCode::Cancelled.
//  - The AsyncScope token fences the worker part: the owner calls
//    closeAndWait() before the scheduler / dispatcher die. When the scope is
//    already closed nothing is scheduled and false is returned (no callback).
//  - `work` receives the CancelToken and should poll it between steps; an
//    operation already inside a provider call (hardware PIN prompt) completes
//    that call and the result is then discarded as Cancelled.
//  - Secrets must be moved into `work`'s closure (SecretBuffer is move-only).
template <typename T>
bool runCryptoAsync(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher, core::AsyncScope& scope,
                    CancelToken cancel, std::function<CryptoResult<T>(const CancelToken&)> work,
                    std::function<void(CryptoResult<T>)> done) {
    std::optional<core::AsyncScope::Token> token = scope.enter();
    if (!token) return false;
    auto shared = std::make_shared<core::AsyncScope::Token>(std::move(*token));
    scheduler.post([shared, &dispatcher, cancel, work = std::move(work), done = std::move(done)]() mutable {
        CryptoResult<T> result = cancel.cancelled() || shared->cancelled()
                                     ? CryptoResult<T>(fail(CryptoErrorCode::Cancelled, "operation cancelled"))
                                     : work(cancel);
        if (cancel.cancelled() && result) result = fail(CryptoErrorCode::Cancelled, "operation cancelled");
        auto shipped = std::make_shared<CryptoResult<T>>(std::move(result));
        dispatcher.post([shipped, done = std::move(done)]() mutable { done(std::move(*shipped)); });
    });
    return true;
}

} // namespace rivet::crypto
