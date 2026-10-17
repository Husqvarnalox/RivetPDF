// SPDX-License-Identifier: MPL-2.0
#pragma once

// Revocation evidence model shared by the OCSP (RFC 6960) and CRL (RFC 5280)
// code. Parsing and verification here are backend-free except for hashing and
// signature verification, which go through CryptoProvider; only the HTTP
// clients (OcspClient / CrlClient / RevocationChecker) use rivet::net, and
// they reach the network exclusively through net::NetworkClient.

#include "crypto/Certificate.hpp"
#include "crypto/Error.hpp"
#include "crypto/Types.hpp"
#include "crypto/Validation.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto {

using TimePoint = std::chrono::system_clock::time_point;

// ---- certificate extensions ----------------------------------------------------

struct DistributionPointInfo {
    std::vector<std::string> uris;   // uniformResourceIdentifier names of fullName (any scheme)
    std::vector<Bytes> generalNames; // DER of every GeneralName of fullName, for CRL scope matching
    bool hasReasons = false;         // the DP covers only some reason codes (partitioned CRLs)
    bool hasCrlIssuer = false;       // indirect CRL
    bool hasRelativeName = false;    // nameRelativeToCRLIssuer (not supported)
};

// The revocation-related extensions of one certificate. URLs are UNTRUSTED
// input: classifyRevocationUrl() decides which are even attempted, and the
// network layer applies its SSRF policy on top.
struct RevocationExtensions {
    std::vector<std::string> ocspUrls;        // AIA accessMethod id-ad-ocsp
    std::vector<std::string> caIssuerUrls;    // AIA accessMethod id-ad-caIssuers
    std::vector<DistributionPointInfo> crlDistributionPoints;
    std::vector<std::string> freshestCrlUrls; // delta CRL pointer: recorded, delta CRLs are not processed
    bool ocspNoCheck = false;                 // id-pkix-ocsp-nocheck present
    bool freshestCrlPresent = false;
};

// Reads AIA, CRL Distribution Points, Freshest CRL and OCSP no-check from a
// certificate. Malformed revocation extensions are an error (CorruptData);
// absent ones are simply empty.
CryptoResult<RevocationExtensions> parseRevocationExtensions(ByteView certDer);

// nullopt when `url` may be attempted (http / https, bounded length, no
// control characters); otherwise the reason it is skipped (ldap://, file://,
// ...). The network layer still applies its own scheme / SSRF policy.
std::optional<std::string> classifyRevocationUrl(std::string_view url);

// ---- results ----------------------------------------------------------------------

enum class EvidenceFailure : std::uint8_t {
    None,
    Malformed,
    ResponderRejected,      // OCSP responseStatus is not successful
    CertIdMismatch,         // the response does not cover the certificate asked about
    NonceMismatch,          // nonce sent but missing / different in the response
    SignatureInvalid,
    UnsupportedAlgorithm,
    UnauthorizedResponder,  // signer is neither the issuing CA nor a delegated responder it issued
    ResponderNotIssuedByCa, // delegated responder certificate not issued by the CA
    ResponderNoOcspSigning, // delegated responder lacks id-kp-OCSPSigning
    ResponderCertInvalid,   // delegated responder certificate not valid at producedAt
    NotYetValid,            // thisUpdate / producedAt in the future
    Stale,                  // nextUpdate passed (or too old without nextUpdate)
    DoesNotCoverTime,       // historical validation: evidence neither covers nor postdates the validation time
    IssuerMismatch,         // CRL issuer differs from the certificate's issuer
    IssuerNotCrlSigner,     // CRL issuer certificate may not sign CRLs
    NotApplicable,          // CRL scope (issuingDistributionPoint) does not cover the certificate
    UnsupportedFeature,     // critical extension, delta / indirect CRL, ...
    BackendUnavailable,
};

std::string_view toString(EvidenceFailure);

enum class OcspResponseStatus : std::uint8_t {
    Successful = 0,
    MalformedRequest = 1,
    InternalError = 2,
    TryLater = 3,
    SigRequired = 5,
    Unauthorized = 6,
    Unknown = 255, // not an OCSPResponse / unrecognised value
};

std::string_view toString(OcspResponseStatus);

// What the evidence says about the certificate; kept apart from the
// verdict so that "verified == false" evidence can never be mistaken for it.
enum class EvidenceCertStatus : std::uint8_t { Good, Revoked, Unknown };

struct OcspEvidence {
    Bytes rawResponseDer;              // exactly as received (goes into the PDF /DSS)
    OcspResponseStatus responseStatus = OcspResponseStatus::Unknown;
    EvidenceCertStatus status = EvidenceCertStatus::Unknown;
    TimePoint thisUpdate{};
    std::optional<TimePoint> nextUpdate;
    TimePoint producedAt{};
    std::string responderName;         // subject of the matched signer certificate, or "key:<hex>" / "name:<hex>"
    std::optional<TimePoint> revocationTime;
    std::optional<int> revocationReason; // CRLReason value
    std::string url;
    bool nonceSent = false;
    bool nonceChecked = false;
    bool certIdSha1Fallback = false;   // answered after retrying with a SHA-1 CertID
    bool verified = false;             // signature, responder authorization, CertID, nonce and freshness all passed
    EvidenceFailure failure = EvidenceFailure::None;
    std::vector<std::string> reasons;  // why not verified, plus informational notes
};

struct CrlEvidence {
    Bytes rawCrlDer;                   // DER (a PEM CRL is decoded)
    std::string url;
    Bytes issuerDer;
    TimePoint thisUpdate{};
    std::optional<TimePoint> nextUpdate;
    Bytes crlNumber;                   // normalized INTEGER content, empty when absent
    EvidenceCertStatus status = EvidenceCertStatus::Unknown;
    std::optional<TimePoint> revocationTime;
    std::optional<int> revocationReason;
    bool onHold = false;               // certificateHold: revoked, possibly temporarily
    bool applicable = true;            // false: valid CRL, but its scope does not cover the certificate
    bool fromCache = false;            // served from the per-session CrlCache
    bool verified = false;
    EvidenceFailure failure = EvidenceFailure::None;
    std::vector<std::string> reasons;
    std::vector<std::string> limitations; // e.g. "delta CRL not processed"
};

// ---- time rules ---------------------------------------------------------------------

// Clock for judging evidence. `validationTime` is the instant the status is
// needed for: "now" for live validation, the signing time for historical.
struct EvidenceClock {
    TimePoint now = std::chrono::system_clock::now();
    TimePoint validationTime = now;
    std::chrono::milliseconds tolerance = std::chrono::minutes(5);
    std::chrono::milliseconds maxAge = std::chrono::hours(24); // used when nextUpdate is absent

    bool historical() const { return validationTime + tolerance < now; }
};

// nullopt when the evidence may be used. Rules:
//  - thisUpdate / producedAt later than now + tolerance: NotYetValid (always).
//  - live validation: nextUpdate >= now (or thisUpdate + maxAge >= now when
//    nextUpdate is absent), else Stale.
//  - historical validation: staleness at "now" is irrelevant, but the
//    evidence must speak for the validation time: either its thisUpdate is
//    at or after it (a later "good" proves the certificate was not revoked
//    earlier, a later revocation time after it proves the same), or
//    thisUpdate <= validationTime <= nextUpdate. Otherwise DoesNotCoverTime.
std::optional<EvidenceFailure> evidenceTimeProblem(TimePoint thisUpdate, std::optional<TimePoint> nextUpdate,
                                                   TimePoint producedAt, const EvidenceClock& clock,
                                                   std::string* reason = nullptr);

} // namespace rivet::crypto
