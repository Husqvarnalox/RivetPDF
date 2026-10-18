// SPDX-License-Identifier: MPL-2.0
#include "app/SecurityText.hpp"

#include "crypto/CertificateDisplay.hpp"

#include <format>
#include <utility>

namespace rivet::app {

namespace {

using crypto::SignatureClass;
using crypto::SignatureState;
using pdf::sign::PadesLevel;
using pdf::sign::SignErrorCode;

// Days since 1970-01-01 to a civil date (H. Hinnant's algorithm).
void civilFromDays(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

std::string dateOnly(std::chrono::system_clock::time_point time) { return formatUtc(time).substr(0, 10); }

} // namespace

std::string formatUtc(std::chrono::system_clock::time_point time) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    std::int64_t days = seconds / 86400;
    std::int64_t rest = seconds % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    int y = 1970;
    unsigned m = 1;
    unsigned d = 1;
    civilFromDays(days, y, m, d);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02} UTC", y, m, d, rest / 3600, (rest % 3600) / 60);
}

std::string signatureStateTitle(SignatureState state) {
    switch (state) {
    case SignatureState::ValidTrusted: return "Valid, signer is trusted";
    case SignatureState::ValidUntrusted: return "Intact, but the signer is not in your trusted roots";
    case SignatureState::ValidExpiredNow: return "Valid, but the signer's certificate has expired since";
    case SignatureState::ValidAtTrustedTimestamp: return "Valid at the time-stamp (certificate has expired since)";
    case SignatureState::ValidTimestamped: return "Valid, signer is trusted, time-stamped";
    case SignatureState::ValidTimestampInvalid: return "Valid, but its time-stamp is not valid or not trusted";
    case SignatureState::Revoked: return "The signer's certificate was revoked";
    case SignatureState::DocumentModified: return "The document was changed after signing";
    case SignatureState::PermittedModifications: return "Valid; only permitted changes were made after signing";
    case SignatureState::InvalidSignature: return "Invalid signature";
    case SignatureState::UnableToValidate: return "Could not be validated";
    case SignatureState::UnsupportedAlgorithm: return "Uses an algorithm Rivet does not support";
    case SignatureState::WeakLegacyAlgorithm: return "Valid, but uses a weak legacy algorithm";
    }
    return "Unknown state";
}

Severity signatureStateSeverity(SignatureState state) {
    switch (state) {
    case SignatureState::ValidTrusted:
    case SignatureState::ValidAtTrustedTimestamp:
    case SignatureState::ValidTimestamped:
    case SignatureState::PermittedModifications:
        return Severity::Good;
    case SignatureState::ValidUntrusted:
    case SignatureState::ValidExpiredNow:
    case SignatureState::ValidTimestampInvalid:
    case SignatureState::WeakLegacyAlgorithm:
    case SignatureState::UnableToValidate:
    case SignatureState::UnsupportedAlgorithm:
        return Severity::Warning;
    case SignatureState::Revoked:
    case SignatureState::DocumentModified:
    case SignatureState::InvalidSignature:
        return Severity::Bad;
    }
    return Severity::Unknown;
}

std::string levelShortName(PadesLevel level) {
    switch (level) {
    case PadesLevel::NotPades: return "not PAdES";
    case PadesLevel::BaselineB: return "B-B";
    case PadesLevel::BaselineT: return "B-T";
    case PadesLevel::BaselineLT: return "B-LT";
    case PadesLevel::BaselineLTA: return "B-LTA";
    }
    return "unknown";
}

std::string levelLabel(PadesLevel level) {
    switch (level) {
    case PadesLevel::NotPades: return "Not a PAdES signature (basic PDF signature)";
    case PadesLevel::BaselineB: return "PAdES B-B: basic signature";
    case PadesLevel::BaselineT: return "PAdES B-T: with a signature time-stamp";
    case PadesLevel::BaselineLT: return "PAdES B-LT: time-stamp plus embedded validation data";
    case PadesLevel::BaselineLTA: return "PAdES B-LTA: long-term validation with an archive time-stamp";
    }
    return "Unknown level";
}

bool levelNeedsTimestamp(PadesLevel level) {
    return level == PadesLevel::BaselineT || level == PadesLevel::BaselineLT || level == PadesLevel::BaselineLTA;
}

bool levelNeedsNetwork(PadesLevel level) { return levelNeedsTimestamp(level); }

std::string qualificationLabel(const crypto::SignatureClassification& classification,
                               crypto::ProviderAssurance assurance) {
    SignatureClass level = classification.level;
    // Defence in depth: only a certified-CSP-declared provider can ever lead
    // to the qualified-evidence wording. Software keys and remote services
    // are capped at the advanced-indicators wording.
    if (level == SignatureClass::QualifiedEvidence && assurance != crypto::ProviderAssurance::CertifiedCspDeclared) {
        level = SignatureClass::AdvancedUnqualified;
    }
    switch (level) {
    case SignatureClass::Unknown: return "Signature class: could not be determined";
    case SignatureClass::SimpleCrypto:
        return "Simple electronic signature: the cryptography checks out, but no identity evidence was found";
    case SignatureClass::GostDigitalSignature:
        return "GOST digital signature: algorithm evidence only, no further identity evidence";
    case SignatureClass::AdvancedUnqualified:
        return "Advanced-signature indicators present; not shown to be a qualified signature";
    case SignatureClass::QualifiedEvidence:
        return "All technical indicators of a qualified signature are present (evidence only, not a legal finding)";
    }
    return "Signature class: could not be determined";
}

std::string certificateSummary(const crypto::CertificateInfo& certificate, std::chrono::system_clock::time_point now) {
    const crypto::CertificateDisplay display = crypto::makeCertificateDisplay(certificate, nullptr, nullptr, false, now);
    std::string name = display.personName.value_or(display.subjectName.value_or(certificate.subject));
    if (display.organization.has_value() && *display.organization != name) name += " (" + *display.organization + ")";
    std::string text = std::move(name);
    if (display.issuerName.has_value()) text += ", issued by " + *display.issuerName;
    if (display.expired) {
        text += ", expired " + dateOnly(display.validTo);
    } else if (display.notYetValid) {
        text += ", not valid before " + dateOnly(display.validFrom);
    } else {
        text += ", valid until " + dateOnly(display.validTo);
    }
    return text;
}

SignatureRowModel makeSignatureRow(const pdf::sign::PdfSignatureInfo& info, const crypto::AccreditedCaList* list,
                                   std::chrono::system_clock::time_point now) {
    SignatureRowModel row;
    row.index = info.index;
    row.field = info.fieldName;
    const bool docTimestamp = info.kind == pdf::sign::SignatureKind::DocumentTimestamp;
    row.stateTitle = signatureStateTitle(info.validation.state);
    row.severity = signatureStateSeverity(info.validation.state);
    row.disclaimer = std::string(crypto::kLegalDisclaimer);

    std::string who;
    if (info.signer.has_value()) {
        row.signer = certificateSummary(*info.signer, now);
        const crypto::CertificateDisplay display =
            crypto::makeCertificateDisplay(*info.signer, nullptr, nullptr, false, now);
        who = display.personName.value_or(display.subjectName.value_or(std::string()));
    }
    if (who.empty()) who = info.signerName;
    row.heading = docTimestamp ? std::format("Document time-stamp {}", info.index + 1)
                               : std::format("Signature {}{}{}", info.index + 1, who.empty() ? "" : ": ", who);
    row.signingTime = info.signingTimeText;
    if (!info.reason.empty()) row.notes.push_back("Reason: " + info.reason);
    if (!info.location.empty()) row.notes.push_back("Location: " + info.location);

    if (info.timestamp.has_value() && info.timestamp->status != crypto::TimestampStatus::None) {
        const auto& ts = *info.timestamp;
        std::string text = ts.verified ? "Time-stamp: valid" : "Time-stamp: present but not verified";
        if (ts.genTime.has_value()) text += ", " + formatUtc(*ts.genTime);
        if (!ts.tsaSubject.empty()) text += ", by " + ts.tsaSubject;
        row.timestamp = std::move(text);
    } else {
        row.timestamp = "No signature time-stamp";
    }
    row.level = docTimestamp ? "Document time-stamp (RFC 3161)" : levelLabel(info.level);

    if (docTimestamp) {
        row.qualification = "A document time-stamp proves when the document existed; it is not a signature by a person";
    } else if (info.signer.has_value()) {
        // A file does not reveal where the private key lived: custody is
        // unknown, so the classification is made as a software key and can
        // never reach the advanced or qualified wording from a file alone.
        crypto::ClassificationInput input;
        input.signer = &*info.signer;
        input.provider = crypto::ProviderAssurance::Software;
        input.list = list;
        input.validAt = now;
        row.qualification = qualificationLabel(crypto::classify(input), crypto::ProviderAssurance::Software) +
                            " (key custody cannot be checked from a file)";
    } else {
        row.qualification = "Signature class: no signer certificate to examine";
    }

    for (const std::string& note : info.validation.notes) row.notes.push_back(note);
    if (info.level >= PadesLevel::BaselineT && info.ltv.evaluated) {
        for (const std::string& reason : info.ltv.reasons) row.notes.push_back(reason);
        for (const std::string& missing : info.ltv.missing) row.notes.push_back("Missing validation data: " + missing);
    }
    return row;
}

std::string describeSignError(const pdf::sign::SignError& error) {
    const auto withDetail = [&](std::string text) {
        if (!error.message.empty()) text += " (" + error.message + ")";
        return text;
    };
    switch (error.code) {
    case SignErrorCode::Cancelled: return "Signing was cancelled. The original file was not changed.";
    case SignErrorCode::EncryptedDocument:
        return "This document is password-protected and cannot be signed. Sign it before setting a password.";
    case SignErrorCode::ViolatesCertification:
        return "This document is certified against further changes, so it cannot be signed again.";
    case SignErrorCode::CertificationNotAllowed:
        return "A certification signature must be the first signature of a document.";
    case SignErrorCode::FieldNotFound: return withDetail("The chosen signature field does not exist.");
    case SignErrorCode::FieldNotEmpty: return withDetail("The chosen signature field is already signed.");
    case SignErrorCode::MalformedPdf:
    case SignErrorCode::UnsupportedStructure:
        return withDetail("This PDF has a structure Rivet cannot sign safely.");
    case SignErrorCode::LimitExceeded: return withDetail("The document is too large or too complex to sign.");
    case SignErrorCode::SignatureTooLarge:
    case SignErrorCode::ContentsPlaceholderTooSmall:
        return withDetail("The signature (with its certificate chain and time-stamp) does not fit. Try again.");
    case SignErrorCode::TimestampFailed:
        return withDetail("The time-stamp could not be obtained. The document was not signed.");
    case SignErrorCode::EvidenceUnavailable:
        return withDetail("Long-term validation data could not be collected for every certificate (" +
                          std::to_string(error.missingEvidence.size()) +
                          " missing). Choose a lower level or try again online. The document was not signed.");
    case SignErrorCode::UnsupportedUpgrade: return withDetail("This signature level cannot be reached from this document.");
    case SignErrorCode::SelfVerifyFailed:
        return withDetail("The new signature failed Rivet's own check, so nothing was written.");
    case SignErrorCode::Io: return withDetail("The file could not be read or written.");
    case SignErrorCode::InvalidArgument: return withDetail("The signing settings are not valid.");
    case SignErrorCode::Internal: return withDetail("Signing failed unexpectedly.");
    case SignErrorCode::Crypto: break;
    }
    // Crypto errors: say what happened to the key, not how it failed inside.
    if (error.cryptoCode.has_value()) {
        switch (*error.cryptoCode) {
        case crypto::CryptoErrorCode::Cancelled: return "Signing was cancelled. The original file was not changed.";
        case crypto::CryptoErrorCode::WrongPassword:
        case crypto::CryptoErrorCode::ConfirmationRejected: return "The PIN was not accepted.";
        case crypto::CryptoErrorCode::PinLocked: return "The PIN is blocked. Unblock the token or card with its own tool.";
        case crypto::CryptoErrorCode::DeviceRemoved: return "The token or smart card is not connected.";
        case crypto::CryptoErrorCode::SessionExpired: return "The signing session ended. Try again.";
        case crypto::CryptoErrorCode::NoPrivateKey:
        case crypto::CryptoErrorCode::CertificateNotFound: return "The private key for this identity could not be found.";
        case crypto::CryptoErrorCode::Network:
            return "An online request was refused or failed (online validation may be off, or the server is unreachable).";
        case crypto::CryptoErrorCode::Timeout: return "The signing service did not answer in time.";
        case crypto::CryptoErrorCode::AuthFailed: return "The signing service rejected the sign-in.";
        case crypto::CryptoErrorCode::NotQualified:
            return "The signing service did not allow this identity to sign. Rivet does not claim any qualified status.";
        case crypto::CryptoErrorCode::BackendUnavailable:
            return "This build has no cryptography backend, so documents cannot be signed.";
        case crypto::CryptoErrorCode::LicenseInvalid: return "The signing provider reports an invalid or missing licence.";
        case crypto::CryptoErrorCode::WeakAlgorithm:
        case crypto::CryptoErrorCode::UnsupportedAlgorithm:
            return "This key uses an algorithm Rivet does not allow for new signatures.";
        default: break;
        }
    }
    return withDetail("Signing failed.");
}

std::string describeEncryptError(const core::Error& error) {
    switch (error.code) {
    case core::ErrorCode::Cancelled: return "Setting the password was cancelled. No file was written.";
    case core::ErrorCode::Unsupported:
        return "This document cannot be password-protected by Rivet (" + error.message + "). No file was written.";
    case core::ErrorCode::NotAvailable: return "This build has no cryptography backend, so passwords cannot be set.";
    case core::ErrorCode::InvalidArgument: return "The password settings are not valid: " + error.message;
    case core::ErrorCode::PermissionDenied: return "Rivet may not write to that location.";
    case core::ErrorCode::DiskFull: return "There is not enough disk space to write the file.";
    default: return "Setting the password failed: " + core::describe(error);
    }
}

} // namespace rivet::app
