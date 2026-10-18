// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace rivet::net {

// Typed failure classes of the network layer. They are carried inside the
// shared core::Error (code + subsystem "net.<name>") so Result<T> stays
// uniform; failureOf() recovers the class for callers and tests.
enum class NetFailure : std::uint8_t {
    InvalidRequest,      // malformed URL / method / header / body
    PolicyViolation,     // scheme, allow-list, private host name
    ForbiddenAddress,    // SSRF guard: the destination address is not allowed
    OnlineValidationOff, // user setting: online validation disabled
    ConsentDenied,       // user (or default prompt) declined the connection
    RedirectLimit,       // too many redirects
    RedirectDowngrade,   // https -> http redirect
    BadRedirect,         // unusable Location header
    ResponseTooLarge,    // response exceeds the size cap
    Timeout,             // connect or total timeout
    Cancelled,           // caller cancelled
    BackendUnavailable,  // no transport compiled in (RIVET_ENABLE_NETWORK=OFF)
    Transport,           // DNS / TLS / socket / protocol failure
};

std::string_view toString(NetFailure failure);
core::Error makeNetError(NetFailure failure, std::string message);

// NetFailure::Transport for errors that did not originate in this layer.
NetFailure failureOf(const core::Error& error);

template <typename T>
inline core::Result<T> netFail(NetFailure failure, std::string message) {
    return std::unexpected(makeNetError(failure, std::move(message)));
}

// Cooperative cancellation shared between the caller and a running request.
// Copies share one flag; cancel() is thread-safe and idempotent.
class CancelToken {
public:
    CancelToken() : flag_(std::make_shared<std::atomic<bool>>(false)) {}
    void cancel() const { flag_->store(true, std::memory_order_release); }
    bool cancelled() const { return flag_->load(std::memory_order_acquire); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

} // namespace rivet::net
