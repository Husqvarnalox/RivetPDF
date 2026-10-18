// SPDX-License-Identifier: MPL-2.0
#include "crypto/revocation/RevocationChecker.hpp"

#include "crypto/Async.hpp"
#include "crypto/revocation/Internal.hpp"

#include <algorithm>

namespace rivet::crypto {

using namespace revocation_detail;

std::string_view toString(RevocationMode m) {
    switch (m) {
    case RevocationMode::Off: return "off";
    case RevocationMode::PreferOcspThenCrl: return "prefer OCSP, then CRL";
    case RevocationMode::OcspOnly: return "OCSP only";
    case RevocationMode::CrlOnly: return "CRL only";
    case RevocationMode::RequireAny: return "require OCSP or CRL";
    }
    return "?";
}

void VectorEvidenceStore::addOcsp(Bytes der) {
    std::lock_guard lock(mutex_);
    ocsp_.push_back(std::move(der));
}
void VectorEvidenceStore::addCrl(Bytes der) {
    std::lock_guard lock(mutex_);
    crl_.push_back(std::move(der));
}
std::vector<Bytes> VectorEvidenceStore::ocspResponses() const {
    std::lock_guard lock(mutex_);
    return ocsp_;
}
std::vector<Bytes> VectorEvidenceStore::crls() const {
    std::lock_guard lock(mutex_);
    return crl_;
}

bool RevocationOutcome::acceptable() const {
    if (status == RevocationStatus::Revoked) return false;
    if (status == RevocationStatus::Unknown && hardFail) return false;
    return true;
}

namespace {

// URLs come from certificates (untrusted): shown in notes only in bounded,
// printable form.
std::string shown(std::string_view url) {
    std::string out;
    for (char c : url.substr(0, 120)) out += (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F) ? c : '?';
    if (url.size() > 120) out += "...";
    return out;
}

struct Decision {
    RevocationStatus status = RevocationStatus::Unknown;
    std::optional<TimePoint> revokedAt;
    std::optional<int> reason;
    bool onHold = false;
    std::string note;
};

// Verified evidence -> verdict at the validation time. A revocation later
// than the validation time proves the certificate was still good then.
std::optional<Decision> decide(EvidenceCertStatus status, std::optional<TimePoint> revocationTime,
                               std::optional<int> reason, bool onHold, const EvidenceClock& clock) {
    Decision d;
    switch (status) {
    case EvidenceCertStatus::Good: d.status = RevocationStatus::Good; return d;
    case EvidenceCertStatus::Revoked:
        if (revocationTime && *revocationTime > clock.validationTime) {
            d.status = RevocationStatus::Good;
            d.note = "the certificate was revoked only after the validation time";
            return d;
        }
        d.status = RevocationStatus::Revoked;
        d.revokedAt = revocationTime;
        d.reason = reason;
        d.onHold = onHold;
        return d;
    case EvidenceCertStatus::Unknown: return std::nullopt; // "the responder does not know this certificate"
    }
    return std::nullopt;
}

void adopt(CertRevocation& r, const Decision& d, RevocationSource source) {
    r.status = d.status;
    r.source = source;
    r.revokedAt = d.revokedAt;
    r.reasonCode = d.reason;
    r.onHold = d.onHold;
    if (!d.note.empty()) r.notes.push_back(d.note);
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) out += "; ";
        out += s;
    }
    return out;
}

} // namespace

RevocationChecker::RevocationChecker(std::shared_ptr<CryptoProvider> provider,
                                     std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CrlCache> cache)
    : provider_(std::move(provider)), network_(std::move(network)), cache_(std::move(cache)) {}

CertRevocation RevocationChecker::checkOne(const std::vector<CertificateInfo>& chain, std::size_t index,
                                           const EvidenceClock& clock, const RevocationPolicy& policy,
                                           const CancelToken& cancel) const {
    const CertificateInfo& cert = chain[index];
    const CertificateInfo& issuer = chain[index + 1];
    CertRevocation r;
    r.chainIndex = index;
    r.subject = cert.subject;

    std::vector<std::string> problems; // why no usable evidence
    auto finishUnknown = [&] {
        r.status = RevocationStatus::Unknown;
        r.source = RevocationSource::None;
        r.unknownReason = problems.empty() ? "no usable revocation evidence" : join(problems);
        return r;
    };

    if (!provider_ || !provider_->isAvailable()) {
        problems.push_back("no crypto backend available");
        return finishUnknown();
    }
    auto ext = parseRevocationExtensions(cert.der);
    if (!ext) {
        problems.push_back("unreadable revocation extensions: " + ext.error().message);
        return finishUnknown();
    }
    if (ext->ocspNoCheck) {
        r.ocspNoCheck = true;
        r.notes.push_back("id-pkix-ocsp-nocheck: revocation checking skipped for this certificate");
        return r; // NotChecked
    }
    if (ext->freshestCrlPresent)
        r.notes.push_back("the certificate points to delta CRLs (freshestCRL): delta processing is not supported");

    const bool useOcsp = policy.mode == RevocationMode::PreferOcspThenCrl || policy.mode == RevocationMode::OcspOnly ||
                         policy.mode == RevocationMode::RequireAny;
    const bool useCrl = policy.mode == RevocationMode::PreferOcspThenCrl || policy.mode == RevocationMode::CrlOnly ||
                        policy.mode == RevocationMode::RequireAny;
    bool networkOpen = policy.allowNetwork && network_ != nullptr;

    // ---- OCSP ----
    if (useOcsp) {
        if (policy.store) {
            for (const Bytes& raw : policy.store->ocspResponses()) {
                if (cancel.cancelled()) break;
                OcspEvidence ev = verifyOcspResponse(raw, *provider_, cert, issuer, std::nullopt, clock);
                if (ev.failure == EvidenceFailure::CertIdMismatch || ev.failure == EvidenceFailure::Malformed ||
                    ev.failure == EvidenceFailure::ResponderRejected)
                    continue; // not about this certificate
                ev.url = "(supplied evidence)";
                const bool ok = ev.verified;
                if (!ok) problems.push_back("supplied OCSP response rejected: " + (ev.reasons.empty() ? std::string(toString(ev.failure)) : ev.reasons.back()));
                r.ocsp.push_back(std::move(ev));
                if (ok) {
                    const OcspEvidence& e = r.ocsp.back();
                    if (auto d = decide(e.status, e.revocationTime, e.revocationReason, false, clock)) {
                        r.acceptedOcsp = r.ocsp.size() - 1;
                        adopt(r, *d, RevocationSource::Ocsp);
                        return r;
                    }
                    problems.push_back("supplied OCSP response says the certificate is unknown to the responder");
                }
            }
        }
        if (networkOpen && !cancel.cancelled()) {
            if (ext->ocspUrls.empty()) problems.push_back("the certificate names no OCSP responder");
            OcspClient client(network_, provider_);
            OcspQueryOptions qo;
            qo.nonce = policy.useOcspNonce;
            qo.sha1Fallback = policy.allowSha1CertIdFallback;
            qo.maxResponseBytes = policy.maxOcspBytes;
            qo.clock = clock;
            for (const std::string& url : ext->ocspUrls) {
                if (cancel.cancelled() || !networkOpen) break;
                if (auto skip = classifyRevocationUrl(url)) {
                    r.notes.push_back("skipped OCSP URL " + shown(url) + ": " + *skip);
                    continue;
                }
                auto ev = client.query(url, cert, issuer, qo, cancel);
                if (!ev) {
                    problems.push_back("OCSP " + shown(url) + ": " + std::string(toString(ev.error().code)) + ": " + ev.error().message);
                    if (isGateRefusal(ev.error())) networkOpen = false;
                    if (ev.error().code == CryptoErrorCode::Cancelled) break;
                    continue;
                }
                const bool ok = ev->verified;
                if (!ok) problems.push_back("OCSP " + shown(url) + " rejected: " + (ev->reasons.empty() ? std::string(toString(ev->failure)) : ev->reasons.back()));
                r.ocsp.push_back(std::move(*ev));
                if (ok) {
                    const OcspEvidence& e = r.ocsp.back();
                    if (auto d = decide(e.status, e.revocationTime, e.revocationReason, false, clock)) {
                        r.acceptedOcsp = r.ocsp.size() - 1;
                        adopt(r, *d, RevocationSource::Ocsp);
                        return r;
                    }
                    problems.push_back("OCSP " + shown(url) + ": the responder does not know this certificate");
                }
            }
        } else if (!networkOpen && !policy.store) {
            problems.push_back("network access is not available for OCSP");
        }
    }

    // ---- CRL ----
    if (useCrl && !cancel.cancelled()) {
        auto consider = [&](CrlEvidence ev) -> bool {
            const bool ok = ev.verified;
            if (!ok) problems.push_back("CRL " + (ev.url.empty() ? std::string("(supplied)") : shown(ev.url)) + " rejected: " +
                                        (ev.reasons.empty() ? std::string(toString(ev.failure)) : ev.reasons.back()));
            for (const std::string& l : ev.limitations) r.notes.push_back("CRL limitation: " + l);
            r.crl.push_back(std::move(ev));
            if (!ok) return false;
            const CrlEvidence& e = r.crl.back();
            if (auto d = decide(e.status, e.revocationTime, e.revocationReason, e.onHold, clock)) {
                r.acceptedCrl = r.crl.size() - 1;
                adopt(r, *d, RevocationSource::Crl);
                return true;
            }
            return false;
        };
        if (policy.store) {
            for (const Bytes& raw : policy.store->crls()) {
                if (cancel.cancelled()) break;
                CrlEvidence ev = verifyCrl(raw, *provider_, cert, issuer, *ext, clock);
                if (ev.failure == EvidenceFailure::IssuerMismatch || ev.failure == EvidenceFailure::Malformed) continue; // not about this CA
                ev.url = "(supplied evidence)";
                if (consider(std::move(ev))) return r;
            }
        }
        if (networkOpen && !cancel.cancelled()) {
            CrlClient client(network_, provider_, cache_);
            CrlFetchOptions fo;
            fo.maxResponseBytes = policy.maxCrlBytes;
            fo.clock = clock;
            bool anyUri = false;
            for (const DistributionPointInfo& dp : ext->crlDistributionPoints) {
                if (dp.hasCrlIssuer || dp.hasReasons || dp.hasRelativeName) {
                    r.notes.push_back("skipped a CRL distribution point: indirect, reason-partitioned or relative-name points are not supported");
                    continue;
                }
                for (const std::string& url : dp.uris) {
                    anyUri = true;
                    if (cancel.cancelled() || !networkOpen) break;
                    if (auto skip = classifyRevocationUrl(url)) {
                        r.notes.push_back("skipped CRL URL " + shown(url) + ": " + *skip);
                        continue;
                    }
                    auto ev = client.fetch(url, cert, issuer, *ext, fo, cancel);
                    if (!ev) {
                        problems.push_back("CRL " + shown(url) + ": " + std::string(toString(ev.error().code)) + ": " + ev.error().message);
                        if (isGateRefusal(ev.error())) networkOpen = false;
                        if (ev.error().code == CryptoErrorCode::Cancelled) break;
                        continue;
                    }
                    if (consider(std::move(*ev))) return r;
                }
            }
            if (!anyUri && ext->crlDistributionPoints.empty()) problems.push_back("the certificate names no CRL distribution point");
        } else if (!networkOpen && !policy.store) {
            problems.push_back("network access is not available for CRLs");
        }
    }

    if (cancel.cancelled()) problems.push_back("cancelled");
    if (policy.store && r.ocsp.empty() && r.crl.empty() && !networkOpen)
        problems.push_back("no supplied evidence covers this certificate");
    return finishUnknown();
}

RevocationOutcome RevocationChecker::check(const std::vector<CertificateInfo>& chain, TimePoint validationTime,
                                           const RevocationPolicy& policy, const CancelToken& cancel) const {
    RevocationOutcome out;
    out.hardFail = policy.hardFail || policy.mode == RevocationMode::RequireAny;
    if (policy.mode == RevocationMode::Off) {
        out.notes.push_back("revocation checking is off");
        for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
            CertRevocation r;
            r.chainIndex = i;
            r.subject = chain[i].subject;
            out.certs.push_back(std::move(r));
        }
        return out;
    }
    if (chain.size() < 2) {
        out.notes.push_back("the path has no certificate other than the trust anchor: nothing to check");
        return out;
    }

    EvidenceClock clock;
    clock.now = policy.now.value_or(std::chrono::system_clock::now());
    clock.validationTime = validationTime;
    clock.tolerance = policy.clockTolerance;
    clock.maxAge = policy.maxAge;

    bool anyRevoked = false;
    bool anyUnknown = false;
    bool anyGood = false;
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        CertRevocation r = checkOne(chain, i, clock, policy, cancel);
        anyRevoked |= r.status == RevocationStatus::Revoked;
        anyUnknown |= r.status == RevocationStatus::Unknown;
        anyGood |= r.status == RevocationStatus::Good;
        out.certs.push_back(std::move(r));
    }
    out.status = anyRevoked   ? RevocationStatus::Revoked
                 : anyUnknown ? RevocationStatus::Unknown
                 : anyGood    ? RevocationStatus::Good
                              : RevocationStatus::NotChecked;
    return out;
}

bool RevocationChecker::checkAsync(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher,
                                   core::AsyncScope& scope, CancelToken cancel, std::vector<CertificateInfo> chain,
                                   TimePoint validationTime, RevocationPolicy policy,
                                   std::function<void(CryptoResult<RevocationOutcome>)> done) const {
    RevocationChecker self = *this;
    return runCryptoAsync<RevocationOutcome>(
        scheduler, dispatcher, scope, cancel,
        std::function<CryptoResult<RevocationOutcome>(const CancelToken&)>(
            [self = std::move(self), chain = std::move(chain), validationTime, policy = std::move(policy)](const CancelToken& token) {
                return CryptoResult<RevocationOutcome>(self.check(chain, validationTime, policy, token));
            }),
        std::move(done));
}

void applyRevocation(ValidationResult& result, const RevocationOutcome& outcome) {
    result.revocation = outcome.status;
    RevocationDetail detail;
    detail.status = outcome.status;
    detail.hardFail = outcome.hardFail;
    for (const CertRevocation& c : outcome.certs) {
        if (c.ocspNoCheck || c.status == RevocationStatus::NotChecked) continue;
        ++detail.certificatesChecked;
        if (c.status == RevocationStatus::Unknown) {
            ++detail.certificatesUnknown;
            result.notes.push_back("revocation unknown for '" + c.subject + "': " + c.unknownReason);
        }
        if (c.status == RevocationStatus::Revoked && !detail.revokedAt && !detail.reasonCode) {
            detail.revokedAt = c.revokedAt;
            detail.reasonCode = c.reasonCode;
            result.notes.push_back("certificate '" + c.subject + "' is revoked");
        }
    }
    detail.summary = std::string(toString(outcome.status));
    result.revocationDetail = std::move(detail);

    result.state = foldState(result);
    if (outcome.status == RevocationStatus::Unknown && outcome.hardFail) {
        switch (result.state) {
        case SignatureState::ValidTrusted:
        case SignatureState::ValidUntrusted:
        case SignatureState::ValidExpiredNow:
        case SignatureState::ValidAtTrustedTimestamp:
        case SignatureState::ValidTimestamped:
        case SignatureState::ValidTimestampInvalid:
        case SignatureState::PermittedModifications: result.state = SignatureState::UnableToValidate; break;
        default: break;
        }
    }
}

} // namespace rivet::crypto
