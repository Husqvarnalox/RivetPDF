// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crypto/Algorithms.hpp"
#include "crypto/Certificate.hpp"
#include "crypto/Cms.hpp"
#include "crypto/Error.hpp"
#include "crypto/Providers.hpp"
#include "crypto/Types.hpp"
#include "crypto/Validation.hpp"
#include "crypto/timestamp/TimestampInfo.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// RFC 3161 time-stamping, backend-free except for hashing, signature
// verification and the RNG, which go through CryptoProvider. This header and
// its sources contain no networking: fetching a token over HTTP is
// TimeStampClient (rivet_timestamp), which goes through rivet::net.
namespace rivet::crypto {

// ---- request (RFC 3161 section 2.4.1) ----------------------------------------

struct TimeStampRequestOptions {
    bool includeNonce = true;              // random 64-bit nonce from the provider RNG
    bool certReq = true;                   // ask the TSA to embed its certificate chain
    std::optional<std::string> policyOid;  // reqPolicy
};

struct TimeStampRequest {
    Bytes der;                             // the encoded TimeStampReq
    DigestAlgorithm algorithm = DigestAlgorithm::Sha256;
    Bytes digest;                          // messageImprint.hashedMessage
    std::optional<Bytes> nonce;            // normalized INTEGER content (matches TstInfo::nonce)
    std::optional<std::string> policyOid;
    bool certReq = true;
};

// SHA-1 (legacy) is refused for creation (WeakAlgorithm); `digest` must have
// the length of `algorithm`.
CryptoResult<TimeStampRequest> buildTimeStampRequest(CryptoProvider& provider, DigestAlgorithm algorithm,
                                                     ByteView digest, const TimeStampRequestOptions& options = {});

// ---- response / token parsing (RFC 3161 section 2.4.2) ---------------------------

enum class PkiStatus : std::uint8_t {
    Granted = 0,
    GrantedWithMods = 1,
    Rejection = 2,
    Waiting = 3,
    RevocationWarning = 4,
    RevocationNotification = 5,
    Unknown = 255,
};

// PKIFailureInfo bit n of the BIT STRING is 1u << n.
namespace pkifailure {
inline constexpr std::uint32_t kBadAlg = 1u << 0;
inline constexpr std::uint32_t kBadRequest = 1u << 2;
inline constexpr std::uint32_t kBadDataFormat = 1u << 5;
inline constexpr std::uint32_t kTimeNotAvailable = 1u << 14;
inline constexpr std::uint32_t kUnacceptedPolicy = 1u << 15;
inline constexpr std::uint32_t kUnacceptedExtension = 1u << 16;
inline constexpr std::uint32_t kAddInfoNotAvailable = 1u << 17;
inline constexpr std::uint32_t kSystemFailure = 1u << 25;
} // namespace pkifailure

// Human-readable names of the set failInfo bits ("badAlg", ...).
std::vector<std::string> failInfoNames(std::uint32_t failInfo);

struct PkiStatusInfo {
    int statusValue = -1;
    PkiStatus status = PkiStatus::Unknown;
    std::vector<std::string> text; // statusString entries, sanitized (printable ASCII, bounded)
    std::uint32_t failInfo = 0;
    bool hasFailInfo = false;

    bool accepted() const { return status == PkiStatus::Granted || status == PkiStatus::GrantedWithMods; }
};

struct TimeStampResponse {
    PkiStatusInfo status;
    std::optional<Bytes> tokenDer; // the ContentInfo (CMS SignedData), exactly as received
};

CryptoResult<TimeStampResponse> parseTimeStampResponse(ByteView der);

struct TstAccuracy {
    std::optional<std::int64_t> seconds;
    std::optional<std::int64_t> millis;
    std::optional<std::int64_t> micros;
};

struct TstInfo {
    int version = 1;
    std::string policy;
    std::string imprintAlgorithmOid;
    std::optional<DigestAlgorithm> imprintAlgorithm; // nullopt: unknown OID
    Bytes imprintDigest;
    Bytes serial;                  // normalized INTEGER content
    std::string serialHex;
    std::chrono::system_clock::time_point genTime{}; // fractional seconds kept down to the clock resolution
    std::optional<TstAccuracy> accuracy;
    bool ordering = false;
    std::optional<Bytes> nonce;    // normalized INTEGER content
    std::optional<Bytes> tsaDirectoryNameDer; // the tsa field when it is a directoryName
};

CryptoResult<TstInfo> parseTstInfo(ByteView der);

// ---- verification ----------------------------------------------------------------

// What the caller expects the token to say.
struct TimeStampExpectation {
    DigestAlgorithm algorithm = DigestAlgorithm::Sha256;
    Bytes digest;
    std::optional<Bytes> nonce;       // when we sent one, the token must echo it
    std::optional<std::string> policy; // when we requested one, the token must carry it
};

struct TimeStampVerifyOptions {
    // The TSA chain must reach one of these anchors. nullptr: nothing is
    // trusted (the result is UntrustedTsa); the OS store is never consulted.
    const TrustAnchorSet* anchors = nullptr;
    std::vector<Bytes> extraCertificates; // additional chain material (tokens without certReq)
    CancelToken cancel;
};

struct TimeStampVerification {
    TimestampFailure failure = TimestampFailure::None;
    std::optional<TstInfo> info;                    // set whenever the TSTInfo could be parsed
    std::optional<CertificateInfo> tsaCertificate;  // the signer certificate when found
    ValidationResult validation;                    // crypto / certificate / trust / timestamp + notes
    Bytes tokenDer;                                 // the raw token, to embed unchanged

    bool ok() const { return failure == TimestampFailure::None; }
};

// Errors are returned only for conditions that prevent verification altogether
// (no backend, cancellation); a bad token is a result whose `failure` says why.
CryptoResult<TimeStampVerification> verifyTimeStampToken(ByteView tokenDer, CryptoProvider& provider,
                                                         const TimeStampExpectation& expected,
                                                         const TimeStampVerifyOptions& options = {});

// Status check (BadStatus carries the status text and failInfo names in
// validation.notes), then verifyTimeStampToken against the request.
CryptoResult<TimeStampVerification> verifyTimeStampResponse(ByteView responseDer, CryptoProvider& provider,
                                                            const TimeStampRequest& request,
                                                            const TimeStampVerifyOptions& options = {});

// ---- CMS signature time-stamp (RFC 5126 section 6.1.1, CAdES-T / PAdES B-T) ---------

// Digest of the signerInfo signature value octets: the messageImprint to send.
CryptoResult<Bytes> signatureTimestampImprint(CryptoProvider& provider, const CmsSignerInfo& signer,
                                              DigestAlgorithm algorithm);

// Adds the token as the unsigned attribute id-aa-signatureTimeStampToken of
// signer `signerIndex`. Only the unsigned attributes change: the signed
// attributes and the signature value stay byte-identical. The token must be a
// parsable TimeStampToken whose imprint matches this signer's signature value.
CryptoOk attachSignatureTimestamp(CmsSignedData& model, std::size_t signerIndex, ByteView tokenDer,
                                  CryptoProvider& provider);
// Same on encoded CMS (parse, attach, re-encode).
CryptoResult<Bytes> attachSignatureTimestampDer(ByteView cmsDer, std::size_t signerIndex, ByteView tokenDer,
                                                CryptoProvider& provider);

// Verifies the embedded signature time-stamp of `signer` (which must carry one).
CryptoResult<SignatureTimestampInfo> verifySignatureTimestamp(const CmsSignerInfo& signer, CryptoProvider& provider,
                                                              const TrustAnchorSet* anchors,
                                                              const std::vector<Bytes>& extraCertificates,
                                                              const CancelToken& cancel);

} // namespace rivet::crypto
