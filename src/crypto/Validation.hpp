// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crypto/Certificate.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto {

// Signature validation is never a bool. The dimensions below are reported
// independently and then folded into one user-facing SignatureState.

enum class CryptoCheck : std::uint8_t { Valid, Invalid, Unsupported, NotChecked };
enum class IntegrityStatus : std::uint8_t { Intact, Modified, Unknown };
// For standalone CMS there is no incremental-update concept: NotApplicable.
enum class ModificationStatus : std::uint8_t { NotApplicable, None, Permitted, Disallowed, Unknown };
enum class TimestampStatus : std::uint8_t { None, PresentUnverified, Valid, Invalid, Untrusted };
enum class RevocationStatus : std::uint8_t { NotChecked, Good, Revoked, Unknown };
enum class PolicyStatus : std::uint8_t { Compliant, WeakLegacyAlgorithm, NonCompliant };

enum class SignatureState : std::uint8_t {
    ValidTrusted,
    ValidUntrusted,
    ValidExpiredNow,         // signature valid, trusted chain, certificate expired at validation time
    ValidAtTrustedTimestamp, // valid at a trusted timestamp even though expired now
    ValidTimestamped,        // valid and trusted, with a verified signature time-stamp
    ValidTimestampInvalid,   // valid and trusted, but the signature time-stamp present is not valid / not trusted
    Revoked,
    DocumentModified,
    PermittedModifications,
    InvalidSignature,
    UnableToValidate,
    UnsupportedAlgorithm,
    WeakLegacyAlgorithm,
};

std::string_view toString(CryptoCheck);
std::string_view toString(IntegrityStatus);
std::string_view toString(ModificationStatus);
std::string_view toString(TimestampStatus);
std::string_view toString(RevocationStatus);
std::string_view toString(PolicyStatus);
std::string_view toString(CertificateStatus);
std::string_view toString(TrustStatus);
std::string_view toString(SignatureState);

// Optional detail of an online/offline revocation check (see
// crypto/revocation/RevocationChecker.hpp, applyRevocation). Absent unless a
// revocation policy was applied.
struct RevocationDetail {
    RevocationStatus status = RevocationStatus::NotChecked;
    bool hardFail = false;
    std::size_t certificatesChecked = 0;
    std::size_t certificatesUnknown = 0;
    std::optional<std::chrono::system_clock::time_point> revokedAt;
    std::optional<int> reasonCode; // RFC 5280 CRLReason
    std::string summary;
};

struct ValidationResult {
    SignatureState state = SignatureState::UnableToValidate;
    CryptoCheck crypto = CryptoCheck::NotChecked;
    CertificateStatus certificate = CertificateStatus::Unknown;
    TrustStatus trust = TrustStatus::Unknown;
    IntegrityStatus integrity = IntegrityStatus::Unknown;
    ModificationStatus modification = ModificationStatus::NotApplicable;
    TimestampStatus timestamp = TimestampStatus::None;
    RevocationStatus revocation = RevocationStatus::NotChecked;
    PolicyStatus policy = PolicyStatus::Compliant;
    std::vector<std::string> notes; // short human-readable reasons, no secrets
    std::optional<RevocationDetail> revocationDetail;
};

// Folds the independent statuses into the user-facing state.
// Precedence: Revoked > DocumentModified > InvalidSignature >
// UnsupportedAlgorithm > UnableToValidate > WeakLegacyAlgorithm >
// ValidAtTrustedTimestamp / ValidExpiredNow > ValidTimestamped /
// ValidTimestampInvalid (trusted signer only) > ValidTrusted / ValidUntrusted.
SignatureState foldState(const ValidationResult& r);

} // namespace rivet::crypto
