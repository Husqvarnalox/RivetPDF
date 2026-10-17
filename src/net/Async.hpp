// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "net/Client.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace rivet::net {

// Runs client->fetch() on the scheduler and delivers the result to `done` on
// the main thread through the dispatcher (same contract as
// crypto::runCryptoAsync).
//
//  - `done` is called exactly once on the main thread, never under a lock. A
//    cancelled request completes with NetFailure::Cancelled.
//  - The AsyncScope token fences the worker part; the owner calls
//    closeAndWait() before the scheduler / dispatcher die. When the scope is
//    already closed nothing is scheduled and false is returned (no callback).
//  - The client is shared so it outlives the worker regardless of the owner.
//  - Closing the scope cancels a request that has not started. A request
//    already in flight only observes its own CancelToken (and the timeouts),
//    so the owner cancels its tokens before closeAndWait() to avoid waiting
//    for the total timeout.
inline bool runNetworkAsync(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher,
                            core::AsyncScope& scope, std::shared_ptr<const NetworkClient> client, HttpRequest request,
                            CancelToken cancel, std::function<void(core::Result<HttpResponse>)> done) {
    std::optional<core::AsyncScope::Token> token = scope.enter();
    if (!token) return false;
    auto shared = std::make_shared<core::AsyncScope::Token>(std::move(*token));
    scheduler.post([shared, &dispatcher, client = std::move(client), request = std::move(request), cancel,
                    done = std::move(done)]() mutable {
        core::Result<HttpResponse> result = (cancel.cancelled() || shared->cancelled())
                                                ? netFail<HttpResponse>(NetFailure::Cancelled, "operation cancelled")
                                                : client->fetch(std::move(request), cancel);
        if (cancel.cancelled() && result) result = netFail<HttpResponse>(NetFailure::Cancelled, "operation cancelled");
        auto shipped = std::make_shared<core::Result<HttpResponse>>(std::move(result));
        dispatcher.post([shipped, done = std::move(done)]() mutable { done(std::move(*shipped)); });
    });
    return true;
}

} // namespace rivet::net
