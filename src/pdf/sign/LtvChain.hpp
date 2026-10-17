// SPDX-License-Identifier: MPL-2.0
#pragma once

// One certificate chain's path and revocation evidence, shared by the
// verifier (offline, embedded evidence only) and the collector (network
// through rivet::net) so both apply the same rules (ADR-0045). Internal.

#include "crypto/PathBuilder.hpp"
#include "crypto/revocation/RevocationChecker.hpp"
#include "pdf/sign/PdfLtvTypes.hpp"

#include <string>
#include <vector>

namespace rivet::pdf::sign::ltv {

struct ChainInput {
    std::string role;                      // "signer" / "time-stamp authority" (used in messages)
    crypto::CertificateInfo leaf;
    const crypto::TrustAnchorSet* anchors = nullptr;
    std::vector<crypto::CertificateInfo> pool; // CMS / DSS / caller intermediates
    crypto::IssuerFetcher fetchIssuers;        // optional AIA
    crypto::TimePoint validationTime;          // the instant the status is needed for
    crypto::TimePoint now;                     // clock for judging evidence
};

struct ChainEvidence {
    crypto::PathResult path;
    crypto::RevocationOutcome revocation;
    // Evidence used: the accepted OCSP / CRL of every certificate, raw DER.
    std::vector<crypto::Bytes> ocsps, crls;
    std::vector<std::string> missing; // one message per certificate or path problem, bounded
    bool complete = false;            // anchored path and verified good evidence for every non-anchor certificate
};

// `policy.store` / `policy.allowNetwork` / `network` decide where evidence comes from. Mode Off is refused (complete == false).
ChainEvidence checkChain(crypto::CryptoProvider& provider, const ChainInput& input,
                         const std::shared_ptr<const net::NetworkClient>& network, crypto::RevocationPolicy policy,
                         const crypto::CancelToken& cancel);

// Non-owning shared_ptr for APIs that take shared_ptr<CryptoProvider>; the provider must outlive every use.
std::shared_ptr<crypto::CryptoProvider> borrowProvider(crypto::CryptoProvider& provider);

// Trims the zero padding after the CMS / token DER of a hex /Contents value; false when non-zero bytes follow it.
bool trimSignatureContents(crypto::Bytes& der);

// Parses DER certificates, dropping unparsable ones.
std::vector<crypto::CertificateInfo> parseCertificates(crypto::CryptoProvider& provider,
                                                       const std::vector<crypto::Bytes>& der);

} // namespace rivet::pdf::sign::ltv
