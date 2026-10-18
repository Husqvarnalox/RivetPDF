// SPDX-License-Identifier: MPL-2.0
#include "crypto/revocation/Crl.hpp"

#include "crypto/revocation/Internal.hpp"

#include <algorithm>

namespace rivet::crypto {

namespace dr = rivet::crypto::der;
using namespace revocation_detail;

namespace {

constexpr std::uint8_t kEnumerated = 0x0A;
constexpr std::string_view kOidAki = "2.5.29.35";
constexpr std::string_view kOidIan = "2.5.29.18";
constexpr std::string_view kOidCrlNumber = "2.5.29.20";
constexpr std::string_view kOidReasonCode = "2.5.29.21";
constexpr std::string_view kOidDeltaIndicator = "2.5.29.27";
constexpr std::string_view kOidIdp = "2.5.29.28";
constexpr std::string_view kOidCertIssuer = "2.5.29.29";

CrlEvidence reject(CrlEvidence ev, EvidenceFailure f, std::string reason) {
    ev.verified = false;
    ev.failure = f;
    ev.reasons.push_back(std::move(reason));
    return ev;
}

bool isTimeTag(std::uint8_t t) { return t == dr::kUtcTime || t == dr::kGeneralizedTime; }

// Decides whether the issuingDistributionPoint scope covers the certificate.
// Returns nullopt when it does, else the failure and reason.
std::optional<std::pair<EvidenceFailure, std::string>> idpScopeProblem(ByteView idpValue, const CertificateInfo& cert,
                                                                       const RevocationExtensions& certExt,
                                                                       std::vector<std::string>& limitations) {
    auto seq = dr::readSingle(idpValue);
    if (!seq || seq->tag != dr::kSequence) return std::make_pair(EvidenceFailure::Malformed, std::string("malformed issuingDistributionPoint"));
    auto fields = dr::children(*seq);
    if (!fields) return std::make_pair(EvidenceFailure::Malformed, std::string("malformed issuingDistributionPoint"));
    auto flag = [](const dr::Element& e) { return e.content.size() == 1 && e.content[0] != 0; };
    std::vector<Bytes> idpNames;
    bool hasDpName = false;
    bool relativeName = false;
    for (const dr::Element& f : *fields) {
        if (f.tag == dr::contextTag(0, true)) {
            auto inner = dr::children(f);
            if (!inner || inner->empty()) return std::make_pair(EvidenceFailure::Malformed, std::string("malformed IDP distributionPoint"));
            hasDpName = true;
            if ((*inner)[0].tag == dr::contextTag(0, true)) {
                auto gns = dr::children((*inner)[0]);
                if (!gns) return std::make_pair(EvidenceFailure::Malformed, std::string("malformed IDP fullName"));
                for (const dr::Element& gn : *gns) idpNames.emplace_back(gn.full.begin(), gn.full.end());
            } else {
                relativeName = true;
            }
        } else if (f.tag == 0x84 && flag(f)) {
            return std::make_pair(EvidenceFailure::UnsupportedFeature, std::string("indirect CRLs are not supported"));
        } else if (f.tag == 0x83) {
            limitations.push_back("the CRL covers only some revocation reasons (onlySomeReasons)");
            return std::make_pair(EvidenceFailure::NotApplicable, std::string("the CRL covers only some revocation reasons, so it cannot prove the certificate is not revoked"));
        } else if (f.tag == 0x85 && flag(f)) {
            return std::make_pair(EvidenceFailure::NotApplicable, std::string("the CRL contains only attribute certificates"));
        }
    }
    for (const dr::Element& f : *fields) {
        if (f.tag == 0x81 && flag(f) && cert.isCa)
            return std::make_pair(EvidenceFailure::NotApplicable, std::string("the CRL covers only end-entity certificates (onlyContainsUserCerts)"));
        if (f.tag == 0x82 && flag(f) && !cert.isCa)
            return std::make_pair(EvidenceFailure::NotApplicable, std::string("the CRL covers only CA certificates (onlyContainsCACerts)"));
    }
    if (hasDpName) {
        if (relativeName) return std::make_pair(EvidenceFailure::NotApplicable, std::string("the CRL's distribution point is a relative name (not supported)"));
        bool covered = false;
        for (const DistributionPointInfo& dp : certExt.crlDistributionPoints)
            for (const Bytes& n : dp.generalNames)
                if (std::find(idpNames.begin(), idpNames.end(), n) != idpNames.end()) covered = true;
        if (!covered)
            return std::make_pair(EvidenceFailure::NotApplicable, std::string("the CRL's distribution point is not one named by the certificate"));
    }
    return std::nullopt;
}

} // namespace

CrlEvidence verifyCrl(ByteView input, CryptoProvider& provider, const CertificateInfo& cert,
                      const CertificateInfo& issuer, const RevocationExtensions& certExt, const EvidenceClock& clock) {
    CrlEvidence ev;
    if (!provider.isAvailable()) return reject(std::move(ev), EvidenceFailure::BackendUnavailable, "no crypto backend available");

    Bytes der(input.begin(), input.end());
    {
        std::size_t k = 0;
        while (k < der.size() && (der[k] == ' ' || der[k] == '\t' || der[k] == '\r' || der[k] == '\n')) ++k;
        constexpr std::string_view kPem = "-----BEGIN";
        if (der.size() - k >= kPem.size() && std::equal(kPem.begin(), kPem.end(), der.begin() + static_cast<std::ptrdiff_t>(k))) {
            auto decoded = decodePemCrl(input);
            if (!decoded) return reject(std::move(ev), EvidenceFailure::Malformed, "malformed PEM CRL: " + decoded.error().message);
            der = std::move(*decoded);
        }
    }
    ev.rawCrlDer = der;
    auto malformed = [&](std::string why) { return reject(ev, EvidenceFailure::Malformed, "malformed CRL: " + std::move(why)); };

    auto top = dr::readSingle(ev.rawCrlDer);
    if (!top || top->tag != dr::kSequence) return malformed("not a SEQUENCE");
    auto tk = dr::children(*top);
    if (!tk || tk->size() != 3 || (*tk)[0].tag != dr::kSequence || (*tk)[1].tag != dr::kSequence || (*tk)[2].tag != dr::kBitString)
        return malformed("CertificateList layout");
    const dr::Element& tbs = (*tk)[0];
    const dr::Element& outerAlg = (*tk)[1];
    const dr::Element& sigBits = (*tk)[2];
    auto f = dr::children(tbs);
    if (!f) return malformed("TBSCertList");
    std::size_t i = 0;
    if (i < f->size() && (*f)[i].tag == dr::kInteger) {
        if ((*f)[i].content.size() != 1 || ((*f)[i].content[0] != 1 && (*f)[i].content[0] != 0)) return malformed("unsupported CRL version");
        ++i;
    }
    if (i + 3 > f->size() || (*f)[i].tag != dr::kSequence || (*f)[i + 1].tag != dr::kSequence || !isTimeTag((*f)[i + 2].tag))
        return malformed("TBSCertList fields");
    const dr::Element& innerAlg = (*f)[i];
    const dr::Element& issuerName = (*f)[i + 1];
    if (!std::equal(innerAlg.full.begin(), innerAlg.full.end(), outerAlg.full.begin(), outerAlg.full.end()))
        return malformed("the inner and outer signature algorithms differ");
    {
        auto tu = dr::decodeTime((*f)[i + 2]);
        if (!tu) return malformed("bad thisUpdate");
        ev.thisUpdate = *tu;
    }
    ev.issuerDer.assign(issuerName.full.begin(), issuerName.full.end());
    i += 3;
    if (i < f->size() && isTimeTag((*f)[i].tag)) {
        auto nu = dr::decodeTime((*f)[i]);
        if (!nu) return malformed("bad nextUpdate");
        ev.nextUpdate = *nu;
        ++i;
    }
    const dr::Element* revoked = nullptr;
    if (i < f->size() && (*f)[i].tag == dr::kSequence) revoked = &(*f)[i++];
    const dr::Element* extWrap = nullptr;
    if (i < f->size() && (*f)[i].tag == dr::contextTag(0, true)) extWrap = &(*f)[i++];
    if (i != f->size()) return malformed("unexpected fields in TBSCertList");

    // ---- issuer / signature ----
    if (ev.issuerDer != cert.issuerDer || ev.issuerDer != issuer.subjectDer)
        return reject(std::move(ev), EvidenceFailure::IssuerMismatch, "the CRL issuer is not the certificate's issuer (only direct CRLs are supported)");
    if (issuer.hasKeyUsage && !(issuer.keyUsage & keyusage::kCrlSign))
        return reject(std::move(ev), EvidenceFailure::IssuerNotCrlSigner, "the issuer certificate's key usage does not allow CRL signing");
    if (auto ok = verifyWithCertificate(provider, issuer, outerAlg.full, tbs.full, sigBits.content); !ok) {
        const bool unsupported = ok.error().code == CryptoErrorCode::UnsupportedAlgorithm;
        return reject(std::move(ev), unsupported ? EvidenceFailure::UnsupportedAlgorithm : EvidenceFailure::SignatureInvalid,
                      "CRL signature: " + ok.error().message);
    }

    // ---- extensions ----
    std::optional<Bytes> idp;
    if (extWrap) {
        auto w = dr::children(*extWrap);
        if (!w || w->empty()) return malformed("extensions");
        auto exts = readExtensions((*w)[0]);
        if (!exts) return malformed("extensions: " + exts.error().message);
        for (const RawExtension& e : *exts) {
            if (e.oid == kOidDeltaIndicator)
                return reject(std::move(ev), EvidenceFailure::UnsupportedFeature, "delta CRLs are not supported");
            if (e.oid == kOidIdp) {
                idp = Bytes(e.value.begin(), e.value.end());
            } else if (e.oid == kOidCrlNumber) {
                auto n = dr::readSingle(e.value);
                if (!n || n->tag != dr::kInteger) return malformed("cRLNumber");
                ev.crlNumber = dr::normalizeInteger(n->content);
            } else if (e.oid == kOidAki) {
                auto a = dr::readSingle(e.value);
                if (!a) return malformed("authorityKeyIdentifier");
                auto kids = dr::children(*a);
                if (!kids) return malformed("authorityKeyIdentifier");
                for (const dr::Element& k : *kids)
                    if (k.tag == 0x80 && !issuer.subjectKeyId.empty() &&
                        !std::equal(k.content.begin(), k.content.end(), issuer.subjectKeyId.begin(), issuer.subjectKeyId.end()))
                        return reject(std::move(ev), EvidenceFailure::IssuerMismatch, "the CRL's authority key identifier differs from the issuer's key identifier");
            } else if (e.oid == kOidFreshestCrl) {
                ev.limitations.push_back("a delta CRL pointer (freshestCRL) is present: delta CRLs are not processed");
            } else if (e.critical && e.oid != kOidIan) {
                return reject(std::move(ev), EvidenceFailure::UnsupportedFeature, "unsupported critical CRL extension " + e.oid);
            }
        }
    }

    // ---- scope ----
    if (idp) {
        if (auto problem = idpScopeProblem(*idp, cert, certExt, ev.limitations)) {
            ev.applicable = problem->first != EvidenceFailure::NotApplicable;
            return reject(std::move(ev), problem->first, problem->second);
        }
    }

    // ---- time ----
    std::string why;
    if (auto problem = evidenceTimeProblem(ev.thisUpdate, ev.nextUpdate, ev.thisUpdate, clock, &why))
        return reject(std::move(ev), *problem, why);

    // ---- lookup ----
    ev.status = EvidenceCertStatus::Good;
    if (revoked) {
        auto entries = dr::children(*revoked);
        if (!entries) return malformed("revokedCertificates");
        for (const dr::Element& entry : *entries) {
            auto ek = dr::children(entry);
            if (entry.tag != dr::kSequence || !ek || ek->size() < 2 || (*ek)[0].tag != dr::kInteger || !isTimeTag((*ek)[1].tag))
                return malformed("revokedCertificates entry");
            const bool match = dr::normalizeInteger((*ek)[0].content) == cert.serial;
            std::optional<int> reason;
            if (ek->size() > 2) {
                auto exts = readExtensions((*ek)[2]);
                if (!exts) return malformed("crlEntryExtensions");
                for (const RawExtension& e : *exts) {
                    if (e.oid == kOidCertIssuer)
                        return reject(std::move(ev), EvidenceFailure::UnsupportedFeature, "indirect CRLs (certificateIssuer entry extension) are not supported");
                    if (e.oid == kOidReasonCode) {
                        auto r = dr::readSingle(e.value);
                        if (!r || r->tag != kEnumerated || r->content.size() != 1) return malformed("reasonCode");
                        reason = r->content[0];
                    }
                }
            }
            if (!match) continue;
            auto when = dr::decodeTime((*ek)[1]);
            if (!when) return malformed("revocationDate");
            if (reason && *reason == 8) {
                ev.limitations.push_back("removeFromCRL is meaningful only in a delta CRL");
                return reject(std::move(ev), EvidenceFailure::UnsupportedFeature, "the entry has reason removeFromCRL, which only a delta CRL may carry");
            }
            ev.status = EvidenceCertStatus::Revoked;
            ev.revocationTime = *when;
            ev.revocationReason = reason;
            ev.onHold = reason && *reason == 6;
            break;
        }
    }
    ev.verified = true;
    return ev;
}

// ---- cache --------------------------------------------------------------------------------

std::optional<Bytes> CrlCache::find(const std::string& url, TimePoint now) {
    std::lock_guard lock(mutex_);
    auto it = entries_.find(url);
    if (it == entries_.end()) return std::nullopt;
    if (it->second.expiry < now) {
        bytes_ -= it->second.der.size();
        lru_.erase(it->second.order);
        entries_.erase(it);
        return std::nullopt;
    }
    lru_.splice(lru_.begin(), lru_, it->second.order);
    return it->second.der;
}

void CrlCache::insert(const std::string& url, Bytes der, TimePoint expiry) {
    if (der.size() > limits_.maxBytes || limits_.maxEntries == 0) return;
    std::lock_guard lock(mutex_);
    if (auto it = entries_.find(url); it != entries_.end()) {
        bytes_ -= it->second.der.size();
        lru_.erase(it->second.order);
        entries_.erase(it);
    }
    lru_.push_front(url);
    bytes_ += der.size();
    entries_[url] = Entry{std::move(der), expiry, lru_.begin()};
    evictLocked();
}

void CrlCache::evictLocked() {
    while (!lru_.empty() && (entries_.size() > limits_.maxEntries || bytes_ > limits_.maxBytes)) {
        const std::string victim = lru_.back();
        lru_.pop_back();
        auto it = entries_.find(victim);
        if (it != entries_.end()) {
            bytes_ -= it->second.der.size();
            entries_.erase(it);
        }
    }
}

void CrlCache::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    lru_.clear();
    bytes_ = 0;
}

std::size_t CrlCache::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::size_t CrlCache::totalBytes() const {
    std::lock_guard lock(mutex_);
    return bytes_;
}

// ---- client -------------------------------------------------------------------------------

CrlClient::CrlClient(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> provider,
                     std::shared_ptr<CrlCache> cache)
    : network_(std::move(network)), provider_(std::move(provider)), cache_(std::move(cache)) {}

CryptoResult<CrlEvidence> CrlClient::fetch(std::string_view url, const CertificateInfo& cert,
                                           const CertificateInfo& issuer, const RevocationExtensions& certExtensions,
                                           const CrlFetchOptions& options, const CancelToken& cancel) const {
    if (!network_ || !provider_) return fail(CryptoErrorCode::InvalidArgument, "CRL client is not configured");
    if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
    if (!provider_->isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, "no crypto backend available");
    const std::string key(url);

    if (options.useCache && cache_) {
        if (auto cached = cache_->find(key, options.clock.now)) {
            CrlEvidence ev = verifyCrl(*cached, *provider_, cert, issuer, certExtensions, options.clock);
            if (ev.verified || ev.failure == EvidenceFailure::NotApplicable) {
                ev.url = key;
                ev.fromCache = true;
                return ev;
            }
        }
    }

    auto response = httpExchange(*network_, net::HttpMethod::Get, url, {}, {{"Accept", "application/pkix-crl, application/x-pkcs7-crl, */*"}},
                                 options.maxResponseBytes, cancel);
    if (!response) return std::unexpected(response.error());
    if (response->status != 200)
        return fail(CryptoErrorCode::BadResponse, "the CRL server answered with HTTP status " + std::to_string(response->status));
    if (response->body.empty()) return fail(CryptoErrorCode::BadResponse, "the CRL reply is empty");

    CrlEvidence ev = verifyCrl(viewOf(response->body), *provider_, cert, issuer, certExtensions, options.clock);
    ev.url = key;
    if (ev.verified && cache_ && options.useCache) {
        const TimePoint expiry = ev.nextUpdate ? *ev.nextUpdate : ev.thisUpdate + options.clock.maxAge;
        cache_->insert(key, ev.rawCrlDer, expiry);
    }
    return ev;
}

} // namespace rivet::crypto
