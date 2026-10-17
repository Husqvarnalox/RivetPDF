// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "net/Error.hpp"
#include "net/Http.hpp"
#include "net/Policy.hpp"

#include <memory>

namespace rivet::net {

// One HTTP hop. Implementations MUST NOT follow redirects (NetworkClient owns
// the redirect loop and re-validates every hop), MUST honour the effective
// limits (effectiveLimits(request, policy)): response-size cap (abort the
// transfer, ResponseTooLarge), connect/total timeouts (Timeout) and the
// CancelToken (Cancelled), and unless policy.allowPrivateNetworks MUST refuse
// to connect to a forbidden peer address, checked on the address actually
// used (ForbiddenAddress), which defeats DNS rebinding. Thread-safe.
class INetworkTransport {
public:
    virtual ~INetworkTransport() = default;
    virtual core::Result<HttpResponse> perform(const HttpRequest& request, const NetworkPolicy& policy,
                                               CancelToken cancel) = 0;
};

// The libcurl transport when built with RIVET_ENABLE_NETWORK=ON, otherwise a
// transport whose every call fails with NetFailure::BackendUnavailable.
std::shared_ptr<INetworkTransport> makeDefaultTransport();

// True when makeDefaultTransport() returns a real network transport.
bool networkBackendAvailable();

} // namespace rivet::net
