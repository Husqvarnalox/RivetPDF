// SPDX-License-Identifier: MPL-2.0
#include "net/Transport.hpp"

#ifdef RIVET_NET_CURL
#include "net/curl/CurlTransport.hpp"
#endif

namespace rivet::net {

namespace {

class UnavailableTransport final : public INetworkTransport {
public:
    core::Result<HttpResponse> perform(const HttpRequest&, const NetworkPolicy&, CancelToken) override {
        return netFail<HttpResponse>(NetFailure::BackendUnavailable,
                                     "this build has no network backend (RIVET_ENABLE_NETWORK=OFF)");
    }
};

} // namespace

std::shared_ptr<INetworkTransport> makeDefaultTransport() {
#ifdef RIVET_NET_CURL
    return std::make_shared<CurlTransport>();
#else
    return std::make_shared<UnavailableTransport>();
#endif
}

bool networkBackendAvailable() {
#ifdef RIVET_NET_CURL
    return true;
#else
    return false;
#endif
}

} // namespace rivet::net
