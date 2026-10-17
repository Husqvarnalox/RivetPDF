// SPDX-License-Identifier: MPL-2.0
#include "crypto/Validation.hpp"

namespace rivet::crypto {

std::string_view toString(CryptoCheck v) {
    switch (v) {
    case CryptoCheck::Valid: return "valid";
    case CryptoCheck::Invalid: return "invalid";
    case CryptoCheck::Unsupported: return "unsupported";
    case CryptoCheck::NotChecked: return "not checked";
    }
    return "?";
}

std::string_view toString(IntegrityStatus v) {
    switch (v) {
    case IntegrityStatus::Intact: return "intact";
    case IntegrityStatus::Modified: return "modified";
    case IntegrityStatus::Unknown: return "unknown";
    }
    return "?";
}

std::string_view toString(ModificationStatus v) {
    switch (v) {
    case ModificationStatus::NotApplicable: return "not applicable";
    case ModificationStatus::None: return "none";
    case ModificationStatus::Permitted: return "permitted";
    case ModificationStatus::Disallowed: return "disallowed";
    case ModificationStatus::Unknown: return "unknown";
    }
    return "?";
}

std::string_view toString(TimestampStatus v) {
    switch (v) {
    case TimestampStatus::None: return "none";
    case TimestampStatus::PresentUnverified: return "present, not verified";
    case TimestampStatus::Valid: return "valid";
    case TimestampStatus::Invalid: return "invalid";
    case TimestampStatus::Untrusted: return "untrusted";
    }
    return "?";
}

std::string_view toString(RevocationStatus v) {
    switch (v) {
    case RevocationStatus::NotChecked: return "not checked";
    case RevocationStatus::Good: return "good";
    case RevocationStatus::Revoked: return "revoked";
    case RevocationStatus::Unknown: return "unknown";
    }
    return "?";
}

std::string_view toString(PolicyStatus v) {
    switch (v) {
    case PolicyStatus::Compliant: return "compliant";
    case PolicyStatus::WeakLegacyAlgorithm: return "weak legacy algorithm";
    case PolicyStatus::NonCompliant: return "non-compliant";
    }
    return "?";
}

std::string_view toString(CertificateStatus v) {
    switch (v) {
    case CertificateStatus::Valid: return "valid";
    case CertificateStatus::Expired: return "expired";
    case CertificateStatus::NotYetValid: return "not yet valid";
    case CertificateStatus::UsageInvalid: return "key usage not suitable for signing";
    case CertificateStatus::Unknown: return "unknown";
    }
    return "?";
}

std::string_view toString(TrustStatus v) {
    switch (v) {
    case TrustStatus::Trusted: return "trusted";
    case TrustStatus::Untrusted: return "untrusted";
    case TrustStatus::Unknown: return "unknown";
    }
    return "?";
}

std::string_view toString(SignatureState v) {
    switch (v) {
    case SignatureState::ValidTrusted: return "valid, trusted";
    case SignatureState::ValidUntrusted: return "valid, untrusted signer";
    case SignatureState::ValidExpiredNow: return "valid, certificate expired now";
    case SignatureState::ValidAtTrustedTimestamp: return "valid at trusted timestamp";
    case SignatureState::Revoked: return "revoked";
    case SignatureState::DocumentModified: return "document modified";
    case SignatureState::PermittedModifications: return "valid, permitted modifications";
    case SignatureState::InvalidSignature: return "invalid signature";
    case SignatureState::UnableToValidate: return "unable to validate";
    case SignatureState::UnsupportedAlgorithm: return "unsupported algorithm";
    case SignatureState::WeakLegacyAlgorithm: return "weak legacy algorithm";
    }
    return "?";
}

SignatureState foldState(const ValidationResult& r) {
    if (r.revocation == RevocationStatus::Revoked) return SignatureState::Revoked;
    if (r.integrity == IntegrityStatus::Modified || r.modification == ModificationStatus::Disallowed)
        return SignatureState::DocumentModified;
    if (r.crypto == CryptoCheck::Invalid) return SignatureState::InvalidSignature;
    if (r.crypto == CryptoCheck::Unsupported) return SignatureState::UnsupportedAlgorithm;
    if (r.crypto != CryptoCheck::Valid || r.integrity != IntegrityStatus::Intact)
        return SignatureState::UnableToValidate;
    if (r.policy == PolicyStatus::WeakLegacyAlgorithm) return SignatureState::WeakLegacyAlgorithm;
    if (r.certificate == CertificateStatus::UsageInvalid) return SignatureState::InvalidSignature;

    const bool timestampTrusted = r.timestamp == TimestampStatus::Valid;
    if (r.certificate == CertificateStatus::Expired || r.certificate == CertificateStatus::NotYetValid) {
        if (timestampTrusted && r.trust == TrustStatus::Trusted) return SignatureState::ValidAtTrustedTimestamp;
        if (r.certificate == CertificateStatus::Expired)
            return r.trust == TrustStatus::Trusted ? SignatureState::ValidExpiredNow : SignatureState::ValidUntrusted;
        return SignatureState::UnableToValidate; // not yet valid
    }
    if (r.certificate != CertificateStatus::Valid) return SignatureState::UnableToValidate;
    if (r.modification == ModificationStatus::Permitted) return SignatureState::PermittedModifications;
    return r.trust == TrustStatus::Trusted ? SignatureState::ValidTrusted : SignatureState::ValidUntrusted;
}

} // namespace rivet::crypto
