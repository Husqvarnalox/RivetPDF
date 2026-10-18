// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"
#include "net/Consent.hpp"
#include "net/Http.hpp"
#include "net/Policy.hpp"
#include "net/Transport.hpp"

#include <memory>

namespace rivet::net {

// The only way the application reaches the network. Owns the redirect loop
// and enforces, for every hop: online-validation gate, scheme / allow-list /
// private-host / IP-literal policy, https->http downgrade refusal, remaining
// total-time budget, response-size cap and cancellation. The transport adds
// connect-time peer-address checks. Thread-safe; fetch() blocks (run it via
// runNetworkAsync, never on the UI thread).
class NetworkClient {
public:
    // A null transport selects makeDefaultTransport(); a null gate selects
    // OnlineValidationMode::Ask with a deny-all prompt (nothing is contacted
    // until a UI installs a prompt or the user switches validation On).
    NetworkClient(NetworkPolicy policy, std::shared_ptr<INetworkTransport> transport = nullptr,
                  std::shared_ptr<OnlineValidationGate> gate = nullptr);

    core::Result<HttpResponse> fetch(HttpRequest request, CancelToken cancel = {}) const;

    const NetworkPolicy& policy() const { return policy_; }
    OnlineValidationGate& gate() const { return *gate_; }

private:
    NetworkPolicy policy_;
    std::shared_ptr<INetworkTransport> transport_;
    std::shared_ptr<OnlineValidationGate> gate_;
};

} // namespace rivet::net
