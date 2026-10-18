// SPDX-License-Identifier: MPL-2.0
#pragma once

// OCSP (RFC 6960): request building, response parsing / verification and an
// HTTP client. Network access only through net::NetworkClient.

#include "crypto/Algorithms.hpp"
#include "crypto/Providers.hpp"
#include "crypto/revocation/Revocation.hpp"
#include "net/Client.hpp"

#include <memory>
#include <optional>
#include <string_view>

namespace rivet::crypto {

inline constexpr std::string_view kOcspRequestContentType = "application/ocsp-request";
inline constexpr std::string_view kOcspResponseContentType = "application/ocsp-response";

struct OcspRequestOptions {
    // SHA-256 by default. RFC 6960 interoperability still rests on SHA-1
    // CertIDs: OcspClient retries once with SHA-1 when a responder answers a
    // SHA-256 CertID with malformedRequest or unauthorized (OcspQueryOptions).
    // The SHA-1 here only identifies a certificate; it protects nothing.
    DigestAlgorithm certIdHash = DigestAlgorithm::Sha256;
    bool nonce = true; // RFC 6960 section 4.4.1 nonce extension, 16 random bytes
};

struct OcspRequest {
    Bytes der;
    DigestAlgorithm certIdHash = DigestAlgorithm::Sha256;
    std::optional<Bytes> nonce; // the random value (the extension wraps it in an OCTET STRING)
};

// CertID = hash(issuer name as in `cert`), hash(issuer public key bits), serial.
CryptoResult<OcspRequest> buildOcspRequest(CryptoProvider& provider, const CertificateInfo& cert,
                                           const CertificateInfo& issuer, const OcspRequestOptions& options = {});

// Parses and verifies a response for (`cert`, `issuer`). Never throws and
// never fails as a CryptoResult: a bad response is an OcspEvidence with
// verified == false, failure and reasons. Verification:
//  - responseStatus must be successful (tryLater / unauthorized / sigRequired / ... are reported as such);
//  - a SingleResponse must match the CertID (SHA-1/224-512 CertIDs accepted);
//  - signer: the issuing CA itself, or a certificate carried in the response
//    that the CA issued, that has id-kp-OCSPSigning, was valid at producedAt and
//    carries id-pkix-ocsp-nocheck (fail closed otherwise: its own revocation
//    status cannot be established from the response);
//  - the signature over ResponseData verifies under the signer's key;
//  - `expectedNonce`, when given, must be echoed;
//  - time rules of evidenceTimeProblem() under `clock`.
OcspEvidence verifyOcspResponse(ByteView responseDer, CryptoProvider& provider, const CertificateInfo& cert,
                                const CertificateInfo& issuer, const std::optional<Bytes>& expectedNonce,
                                const EvidenceClock& clock);

struct OcspQueryOptions {
    bool nonce = true;
    bool sha1Fallback = true;
    std::size_t maxResponseBytes = std::size_t{1} << 20;
    EvidenceClock clock;
};

class OcspClient {
public:
    OcspClient(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> provider);

    // Blocking: run through a scheduler, never on the UI thread. POSTs an
    // application/ocsp-request to `url`. Errors: InvalidArgument, BackendUnavailable,
    // Network (gate Off/declined, SSRF policy, transport, size cap, timeout),
    // BadResponse (HTTP status != 200, wrong Content-Type, empty body),
    // Cancelled. A reply that arrives but is rejected is a successful result
    // whose evidence has verified == false.
    CryptoResult<OcspEvidence> query(std::string_view url, const CertificateInfo& cert, const CertificateInfo& issuer,
                                     const OcspQueryOptions& options, const CancelToken& cancel) const;

private:
    CryptoResult<OcspEvidence> exchange(std::string_view url, const CertificateInfo& cert, const CertificateInfo& issuer,
                                        const OcspRequestOptions& request, const OcspQueryOptions& options,
                                        const CancelToken& cancel) const;

    std::shared_ptr<const net::NetworkClient> network_;
    std::shared_ptr<CryptoProvider> provider_;
};

} // namespace rivet::crypto
