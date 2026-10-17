// SPDX-License-Identifier: MPL-2.0
#include "crypto/revocation/Revocation.hpp"

namespace rivet::crypto {

std::string_view toString(EvidenceFailure f) {
    switch (f) {
    case EvidenceFailure::None: return "none";
    case EvidenceFailure::Malformed: return "malformed";
    case EvidenceFailure::ResponderRejected: return "responder rejected the request";
    case EvidenceFailure::CertIdMismatch: return "certificate id mismatch";
    case EvidenceFailure::NonceMismatch: return "nonce mismatch";
    case EvidenceFailure::SignatureInvalid: return "signature invalid";
    case EvidenceFailure::UnsupportedAlgorithm: return "unsupported algorithm";
    case EvidenceFailure::UnauthorizedResponder: return "unauthorized responder";
    case EvidenceFailure::ResponderNotIssuedByCa: return "responder certificate not issued by the CA";
    case EvidenceFailure::ResponderNoOcspSigning: return "responder certificate lacks OCSPSigning";
    case EvidenceFailure::ResponderCertInvalid: return "responder certificate not valid";
    case EvidenceFailure::NotYetValid: return "not yet valid";
    case EvidenceFailure::Stale: return "stale";
    case EvidenceFailure::DoesNotCoverTime: return "does not cover the validation time";
    case EvidenceFailure::IssuerMismatch: return "issuer mismatch";
    case EvidenceFailure::IssuerNotCrlSigner: return "issuer may not sign CRLs";
    case EvidenceFailure::NotApplicable: return "not applicable";
    case EvidenceFailure::UnsupportedFeature: return "unsupported feature";
    case EvidenceFailure::BackendUnavailable: return "no crypto backend";
    }
    return "?";
}

std::string_view toString(OcspResponseStatus s) {
    switch (s) {
    case OcspResponseStatus::Successful: return "successful";
    case OcspResponseStatus::MalformedRequest: return "malformedRequest";
    case OcspResponseStatus::InternalError: return "internalError";
    case OcspResponseStatus::TryLater: return "tryLater";
    case OcspResponseStatus::SigRequired: return "sigRequired";
    case OcspResponseStatus::Unauthorized: return "unauthorized";
    case OcspResponseStatus::Unknown: return "unknown";
    }
    return "?";
}

std::optional<EvidenceFailure> evidenceTimeProblem(TimePoint thisUpdate, std::optional<TimePoint> nextUpdate,
                                                   TimePoint producedAt, const EvidenceClock& clock,
                                                   std::string* reason) {
    auto say = [&](std::string text) {
        if (reason) *reason = std::move(text);
    };
    if (thisUpdate > clock.now + clock.tolerance) {
        say("thisUpdate lies in the future");
        return EvidenceFailure::NotYetValid;
    }
    if (producedAt > clock.now + clock.tolerance) {
        say("producedAt lies in the future");
        return EvidenceFailure::NotYetValid;
    }
    if (nextUpdate && *nextUpdate < thisUpdate) {
        say("nextUpdate precedes thisUpdate");
        return EvidenceFailure::Stale;
    }
    if (!clock.historical()) {
        if (nextUpdate) {
            if (*nextUpdate < clock.now) {
                say("nextUpdate has passed");
                return EvidenceFailure::Stale;
            }
        } else if (thisUpdate + clock.maxAge < clock.now) {
            say("no nextUpdate and thisUpdate is older than the maximum age");
            return EvidenceFailure::Stale;
        }
        return std::nullopt;
    }
    // Historical: the evidence must speak for validationTime.
    // A later "good" (or a revocation dated after the validation time) proves the certificate was fine then:
    // revocation does not go away. Judged by thisUpdate, the time the statement is about.
    if (thisUpdate >= clock.validationTime) return std::nullopt;
    const bool started = thisUpdate <= clock.validationTime;
    const bool notEnded = nextUpdate ? clock.validationTime <= *nextUpdate : clock.validationTime <= thisUpdate + clock.maxAge;
    if (started && notEnded) return std::nullopt;
    say("evidence predates the validation time and its validity interval does not include it");
    return EvidenceFailure::DoesNotCoverTime;
}

} // namespace rivet::crypto
