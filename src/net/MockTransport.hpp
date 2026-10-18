// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "net/Transport.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rivet::net {

// A scripted reply for one URL.
struct MockReply {
    int status = 200;
    HttpHeaders headers;
    std::string body;
    // Address the "connection" is made to. Lets tests exercise the connect-time
    // SSRF guard (DNS rebinding: a public name resolving to a private address)
    // without sockets. Unset: the URL's IP literal, or a public address.
    std::optional<IpAddress> connectAddress;
    // Simulated latency. Longer than the effective total timeout -> Timeout
    // (reported immediately, no real wait); otherwise really slept in small
    // slices that poll the CancelToken.
    std::chrono::milliseconds latency{0};
    // Never answers: blocks (polling the CancelToken) until cancelled or the
    // effective total timeout elapses in real time.
    bool hang = false;
};

// In-memory transport for tests (also used by the TSA / OCSP / CRL tests).
// One hop per call, like every INetworkTransport. Thread-safe.
class MockTransport final : public INetworkTransport {
public:
    // `url` is canonicalised with parseUrl; an unparsable key is kept verbatim.
    void script(const std::string& url, MockReply reply);
    void scriptRedirect(const std::string& url, const std::string& location, int status = 302);

    core::Result<HttpResponse> perform(const HttpRequest& request, const NetworkPolicy& policy,
                                       CancelToken cancel) override;

    std::vector<HttpRequest> requests() const;
    std::size_t callCount() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, MockReply> script_;
    std::vector<HttpRequest> requests_;
};

} // namespace rivet::net
