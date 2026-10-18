// SPDX-License-Identifier: MPL-2.0
#pragma once

// Certificate path building for historical validation (ADR-0045). Unlike
// buildChain (Certificate.hpp, which only links issuers), this builder checks
// every link at a caller-supplied validation time: validity period,
// basicConstraints CA / pathLenConstraint, keyUsage keyCertSign and the
// signature algorithm and X.509 nameConstraints (NameConstraints.hpp), and it backtracks over alternative issuers. Candidate
// issuers come from the trust anchors, a caller-supplied pool and (optionally)
// an issuer fetcher (AIA caIssuers, see revocation/AiaFetcher.hpp). Everything
// that is not a trust anchor is untrusted input.

#include "crypto/Certificate.hpp"
#include "crypto/Providers.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace rivet::crypto {

enum class PathFailure : std::uint8_t {
    None,
    NoIssuer,          // no candidate issuer found / none verified
    NotAnchored,       // a complete path exists but its root is not a trust anchor
    NotValidAtTime,    // a certificate is expired / not yet valid at the validation time
    NotACa,            // basicConstraints says CA:FALSE (or is missing)
    KeyUsage,          // keyUsage present without keyCertSign
    PathLength,        // pathLenConstraint exceeded
    WeakAlgorithm,     // MD2 / MD5 certificate signature
    Loop,              // issuer already in the path
    TooLong,           // more than maxLength certificates
    BadSignature,      // a candidate with the right name did not verify
    NameConstraints,   // a nameConstraints violation, or a constraint that cannot be evaluated (fail-closed)
    BackendUnavailable,
};

std::string_view toString(PathFailure);

// Obtains DER candidates (a certificate, or a bundle) that may be the issuer of
// `cert`. Results are untrusted: the builder only uses a candidate that
// verifies as the issuer. Blocking; supplied by the caller.
using IssuerFetcher = std::function<std::vector<Bytes>(const CertificateInfo& cert)>;

struct PathOptions {
    std::chrono::system_clock::time_point validationTime = std::chrono::system_clock::now();
    const TrustAnchorSet* anchors = nullptr; // nullptr: nothing is trusted (the result is never anchored)
    std::vector<CertificateInfo> pool;       // intermediates / other candidates
    IssuerFetcher fetchIssuers;              // optional (AIA caIssuers)
    std::size_t maxLength = 10;              // certificates including leaf and anchor
    std::size_t maxFetches = 8;              // issuer fetches per path
    std::size_t maxSearchSteps = 4096;       // backtracking budget over a hostile pool (hard ceiling 65536)
    bool checkLeafValidity = true;
    CancelToken cancel;
};

struct PathResult {
    std::vector<CertificateInfo> chain; // leaf first; the anchor is the last element when anchored
    bool complete = false;              // the chain ends in a self-issued root
    bool anchored = false;              // ... and that root is a supplied trust anchor
    PathFailure failure = PathFailure::None; // why no anchored path was found (None when anchored)
    std::vector<std::string> reasons;        // rejected candidates and the verdict, bounded
    std::vector<std::string> limitations;    // checks that are not enforced (certificate policies)
    std::vector<std::string> weakNotes;      // SHA-1 signatures accepted for verification only
    std::vector<Bytes> fetched;              // issuer certificates obtained through the fetcher and used
};

// Builds and checks a path from `leaf` at options.validationTime. Never throws;
// "no path" is a result.
PathResult buildValidationPath(const CertificateInfo& leaf, const PathOptions& options, CryptoProvider& provider);

} // namespace rivet::crypto
