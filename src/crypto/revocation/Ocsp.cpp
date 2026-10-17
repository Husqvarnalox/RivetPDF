// SPDX-License-Identifier: MPL-2.0
#include "crypto/revocation/Ocsp.hpp"

#include "crypto/X509Parse.hpp"
#include "crypto/revocation/Internal.hpp"

#include <algorithm>

namespace rivet::crypto {

namespace dr = rivet::crypto::der;
using namespace revocation_detail;

namespace {

constexpr std::uint8_t kEnumerated = 0x0A;

Bytes toBytes(ByteView v) { return Bytes(v.begin(), v.end()); }

struct CertIdHashes {
    Bytes nameHash;
    Bytes keyHash;
};

CryptoResult<CertIdHashes> hashesFor(CryptoProvider& provider, DigestAlgorithm alg, const CertificateInfo& cert,
                                     const CertificateInfo& issuer) {
    auto keyBits = publicKeyBitsOf(issuer);
    if (!keyBits) return std::unexpected(keyBits.error());
    auto nameHash = hashOf(provider, alg, cert.issuerDer);
    if (!nameHash) return std::unexpected(nameHash.error());
    auto keyHash = hashOf(provider, alg, *keyBits);
    if (!keyHash) return std::unexpected(keyHash.error());
    return CertIdHashes{std::move(*nameHash), std::move(*keyHash)};
}

struct ParsedSingle {
    EvidenceCertStatus status = EvidenceCertStatus::Unknown;
    TimePoint thisUpdate{};
    std::optional<TimePoint> nextUpdate;
    std::optional<TimePoint> revocationTime;
    std::optional<int> reason;
};

bool nonceMatches(ByteView extValue, ByteView nonce) {
    // RFC 6960 wraps the nonce in an OCTET STRING inside extnValue; OpenSSL
    // and some responders put the raw bytes there. Either form is accepted,
    // as long as it equals what was sent.
    if (std::equal(extValue.begin(), extValue.end(), nonce.begin(), nonce.end())) return true;
    auto inner = dr::readSingle(extValue);
    if (inner && inner->tag == dr::kOctetString)
        return std::equal(inner->content.begin(), inner->content.end(), nonce.begin(), nonce.end());
    return false;
}

OcspEvidence reject(OcspEvidence ev, EvidenceFailure f, std::string reason) {
    ev.verified = false;
    ev.failure = f;
    ev.reasons.push_back(std::move(reason));
    return ev;
}

} // namespace

CryptoResult<OcspRequest> buildOcspRequest(CryptoProvider& provider, const CertificateInfo& cert,
                                           const CertificateInfo& issuer, const OcspRequestOptions& options) {
    if (!provider.isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, "no crypto backend available");
    auto digestOidText = std::string(digestOid(options.certIdHash));
    if (digestOidText.empty() || !provider.supports(options.certIdHash))
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "unsupported CertID hash");
    auto hashes = hashesFor(provider, options.certIdHash, cert, issuer);
    if (!hashes) return std::unexpected(hashes.error());

    const Bytes certId = dr::sequence({dr::sequence({dr::oidElement(digestOidText), dr::nullValue()}),
                                       dr::octetString(hashes->nameHash), dr::octetString(hashes->keyHash),
                                       dr::integer(cert.serial.empty() ? Bytes{0} : cert.serial)});
    std::vector<Bytes> tbs;
    tbs.push_back(dr::sequence({dr::sequence({certId})}));

    OcspRequest out;
    out.certIdHash = options.certIdHash;
    if (options.nonce) {
        auto random = provider.randomBytes(16);
        if (!random) return std::unexpected(random.error());
        Bytes nonce = *random;
        if (!nonce.empty() && nonce[0] == 0) nonce[0] = 1; // never an all-zero-prefix ambiguity
        const Bytes ext = dr::sequence({dr::oidElement(kOidOcspNonce), dr::octetString(dr::octetString(nonce))});
        tbs.push_back(dr::tlv(dr::contextTag(2, true), dr::sequence({ext})));
        out.nonce = std::move(nonce);
    }
    out.der = dr::sequence({dr::sequence(tbs)});
    return out;
}

OcspEvidence verifyOcspResponse(ByteView responseDer, CryptoProvider& provider, const CertificateInfo& cert,
                                const CertificateInfo& issuer, const std::optional<Bytes>& expectedNonce,
                                const EvidenceClock& clock) {
    OcspEvidence ev;
    ev.rawResponseDer = toBytes(responseDer);
    ev.nonceSent = expectedNonce.has_value();
    if (!provider.isAvailable()) return reject(std::move(ev), EvidenceFailure::BackendUnavailable, "no crypto backend available");

    auto malformed = [&](std::string why) { return reject(ev, EvidenceFailure::Malformed, "malformed OCSP response: " + std::move(why)); };

    auto top = dr::readSingle(responseDer);
    if (!top || top->tag != dr::kSequence) return malformed("not a SEQUENCE");
    auto topKids = dr::children(*top);
    if (!topKids || topKids->empty() || (*topKids)[0].tag != kEnumerated || (*topKids)[0].content.size() != 1)
        return malformed("missing responseStatus");
    switch ((*topKids)[0].content[0]) {
    case 0: ev.responseStatus = OcspResponseStatus::Successful; break;
    case 1: ev.responseStatus = OcspResponseStatus::MalformedRequest; break;
    case 2: ev.responseStatus = OcspResponseStatus::InternalError; break;
    case 3: ev.responseStatus = OcspResponseStatus::TryLater; break;
    case 5: ev.responseStatus = OcspResponseStatus::SigRequired; break;
    case 6: ev.responseStatus = OcspResponseStatus::Unauthorized; break;
    default: ev.responseStatus = OcspResponseStatus::Unknown; break;
    }
    if (ev.responseStatus != OcspResponseStatus::Successful)
        return reject(std::move(ev), EvidenceFailure::ResponderRejected,
                      "the responder did not answer with a status: " + std::string(toString(ev.responseStatus)));
    if (topKids->size() < 2 || (*topKids)[1].tag != dr::contextTag(0, true)) return malformed("no responseBytes");

    auto rbWrap = dr::children((*topKids)[1]);
    if (!rbWrap || rbWrap->empty() || (*rbWrap)[0].tag != dr::kSequence) return malformed("bad responseBytes");
    auto rb = dr::children((*rbWrap)[0]);
    if (!rb || rb->size() != 2 || (*rb)[0].tag != dr::kOid || (*rb)[1].tag != dr::kOctetString) return malformed("bad responseBytes");
    auto typeOid = dr::decodeOid((*rb)[0].content);
    if (!typeOid || *typeOid != kOidOcspBasic)
        return reject(std::move(ev), EvidenceFailure::UnsupportedAlgorithm, "unsupported OCSP response type");
    const Bytes basicBytes = toBytes((*rb)[1].content);
    auto basic = dr::readSingle(basicBytes);
    if (!basic || basic->tag != dr::kSequence) return malformed("BasicOCSPResponse is not a SEQUENCE");
    auto bk = dr::children(*basic);
    if (!bk || bk->size() < 3 || (*bk)[0].tag != dr::kSequence || (*bk)[1].tag != dr::kSequence || (*bk)[2].tag != dr::kBitString)
        return malformed("BasicOCSPResponse layout");
    const dr::Element& tbs = (*bk)[0];
    const dr::Element& sigAlg = (*bk)[1];
    const dr::Element& sigBits = (*bk)[2];
    std::vector<Bytes> embedded;
    if (bk->size() > 3) {
        if ((*bk)[3].tag != dr::contextTag(0, true)) return malformed("unexpected field after signature");
        auto w = dr::children((*bk)[3]);
        if (!w || w->empty() || (*w)[0].tag != dr::kSequence) return malformed("bad certs field");
        auto certs = dr::children((*w)[0]);
        if (!certs) return malformed("bad certs field");
        for (const dr::Element& c : *certs) embedded.push_back(toBytes(c.full));
    }

    // ---- ResponseData ----
    auto rd = dr::children(tbs);
    if (!rd) return malformed("ResponseData");
    std::size_t i = 0;
    if (i < rd->size() && (*rd)[i].tag == dr::contextTag(0, true)) ++i; // version
    if (i >= rd->size()) return malformed("no responderID");
    std::optional<Bytes> responderNameDer;
    std::optional<Bytes> responderKeyHash;
    {
        const dr::Element& id = (*rd)[i];
        auto inner = dr::children(id);
        if (!inner || inner->empty()) return malformed("bad responderID");
        if (id.tag == dr::contextTag(1, true) && (*inner)[0].tag == dr::kSequence) responderNameDer = toBytes((*inner)[0].full);
        else if (id.tag == dr::contextTag(2, true) && (*inner)[0].tag == dr::kOctetString) responderKeyHash = toBytes((*inner)[0].content);
        else return malformed("unknown responderID choice");
        ++i;
    }
    if (i + 1 >= rd->size() || (*rd)[i].tag != dr::kGeneralizedTime) return malformed("no producedAt");
    {
        auto t = dr::decodeTime((*rd)[i]);
        if (!t) return malformed("bad producedAt");
        ev.producedAt = *t;
        ++i;
    }
    if ((*rd)[i].tag != dr::kSequence) return malformed("no responses");
    auto singles = dr::children((*rd)[i]);
    if (!singles) return malformed("bad responses");
    const dr::Element* responseExtensions = nullptr;
    if (i + 1 < rd->size() && (*rd)[i + 1].tag == dr::contextTag(1, true)) responseExtensions = &(*rd)[i + 1];

    // Find the SingleResponse for our certificate.
    std::optional<ParsedSingle> found;
    bool sawAny = false;
    for (const dr::Element& sr : *singles) {
        auto f = dr::children(sr);
        if (sr.tag != dr::kSequence || !f || f->size() < 3) return malformed("bad SingleResponse");
        sawAny = true;
        auto idKids = dr::children((*f)[0]);
        if (!idKids || idKids->size() != 4 || (*idKids)[0].tag != dr::kSequence || (*idKids)[1].tag != dr::kOctetString ||
            (*idKids)[2].tag != dr::kOctetString || (*idKids)[3].tag != dr::kInteger)
            return malformed("bad CertID");
        auto algKids = dr::children((*idKids)[0]);
        if (!algKids || algKids->empty() || (*algKids)[0].tag != dr::kOid) return malformed("bad CertID algorithm");
        auto algOid = dr::decodeOid((*algKids)[0].content);
        if (!algOid) return malformed("bad CertID algorithm");
        auto alg = digestFromOid(*algOid);
        if (!alg || !provider.supports(*alg)) continue; // not comparable
        auto hashes = hashesFor(provider, *alg, cert, issuer);
        if (!hashes) continue;
        const Bytes serial = dr::normalizeInteger((*idKids)[3].content);
        const auto eq = [](const Bytes& a, ByteView b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); };
        if (!eq(hashes->nameHash, (*idKids)[1].content) || !eq(hashes->keyHash, (*idKids)[2].content) || serial != cert.serial) continue;

        ParsedSingle ps;
        const dr::Element& st = (*f)[1];
        if (st.tag == 0x80) {
            ps.status = EvidenceCertStatus::Good;
        } else if (st.tag == 0xA1) {
            ps.status = EvidenceCertStatus::Revoked;
            auto rk = dr::children(st);
            if (!rk || rk->empty() || (*rk)[0].tag != dr::kGeneralizedTime) return malformed("bad RevokedInfo");
            auto rt = dr::decodeTime((*rk)[0]);
            if (!rt) return malformed("bad revocationTime");
            ps.revocationTime = *rt;
            if (rk->size() > 1 && (*rk)[1].tag == dr::contextTag(0, true)) {
                auto rr = dr::children((*rk)[1]);
                if (!rr || rr->empty() || (*rr)[0].tag != kEnumerated || (*rr)[0].content.size() != 1) return malformed("bad revocationReason");
                ps.reason = (*rr)[0].content[0];
            }
        } else if (st.tag == 0x82) {
            ps.status = EvidenceCertStatus::Unknown;
        } else {
            return malformed("unknown certStatus");
        }
        if ((*f)[2].tag != dr::kGeneralizedTime) return malformed("no thisUpdate");
        auto tu = dr::decodeTime((*f)[2]);
        if (!tu) return malformed("bad thisUpdate");
        ps.thisUpdate = *tu;
        if (f->size() > 3 && (*f)[3].tag == dr::contextTag(0, true)) {
            auto nw = dr::children((*f)[3]);
            if (!nw || nw->empty()) return malformed("bad nextUpdate");
            auto nu = dr::decodeTime((*nw)[0]);
            if (!nu) return malformed("bad nextUpdate");
            ps.nextUpdate = *nu;
        }
        found = ps;
        break;
    }
    if (!found)
        return reject(std::move(ev), EvidenceFailure::CertIdMismatch,
                      sawAny ? "the response does not cover this certificate" : "the response contains no SingleResponse");
    ev.status = found->status;
    ev.thisUpdate = found->thisUpdate;
    ev.nextUpdate = found->nextUpdate;
    ev.revocationTime = found->revocationTime;
    ev.revocationReason = found->reason;

    // ---- responder identity and signature ----
    auto issuerKeyBits = publicKeyBitsOf(issuer);
    if (!issuerKeyBits) return reject(std::move(ev), EvidenceFailure::Malformed, "cannot read the issuer's public key");
    auto sha1Of = [&](ByteView v) -> Bytes {
        auto h = provider.digest(DigestAlgorithm::Sha1, v);
        return h ? *h : Bytes{};
    };
    auto idMatches = [&](ByteView subjectDer, ByteView keyBits) {
        if (responderNameDer) return std::equal(responderNameDer->begin(), responderNameDer->end(), subjectDer.begin(), subjectDer.end());
        const Bytes h = sha1Of(keyBits);
        return !h.empty() && h == *responderKeyHash;
    };

    const CertificateInfo* signer = nullptr;
    CertificateInfo delegated;
    if (idMatches(issuer.subjectDer, *issuerKeyBits)) {
        signer = &issuer;
        ev.responderName = issuer.subject;
    } else {
        std::string lastProblem = "the responder is not the issuing CA and no certificate in the response matches its ID";
        EvidenceFailure lastFailure = EvidenceFailure::UnauthorizedResponder;
        for (const Bytes& der : embedded) {
            X509Extras extras;
            auto parsed = parseX509(der, [&](DigestAlgorithm a, ByteView d) { return provider.digest(a, d); }, &extras);
            if (!parsed) continue;
            if (!idMatches(parsed->subjectDer, extras.publicKeyBits)) continue;
            if (parsed->issuerDer != issuer.subjectDer || !provider.isIssuedBy(*parsed, issuer)) {
                lastFailure = EvidenceFailure::ResponderNotIssuedByCa;
                lastProblem = "the delegated responder certificate was not issued by the CA that issued the certificate";
                continue;
            }
            if (std::find(parsed->extendedKeyUsageOids.begin(), parsed->extendedKeyUsageOids.end(), std::string(kOidKpOcspSigning)) ==
                parsed->extendedKeyUsageOids.end()) {
                lastFailure = EvidenceFailure::ResponderNoOcspSigning;
                lastProblem = "the delegated responder certificate lacks the id-kp-OCSPSigning extended key usage";
                continue;
            }
            if (!parsed->validAt(ev.producedAt)) {
                lastFailure = EvidenceFailure::ResponderCertInvalid;
                lastProblem = "the delegated responder certificate was not valid when the response was produced";
                continue;
            }
            delegated = std::move(*parsed);
            signer = &delegated;
            ev.responderName = delegated.subject;
            break;
        }
        if (!signer) {
            ev.responderName = responderNameDer ? "name:" + hexOf(sha1Of(*responderNameDer)) : "key:" + hexOf(*responderKeyHash);
            return reject(std::move(ev), lastFailure, lastProblem);
        }
    }
    if (auto ok = verifyWithCertificate(provider, *signer, sigAlg.full, tbs.full, sigBits.content); !ok) {
        const bool unsupported = ok.error().code == CryptoErrorCode::UnsupportedAlgorithm;
        return reject(std::move(ev), unsupported ? EvidenceFailure::UnsupportedAlgorithm : EvidenceFailure::SignatureInvalid,
                      "response signature: " + ok.error().message);
    }

    // ---- nonce ----
    if (expectedNonce) {
        bool echoed = false;
        if (responseExtensions) {
            auto w = dr::children(*responseExtensions);
            if (w && !w->empty()) {
                if (auto exts = readExtensions((*w)[0]))
                    for (const RawExtension& e : *exts)
                        if (e.oid == kOidOcspNonce && nonceMatches(e.value, *expectedNonce)) echoed = true;
            }
        }
        ev.nonceChecked = true;
        if (!echoed) return reject(std::move(ev), EvidenceFailure::NonceMismatch, "the nonce was not echoed (missing or different)");
    }

    // ---- time ----
    std::string why;
    if (auto problem = evidenceTimeProblem(ev.thisUpdate, ev.nextUpdate, ev.producedAt, clock, &why))
        return reject(std::move(ev), *problem, why);

    ev.verified = true;
    return ev;
}

// ---- client -----------------------------------------------------------------------------

OcspClient::OcspClient(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> provider)
    : network_(std::move(network)), provider_(std::move(provider)) {}

CryptoResult<OcspEvidence> OcspClient::exchange(std::string_view url, const CertificateInfo& cert,
                                                const CertificateInfo& issuer, const OcspRequestOptions& requestOptions,
                                                const OcspQueryOptions& options, const CancelToken& cancel) const {
    auto request = buildOcspRequest(*provider_, cert, issuer, requestOptions);
    if (!request) return std::unexpected(request.error());
    std::string body(reinterpret_cast<const char*>(request->der.data()), request->der.size());
    auto response = httpExchange(*network_, net::HttpMethod::Post, url, std::move(body),
                                 {{"Content-Type", std::string(kOcspRequestContentType)},
                                  {"Accept", std::string(kOcspResponseContentType)}},
                                 options.maxResponseBytes, cancel);
    if (!response) return std::unexpected(response.error());
    if (response->status != 200)
        return fail(CryptoErrorCode::BadResponse, "the OCSP responder answered with HTTP status " + std::to_string(response->status));
    const auto contentType = net::findHeader(response->headers, "Content-Type");
    if (!contentType || lowerMediaType(*contentType) != kOcspResponseContentType)
        return fail(CryptoErrorCode::BadResponse, "the OCSP reply has no application/ocsp-response Content-Type");
    if (response->body.empty()) return fail(CryptoErrorCode::BadResponse, "the OCSP reply is empty");

    OcspEvidence ev = verifyOcspResponse(viewOf(response->body), *provider_, cert, issuer, request->nonce, options.clock);
    ev.url = std::string(url);
    return ev;
}

CryptoResult<OcspEvidence> OcspClient::query(std::string_view url, const CertificateInfo& cert,
                                             const CertificateInfo& issuer, const OcspQueryOptions& options,
                                             const CancelToken& cancel) const {
    if (!network_ || !provider_) return fail(CryptoErrorCode::InvalidArgument, "OCSP client is not configured");
    if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
    if (!provider_->isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, "no crypto backend available");

    OcspRequestOptions ro;
    ro.nonce = options.nonce;
    ro.certIdHash = DigestAlgorithm::Sha256;
    auto first = exchange(url, cert, issuer, ro, options, cancel);
    if (!first) return first;
    const bool retry = options.sha1Fallback && (first->responseStatus == OcspResponseStatus::MalformedRequest ||
                                                first->responseStatus == OcspResponseStatus::Unauthorized);
    if (!retry) return first;

    // Some responders only understand SHA-1 CertIDs (RFC 6960 section 4.1.1
    // names SHA-1 as the mandatory-to-support hash).
    ro.certIdHash = DigestAlgorithm::Sha1;
    auto second = exchange(url, cert, issuer, ro, options, cancel);
    if (!second) return second;
    second->certIdSha1Fallback = true;
    second->reasons.push_back("answered after retrying with a SHA-1 CertID (first attempt: " +
                              std::string(toString(first->responseStatus)) + ")");
    return second;
}

} // namespace rivet::crypto
