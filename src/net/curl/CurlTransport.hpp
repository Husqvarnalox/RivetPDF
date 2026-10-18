// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/Transport.hpp"

namespace rivet::net {

// libcurl-backed single-hop transport (RIVET_ENABLE_NETWORK=ON). Settings:
// http/https only (also for redirects, which are disabled anyway), TLS >= 1.2
// with peer and host verification always on, no cookies / netrc / referer,
// proxy environment ignored unless policy.useSystemProxy, response-size cap
// enforced in the write callback, connect/total timeouts, cancellation and
// deadline polled in the progress callback, and a connect-time peer-address
// check (OPENSOCKETFUNCTION) against isForbiddenAddress unless the policy
// allows private networks. One easy handle per call; safe for concurrent use.
class CurlTransport final : public INetworkTransport {
public:
    CurlTransport();
    core::Result<HttpResponse> perform(const HttpRequest& request, const NetworkPolicy& policy,
                                       CancelToken cancel) override;
};

} // namespace rivet::net
