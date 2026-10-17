// SPDX-License-Identifier: MPL-2.0
#pragma once

// AIA caIssuers fetching for the certificate path builder (ADR-0045). The
// only network access is rivet::net::NetworkClient (online-validation gate and
// SSRF policy apply to every hop). A response is untrusted: it is accepted
// only as a DER or PEM certificate, or a PKCS#7 certs-only bundle, within a
// size cap, and the path builder still requires that it verifies as the issuer.

#include "crypto/PathBuilder.hpp"
#include "net/Client.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rivet::crypto {

struct AiaFetchOptions {
    std::size_t maxResponseBytes = std::size_t{256} << 10;
    std::size_t maxUrlsPerCertificate = 3;
    CancelToken cancel;
};

// Filled while the fetcher runs (single-threaded use).
struct AiaFetchLog {
    std::size_t requests = 0;
    std::size_t certificatesAccepted = 0;
    std::vector<std::string> notes; // refused URLs, gate refusals, bad payloads (bounded)
};

// The returned callable keeps `network` and `provider` alive and blocks while fetching.
IssuerFetcher makeAiaFetcher(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> provider,
                             AiaFetchOptions options = {}, std::shared_ptr<AiaFetchLog> log = nullptr);

// Certificates in an AIA response body: one DER/PEM certificate or a PKCS#7
// bundle. Exposed for tests.
std::vector<Bytes> certificatesFromAiaBody(ByteView body, CryptoProvider& provider);

} // namespace rivet::crypto
