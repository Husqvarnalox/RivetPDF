// SPDX-License-Identifier: MPL-2.0
// OCSP / CRL clients, evidence verification and the revocation policy, against
// a local test CA with responses produced by OpenSSL. Nothing touches a socket.
#include "RivetTest.h"

#include "TestRevocationSupport.hpp"
#include "crypto/Async.hpp"
#include "crypto/revocation/RevocationChecker.hpp"
#include "net/MockTransport.hpp"

#include <condition_variable>
#include <deque>
#include <random>

using namespace rivet;
using namespace rivet::crypto;
using namespace rivet::crypto::testing;
using namespace rivet::crypto::testing::rev;
using Clock = std::chrono::system_clock;

namespace {

std::string failureText(const OcspEvidence& e) {
    std::string s = std::string(toString(e.failure)) + " / status " + std::string(toString(e.responseStatus));
    for (const std::string& r : e.reasons) s += " | " + r;
    return s;
}
std::string failureText(const CrlEvidence& e) {
    std::string s = std::string(toString(e.failure));
    for (const std::string& r : e.reasons) s += " | " + r;
    return s;
}

OcspEvidence ocspQuery(Env& env, const OcspQueryOptions& o = {}, const CertificateInfo* cert = nullptr) {
    OcspClient client(env.network, env.pki->provider);
    auto r = client.query(kOcspUrl, cert ? *cert : env.pki->leafInfo, env.pki->caInfo, o, CancelToken{});
    if (!r) {
        std::fprintf(stderr, "    query error %s: %s\n", std::string(toString(r.error().code)).c_str(), r.error().message.c_str());
        return {};
    }
    return std::move(*r);
}

// True (and silent) when the evidence was rejected with `want`; prints why otherwise.
bool rejectedWith(const OcspEvidence& e, EvidenceFailure want, const char* what) {
    if (!e.verified && e.failure == want) return true;
    std::fprintf(stderr, "    [%s] verified=%d, wanted %s, got %s\n", what, e.verified ? 1 : 0, std::string(toString(want)).c_str(),
                 failureText(e).c_str());
    return false;
}
bool rejectedWith(const CrlEvidence& e, EvidenceFailure want, const char* what) {
    if (!e.verified && e.failure == want) return true;
    std::fprintf(stderr, "    [%s] verified=%d, wanted %s, got %s\n", what, e.verified ? 1 : 0, std::string(toString(want)).c_str(),
                 failureText(e).c_str());
    return false;
}

CrlEvidence crlFetch(Env& env, CrlClient& client, const CrlFetchOptions& o = {}) {
    auto ext = parseRevocationExtensions(env.pki->leafInfo.der);
    if (!ext) return {};
    auto r = client.fetch(kCrlUrl, env.pki->leafInfo, env.pki->caInfo, *ext, o, CancelToken{});
    if (!r) {
        std::fprintf(stderr, "    fetch error %s: %s\n", std::string(toString(r.error().code)).c_str(), r.error().message.c_str());
        return {};
    }
    return std::move(*r);
}

RevocationChecker makeChecker(Env& env) { return RevocationChecker(env.pki->provider, env.network); }

RevocationOutcome runCheck(Env& env, RevocationPolicy policy = {}, std::optional<Clock::time_point> validationTime = std::nullopt,
                           const std::vector<CertificateInfo>* chain = nullptr) {
    auto checker = makeChecker(env);
    return checker.check(chain ? *chain : env.pki->chain(), validationTime.value_or(Clock::now()), policy, CancelToken{});
}

void dump(const RevocationOutcome& o) {
    for (const CertRevocation& c : o.certs) {
        std::fprintf(stderr, "    cert %zu '%s': %s via %d%s%s\n", c.chainIndex, c.subject.c_str(), std::string(toString(c.status)).c_str(),
                     static_cast<int>(c.source), c.unknownReason.empty() ? "" : " reason: ", c.unknownReason.c_str());
        for (const std::string& n : c.notes) std::fprintf(stderr, "      note: %s\n", n.c_str());
    }
}

std::vector<Bytes> mutations(const Bytes& valid, unsigned seed, int count) {
    std::mt19937 rng(seed);
    std::vector<Bytes> out;
    for (int i = 0; i < count; ++i) {
        Bytes m = valid;
        const int flips = 1 + static_cast<int>(rng() % 4);
        for (int f = 0; f < flips; ++f) m[rng() % m.size()] = static_cast<std::uint8_t>(rng());
        if (rng() % 5 == 0) m.resize(1 + rng() % m.size());
        if (rng() % 11 == 0) m.insert(m.begin() + static_cast<std::ptrdiff_t>(rng() % m.size()), static_cast<std::uint8_t>(rng()));
        out.push_back(std::move(m));
    }
    return out;
}

} // namespace


// ---- extensions ---------------------------------------------------------------------------

RIVET_TEST(revocationExtensionsAreParsed) {
    Pki pki("ec256", "caIssuers;URI:http://ca.example.com/ca.cer,OCSP;URI:ldap://dir.example.com/x",
            "URI:ldap://dir.example.com/cn=ca,URI:file:///etc/crl.pem");
    auto ext = parseRevocationExtensions(pki.leafInfo.der);
    CHECK(ext.has_value());
    CHECK_EQ(ext->ocspUrls.size(), std::size_t{2});
    CHECK_EQ(ext->caIssuerUrls.size(), std::size_t{1});
    CHECK_EQ(ext->caIssuerUrls[0], std::string("http://ca.example.com/ca.cer"));
    std::size_t uris = 0;
    for (const auto& dp : ext->crlDistributionPoints) uris += dp.uris.size();
    CHECK(uris >= 3);
    CHECK(!ext->ocspNoCheck);
    CHECK(classifyRevocationUrl("ldap://dir.example.com/x").has_value());
    CHECK(classifyRevocationUrl("file:///etc/passwd").has_value());
    CHECK(classifyRevocationUrl("ftp://x/y").has_value());
    CHECK(!classifyRevocationUrl("http://crl.example.com/a.crl").has_value());
    CHECK(!classifyRevocationUrl("https://crl.example.com/a.crl").has_value());
    CHECK(classifyRevocationUrl("http://exa mple.com/").has_value());
    auto caExt = parseRevocationExtensions(pki.caInfo.der);
    CHECK(caExt.has_value() && caExt->ocspUrls.empty() && caExt->crlDistributionPoints.empty());
    CHECK(!parseRevocationExtensions(Bytes{0x30, 0x00}).has_value());
}

RIVET_TEST(revocationNoCheckAndFreshestCrlAreRecorded) {
    Pki base;
    CertSpec s;
    s.cn = "nocheck responder";
    s.keyKind = "ec256";
    s.issuer = &base.ca;
    s.extKeyUsage = "OCSPSigning";
    s.extraExt = {{NID_id_pkix_OCSP_noCheck, "NULL"}, {NID_freshest_crl, "URI:http://crl.example.com/delta.crl"}};
    TestKey k = makeCert(s);
    auto info = base.provider->parseCertificate(certDer(k));
    CHECK(info.has_value());
    auto ext = parseRevocationExtensions(info->der);
    CHECK(ext.has_value());
    if (ext) {
        std::fprintf(stderr, "    nocheck=%d freshest=%d\n", ext->ocspNoCheck ? 1 : 0, ext->freshestCrlPresent ? 1 : 0);
        CHECK(ext->ocspNoCheck);
        CHECK(ext->freshestCrlPresent);
    }
    // The checker skips such a certificate and says so; it is not "good".
    Env env(std::make_shared<Pki>());
    std::vector<CertificateInfo> chain{*info, base.caInfo};
    auto out = runCheck(env, {}, std::nullopt, &chain);
    dump(out);
    CHECK(out.certs.size() == 1 && out.certs[0].ocspNoCheck);
    CHECK(out.certs[0].status == RevocationStatus::NotChecked);
    CHECK(out.status == RevocationStatus::NotChecked);
    CHECK_EQ(env.transport->calls.load(), 0);
}

// ---- OCSP request ---------------------------------------------------------------------

RIVET_TEST(ocspRequestShape) {
    Pki pki;
    auto parse = [](const Bytes& der) {
        const unsigned char* p = der.data();
        return std::unique_ptr<OCSP_REQUEST, OcspReqDel>(d2i_OCSP_REQUEST(nullptr, &p, static_cast<long>(der.size())));
    };
    auto def = buildOcspRequest(*pki.provider, pki.leafInfo, pki.caInfo);
    CHECK(def.has_value());
    auto req = parse(def->der);
    CHECK(req != nullptr);
    CHECK_EQ(OCSP_request_onereq_count(req.get()), 1);
    ASN1_OBJECT* md = nullptr;
    ASN1_INTEGER* serial = nullptr;
    OCSP_id_get0_info(nullptr, &md, nullptr, &serial, OCSP_onereq_get0_id(OCSP_request_onereq_get0(req.get(), 0)));
    CHECK_EQ(OBJ_obj2nid(md), static_cast<int>(NID_sha256)); // default CertID hash
    CHECK_EQ(ASN1_INTEGER_cmp(serial, X509_get_serialNumber(pki.leaf.cert.get())), 0);
    CHECK(def->nonce.has_value() && def->nonce->size() == 16);
    CHECK(OCSP_REQUEST_get_ext_by_NID(req.get(), NID_id_pkix_OCSP_Nonce, -1) >= 0);

    OcspRequestOptions o;
    o.nonce = false;
    o.certIdHash = DigestAlgorithm::Sha1;
    auto plain = buildOcspRequest(*pki.provider, pki.leafInfo, pki.caInfo, o);
    CHECK(plain.has_value() && !plain->nonce.has_value());
    auto req2 = parse(plain->der);
    CHECK(req2 != nullptr);
    OCSP_id_get0_info(nullptr, &md, nullptr, nullptr, OCSP_onereq_get0_id(OCSP_request_onereq_get0(req2.get(), 0)));
    CHECK_EQ(OBJ_obj2nid(md), static_cast<int>(NID_sha1));
    CHECK(OCSP_REQUEST_get_ext_by_NID(req2.get(), NID_id_pkix_OCSP_Nonce, -1) < 0);
    // Two requests never share a nonce.
    auto again = buildOcspRequest(*pki.provider, pki.leafInfo, pki.caInfo);
    CHECK(again.has_value() && *again->nonce != *def->nonce);
}

// ---- OCSP responses -------------------------------------------------------------------

RIVET_TEST(ocspGoodRevokedUnknown) {
    Env env;
    OcspEvidence good = ocspQuery(env);
    if (!good.verified) std::fprintf(stderr, "    good: %s\n", failureText(good).c_str());
    CHECK(good.verified);
    CHECK(good.status == EvidenceCertStatus::Good);
    CHECK(good.responseStatus == OcspResponseStatus::Successful);
    CHECK(good.nonceSent && good.nonceChecked);
    CHECK(!good.rawResponseDer.empty());
    CHECK(good.nextUpdate.has_value());
    CHECK(!good.certIdSha1Fallback);
    CHECK_EQ(env.transport->ocspCalls.load(), 1);
    const auto sent = env.transport->lastRequest;
    CHECK(sent.method == net::HttpMethod::Post);
    auto ct = net::findHeader(sent.headers, "Content-Type");
    CHECK(ct.has_value() && *ct == "application/ocsp-request");

    OcspCfg c;
    c.kind = Kind::Revoked;
    c.reason = OCSP_REVOKED_STATUS_CESSATIONOFOPERATION;
    c.revokedAgoSec = 3600L * 48;
    env.transport->setOcsp(c);
    OcspEvidence revoked = ocspQuery(env);
    if (!revoked.verified) std::fprintf(stderr, "    revoked: %s\n", failureText(revoked).c_str());
    CHECK(revoked.verified);
    CHECK(revoked.status == EvidenceCertStatus::Revoked);
    CHECK(revoked.revocationReason.has_value() && *revoked.revocationReason == 5);
    CHECK(revoked.revocationTime.has_value());
    if (revoked.revocationTime) {
        const auto ago = Clock::now() - *revoked.revocationTime;
        CHECK(ago > std::chrono::hours(47) && ago < std::chrono::hours(49));
    }

    c.kind = Kind::Unknown;
    env.transport->setOcsp(c);
    OcspEvidence unknown = ocspQuery(env);
    if (!unknown.verified) std::fprintf(stderr, "    unknown: %s\n", failureText(unknown).c_str());
    CHECK(unknown.verified); // a signed "unknown" is genuine evidence...
    CHECK(unknown.status == EvidenceCertStatus::Unknown); // ...but never a "good"
}

RIVET_TEST(ocspDelegatedResponder) {
    Env env;
    OcspCfg c;
    c.signer = &env.pki->responder;
    OcspEvidence byName = ocspQuery(env);
    env.transport->setOcsp(c);
    byName = ocspQuery(env);
    if (!byName.verified) std::fprintf(stderr, "    delegated byName: %s\n", failureText(byName).c_str());
    CHECK(byName.verified && byName.status == EvidenceCertStatus::Good);
    CHECK(byName.responderName.find("Delegated OCSP Responder") != std::string::npos);
    c.byKey = true;
    env.transport->setOcsp(c);
    OcspEvidence byKey = ocspQuery(env);
    if (!byKey.verified) std::fprintf(stderr, "    delegated byKey: %s\n", failureText(byKey).c_str());
    CHECK(byKey.verified && byKey.status == EvidenceCertStatus::Good);
    // Revoked through a delegated responder too.
    c.kind = Kind::Revoked;
    env.transport->setOcsp(c);
    OcspEvidence rev = ocspQuery(env);
    CHECK(rev.verified && rev.status == EvidenceCertStatus::Revoked);
}

RIVET_TEST(ocspRejectedResponses) {
    Env env;
    auto with = [&](auto&& tweak) {
        OcspCfg c;
        tweak(c);
        env.transport->setOcsp(c);
        return ocspQuery(env);
    };
    CHECK(rejectedWith(with([](OcspCfg& c) { c.thisUpdateOffsetSec = -7200; c.nextUpdateOffsetSec = -3600; }), EvidenceFailure::Stale, "stale"));
    CHECK(rejectedWith(with([](OcspCfg& c) { c.thisUpdateOffsetSec = 3600; }), EvidenceFailure::NotYetValid, "thisUpdate in the future"));
    // 2 minutes ahead is within the 5 minute tolerance.
    OcspEvidence slight = with([](OcspCfg& c) { c.thisUpdateOffsetSec = 120; });
    if (!slight.verified) std::fprintf(stderr, "    slight skew: %s\n", failureText(slight).c_str());
    CHECK(slight.verified);
    CHECK(rejectedWith(with([](OcspCfg& c) { c.nonce = NonceMode::Wrong; }), EvidenceFailure::NonceMismatch, "wrong nonce"));
    CHECK(rejectedWith(with([](OcspCfg& c) { c.nonce = NonceMode::Omit; }), EvidenceFailure::NonceMismatch, "missing nonce"));
    // Without a nonce request a missing nonce is fine.
    {
        OcspCfg c;
        c.nonce = NonceMode::Omit;
        env.transport->setOcsp(c);
        OcspQueryOptions o;
        o.nonce = false;
        OcspEvidence e = ocspQuery(env, o);
        if (!e.verified) std::fprintf(stderr, "    no nonce asked: %s\n", failureText(e).c_str());
        CHECK(e.verified && !e.nonceChecked);
    }
    OcspEvidence stranger = with([&](OcspCfg& c) { c.signer = &env.pki->stranger; });
    CHECK(!stranger.verified);
    CHECK(stranger.failure == EvidenceFailure::UnauthorizedResponder || stranger.failure == EvidenceFailure::ResponderNotIssuedByCa);
    OcspEvidence otherCa = with([&](OcspCfg& c) { c.signer = &env.pki->otherResponder; });
    CHECK(!otherCa.verified);
    CHECK(otherCa.failure == EvidenceFailure::ResponderNotIssuedByCa || otherCa.failure == EvidenceFailure::UnauthorizedResponder);
    CHECK(rejectedWith(with([&](OcspCfg& c) { c.signer = &env.pki->responderNoEku; }), EvidenceFailure::ResponderNoOcspSigning, "no OCSPSigning EKU"));
    // Delegated responder whose certificate is not sent: the CA's key does not verify its signature.
    OcspEvidence missing = with([&](OcspCfg& c) { c.signer = &env.pki->responder; c.includeSignerCert = false; });
    CHECK(!missing.verified);
    // The leaf itself must not be accepted as a responder for its own status.
    OcspEvidence self = with([&](OcspCfg& c) { c.signer = &env.pki->leaf; });
    CHECK(!self.verified);
    // A response about another certificate is not an answer for this one.
    {
        OcspCfg c;
        env.transport->setOcsp(c);
        OcspClient client(env.network, env.pki->provider);
        auto req = buildOcspRequest(*env.pki->provider, env.pki->leaf2Info, env.pki->caInfo);
        CHECK(req.has_value());
        Bytes resp = makeOcspResponse(*env.pki, c, req->der);
        OcspEvidence e = verifyOcspResponse(resp, *env.pki->provider, env.pki->leafInfo, env.pki->caInfo, std::nullopt, EvidenceClock{});
        CHECK(rejectedWith(e, EvidenceFailure::CertIdMismatch, "other certificate's response"));
    }
    // A responder certificate that expired before producedAt cannot vouch.
    {
        CertSpec rs;
        rs.cn = "Expired Responder";
        rs.keyKind = "ec256";
        rs.issuer = &env.pki->ca;
        rs.extKeyUsage = "OCSPSigning";
        rs.notBeforeOffsetSec = -3600L * 48;
        rs.notAfterOffsetSec = -3600L * 24;
        TestKey expired = makeCert(rs);
        OcspEvidence e = with([&](OcspCfg& c) { c.signer = &expired; });
        CHECK(rejectedWith(e, EvidenceFailure::ResponderCertInvalid, "responder certificate expired"));
    }
}

RIVET_TEST(ocspResponderStatusesAndSha1Fallback) {
    struct Case { int status; OcspResponseStatus expect; const char* name; };
    for (const Case& k : {Case{OCSP_RESPONSE_STATUS_TRYLATER, OcspResponseStatus::TryLater, "tryLater"},
                          Case{OCSP_RESPONSE_STATUS_UNAUTHORIZED, OcspResponseStatus::Unauthorized, "unauthorized"},
                          Case{OCSP_RESPONSE_STATUS_SIGREQUIRED, OcspResponseStatus::SigRequired, "sigRequired"},
                          Case{OCSP_RESPONSE_STATUS_INTERNALERROR, OcspResponseStatus::InternalError, "internalError"},
                          Case{OCSP_RESPONSE_STATUS_MALFORMEDREQUEST, OcspResponseStatus::MalformedRequest, "malformedRequest"}}) {
        Env env;
        OcspCfg c;
        c.responseStatus = k.status;
        env.transport->setOcsp(c);
        OcspEvidence e = ocspQuery(env);
        std::fprintf(stderr, "    %s -> status %s failure %s calls %d\n", k.name, std::string(toString(e.responseStatus)).c_str(),
                     std::string(toString(e.failure)).c_str(), env.transport->ocspCalls.load());
        CHECK(!e.verified);
        CHECK(e.responseStatus == k.expect);
        CHECK(e.failure == EvidenceFailure::ResponderRejected);
        const bool retried = k.status == OCSP_RESPONSE_STATUS_MALFORMEDREQUEST || k.status == OCSP_RESPONSE_STATUS_UNAUTHORIZED;
        CHECK_EQ(env.transport->ocspCalls.load(), retried ? 2 : 1); // SHA-1 retry only for those two
    }
    // A SHA-1-only responder: malformedRequest to SHA-256, then success with SHA-1.
    for (int rejectStatus : {OCSP_RESPONSE_STATUS_MALFORMEDREQUEST, OCSP_RESPONSE_STATUS_UNAUTHORIZED}) {
        Env env;
        OcspCfg c;
        c.rejectSha256CertId = true;
        c.rejectStatus = rejectStatus;
        env.transport->setOcsp(c);
        OcspEvidence e = ocspQuery(env);
        if (!e.verified) std::fprintf(stderr, "    sha1 fallback: %s\n", failureText(e).c_str());
        CHECK(e.verified);
        CHECK(e.certIdSha1Fallback);
        CHECK_EQ(env.transport->ocspCalls.load(), 2);
        CHECK_EQ(env.transport->sha1RequestsSeen(), std::size_t{1});
        // The fallback can be switched off.
        Env env2;
        env2.transport->setOcsp(c);
        OcspQueryOptions o;
        o.sha1Fallback = false;
        OcspEvidence strict = ocspQuery(env2, o);
        CHECK(!strict.verified && strict.failure == EvidenceFailure::ResponderRejected);
        CHECK_EQ(env2.transport->ocspCalls.load(), 1);
        CHECK_EQ(env2.transport->sha1RequestsSeen(), std::size_t{0});
    }
}

RIVET_TEST(ocspTransportFailures) {
    Env env;
    OcspClient client(env.network, env.pki->provider);
    auto query = [&](Env& e) {
        OcspClient c(e.network, e.pki->provider);
        return c.query(kOcspUrl, e.pki->leafInfo, e.pki->caInfo, {}, CancelToken{});
    };
    env.transport->ocspHttpStatus = 500;
    auto r500 = query(env);
    if (r500) std::fprintf(stderr, "    HTTP 500 unexpectedly gave evidence (verified=%d)\n", r500->verified ? 1 : 0);
    CHECK(!r500.has_value());
    if (!r500) CHECK(r500.error().code == CryptoErrorCode::BadResponse || r500.error().code == CryptoErrorCode::Network);
    env.transport->ocspHttpStatus = 200;
    env.transport->ocspContentType = "text/html";
    auto rType = query(env);
    CHECK(!rType.has_value());
    if (!rType) CHECK(rType.error().code == CryptoErrorCode::BadResponse);
    env.transport->ocspContentType = "application/ocsp-response; charset=binary"; // parameters are tolerated
    auto rParam = query(env);
    CHECK(rParam.has_value() && rParam->verified);

    // Garbage under the right headers is a rejected response, not a crash.
    OcspCfg junk;
    junk.rawBody = std::string("this is not DER");
    env.transport->setOcsp(junk);
    auto rJunk = query(env);
    CHECK(rJunk.has_value() && !rJunk->verified && rJunk->failure == EvidenceFailure::Malformed);

    // Size cap (NetworkPolicy and the client's own limit) and timeout through the mock.
    auto mock = std::make_shared<net::MockTransport>();
    net::MockReply big;
    big.headers.emplace_back("Content-Type", "application/ocsp-response");
    big.body.assign(8192, 'x');
    mock->script(kOcspUrl, big);
    net::NetworkPolicy small;
    small.allowHttp = true;
    small.maxResponseBytes = 1024;
    OcspClient smallClient(std::make_shared<net::NetworkClient>(small, mock, env.gate), env.pki->provider);
    auto rBig = smallClient.query(kOcspUrl, env.pki->leafInfo, env.pki->caInfo, {}, CancelToken{});
    CHECK(!rBig.has_value() && rBig.error().code == CryptoErrorCode::Network);
    net::NetworkPolicy loose;
    loose.allowHttp = true;
    OcspClient bigClient(std::make_shared<net::NetworkClient>(loose, mock, env.gate), env.pki->provider);
    OcspQueryOptions capped;
    capped.maxResponseBytes = 4096;
    auto rBig2 = bigClient.query(kOcspUrl, env.pki->leafInfo, env.pki->caInfo, capped, CancelToken{});
    CHECK(!rBig2.has_value() && rBig2.error().code == CryptoErrorCode::Network);

    auto slow = std::make_shared<net::MockTransport>();
    net::MockReply late;
    late.latency = std::chrono::seconds(60);
    late.headers.emplace_back("Content-Type", "application/ocsp-response");
    slow->script(kOcspUrl, late);
    net::NetworkPolicy quick;
    quick.allowHttp = true;
    quick.totalTimeout = std::chrono::milliseconds(200);
    OcspClient slowClient(std::make_shared<net::NetworkClient>(quick, slow, env.gate), env.pki->provider);
    auto rSlow = slowClient.query(kOcspUrl, env.pki->leafInfo, env.pki->caInfo, {}, CancelToken{});
    CHECK(!rSlow.has_value() && rSlow.error().code == CryptoErrorCode::Network);
    if (!rSlow) CHECK(rSlow.error().message.find("Timeout") != std::string::npos);

    // Cancellation is honoured before any traffic.
    CancelToken cancelled;
    cancelled.cancel();
    const int before = env.transport->calls.load();
    auto rCancel = client.query(kOcspUrl, env.pki->leafInfo, env.pki->caInfo, {}, cancelled);
    CHECK(!rCancel.has_value() && rCancel.error().code == CryptoErrorCode::Cancelled);
    CHECK_EQ(env.transport->calls.load(), before);
}

// ---- CRL ------------------------------------------------------------------------------

RIVET_TEST(crlGoodAndRevoked) {
    Env env; // the CRL lists env.pki->leaf
    CrlClient client(env.network, env.pki->provider);
    CrlEvidence rev = crlFetch(env, client);
    if (!rev.verified) std::fprintf(stderr, "    revoked leaf: %s\n", failureText(rev).c_str());
    CHECK(rev.verified);
    CHECK(rev.status == EvidenceCertStatus::Revoked);
    CHECK(rev.revocationReason.has_value() && *rev.revocationReason == 1);
    CHECK(rev.revocationTime.has_value());
    CHECK(!rev.onHold);
    CHECK(!rev.crlNumber.empty());
    CHECK(rev.nextUpdate.has_value());
    CHECK(!rev.rawCrlDer.empty());
    CHECK(!rev.fromCache);

    // leaf2 is not listed: good.
    auto ext = parseRevocationExtensions(env.pki->leaf2Info.der);
    CHECK(ext.has_value());
    auto good = client.fetch(kCrlUrl, env.pki->leaf2Info, env.pki->caInfo, *ext, {}, CancelToken{});
    CHECK(good.has_value() && good->verified && good->status == EvidenceCertStatus::Good);

    // certificateHold is revoked-with-hold, never good.
    Env hold;
    CrlCfg h;
    h.revoked = {&hold.pki->leaf};
    h.reason = 6;
    hold.transport->setCrl(makeCrl(*hold.pki, h));
    CrlClient holdClient(hold.network, hold.pki->provider);
    CrlEvidence held = crlFetch(hold, holdClient);
    CHECK(held.verified && held.status == EvidenceCertStatus::Revoked && held.onHold);
    // No reasonCode extension: revoked, reason absent.
    h.reason = -1;
    hold.transport->setCrl(makeCrl(*hold.pki, h));
    CrlClient noReason(hold.network, hold.pki->provider);
    CrlEvidence nr = crlFetch(hold, noReason);
    CHECK(nr.verified && nr.status == EvidenceCertStatus::Revoked && !nr.revocationReason.has_value());
}

RIVET_TEST(crlRejectedAndTolerated) {
    Env env;
    auto fetchWith = [&](const CrlCfg& cfg, const char* contentType = "application/pkix-crl") {
        env.transport->setCrl(makeCrl(*env.pki, cfg));
        env.transport->crlContentType = contentType;
        CrlClient client(env.network, env.pki->provider); // fresh cache each time
        return crlFetch(env, client);
    };
    CrlCfg base;
    base.revoked = {&env.pki->leaf2};

    CrlCfg wrongIssuer = base;
    wrongIssuer.issuer = &env.pki->otherCa;
    CHECK(rejectedWith(fetchWith(wrongIssuer), EvidenceFailure::IssuerMismatch, "wrong issuer"));

    CrlCfg badSig = base;
    badSig.corruptSignature = true;
    CHECK(rejectedWith(fetchWith(badSig), EvidenceFailure::SignatureInvalid, "bad signature"));

    CrlCfg expired = base;
    expired.thisUpdateOffsetSec = -3600L * 48;
    expired.nextUpdateOffsetSec = -3600L * 24;
    CHECK(rejectedWith(fetchWith(expired), EvidenceFailure::Stale, "expired"));

    CrlCfg future = base;
    future.thisUpdateOffsetSec = 3600L * 3;
    CHECK(rejectedWith(fetchWith(future), EvidenceFailure::NotYetValid, "thisUpdate in the future"));

    CrlCfg scope = base;
    scope.onlyCaCerts = true;
    CrlEvidence scoped = fetchWith(scope);
    CHECK(rejectedWith(scoped, EvidenceFailure::NotApplicable, "IDP scope mismatch"));
    CHECK(!scoped.applicable);

    CrlCfg critical = base;
    critical.criticalUnknownExt = true;
    CHECK(rejectedWith(fetchWith(critical), EvidenceFailure::UnsupportedFeature, "unsupported critical extension"));

    CrlCfg delta = base;
    delta.deltaIndicator = true;
    CHECK(rejectedWith(fetchWith(delta), EvidenceFailure::UnsupportedFeature, "delta CRL"));

    // Tolerated: any content type, PEM encoding.
    CrlEvidence odd = fetchWith(base, "text/plain");
    CHECK(odd.verified);
    Bytes der = makeCrl(*env.pki, base);
    BIO* bio = BIO_new(BIO_s_mem());
    const unsigned char* p = der.data();
    X509_CRL* parsed = d2i_X509_CRL(nullptr, &p, static_cast<long>(der.size()));
    PEM_write_bio_X509_CRL(bio, parsed);
    X509_CRL_free(parsed);
    char* mem = nullptr;
    const long n = BIO_get_mem_data(bio, &mem);
    env.transport->setCrl(Bytes(mem, mem + n));
    BIO_free(bio);
    CrlClient pemClient(env.network, env.pki->provider);
    CrlEvidence pem = crlFetch(env, pemClient);
    if (!pem.verified) std::fprintf(stderr, "    PEM CRL: %s\n", failureText(pem).c_str());
    CHECK(pem.verified && pem.status == EvidenceCertStatus::Good);
    CHECK(pem.rawCrlDer == der); // stored as DER

    // HTTP failure and garbage.
    env.transport->crlHttpStatus = 503;
    CrlClient failing(env.network, env.pki->provider);
    auto ext = parseRevocationExtensions(env.pki->leafInfo.der);
    auto r503 = failing.fetch(kCrlUrl, env.pki->leafInfo, env.pki->caInfo, *ext, {}, CancelToken{});
    CHECK(!r503.has_value());
    env.transport->crlHttpStatus = 200;
    env.transport->setCrl(bytesOf("definitely not a CRL"));
    CrlClient junk(env.network, env.pki->provider);
    CrlEvidence j = crlFetch(env, junk);
    CHECK(!j.verified && j.failure == EvidenceFailure::Malformed);
    // Oversize.
    env.transport->setCrl(Bytes(2048, 0x30));
    CrlClient oversize(env.network, env.pki->provider);
    CrlFetchOptions small;
    small.maxResponseBytes = 1024;
    auto big = oversize.fetch(kCrlUrl, env.pki->leafInfo, env.pki->caInfo, *ext, small, CancelToken{});
    CHECK(!big.has_value() && big.error().code == CryptoErrorCode::Network);
}

RIVET_TEST(crlCacheIsHonouredAndBounded) {
    Env env;
    CrlClient client(env.network, env.pki->provider);
    CrlEvidence first = crlFetch(env, client);
    CHECK(first.verified && !first.fromCache);
    CrlEvidence second = crlFetch(env, client);
    CHECK(second.verified && second.fromCache);
    CHECK_EQ(env.transport->crlCalls.load(), 1);
    CrlFetchOptions nocache;
    nocache.useCache = false;
    CrlEvidence third = crlFetch(env, client, nocache);
    CHECK(third.verified && !third.fromCache);
    CHECK_EQ(env.transport->crlCalls.load(), 2);
    // A CRL past its nextUpdate is neither served from the cache nor accepted live.
    CrlFetchOptions later;
    later.clock.now = Clock::now() + std::chrono::hours(24 * 30);
    later.clock.validationTime = later.clock.now;
    CrlEvidence stale = crlFetch(env, client, later);
    CHECK(!stale.verified && stale.failure == EvidenceFailure::Stale);
    CHECK_EQ(env.transport->crlCalls.load(), 3);
    // Rejected CRLs are never cached.
    Env bad;
    CrlCfg corrupt;
    corrupt.revoked = {&bad.pki->leaf2};
    corrupt.corruptSignature = true;
    bad.transport->setCrl(makeCrl(*bad.pki, corrupt));
    CrlClient badClient(bad.network, bad.pki->provider);
    crlFetch(bad, badClient);
    crlFetch(bad, badClient);
    CHECK_EQ(bad.transport->crlCalls.load(), 2);
    CHECK_EQ(badClient.cache()->size(), std::size_t{0});

    // Bounds: 32 entries / 16 MiB.
    CrlCache cache;
    const auto expiry = Clock::now() + std::chrono::hours(1);
    for (int i = 0; i < 50; ++i) cache.insert("http://x/" + std::to_string(i), Bytes(100, 1), expiry);
    CHECK_EQ(cache.size(), std::size_t{32});
    CHECK(!cache.find("http://x/0", Clock::now()).has_value()); // oldest evicted
    CHECK(cache.find("http://x/49", Clock::now()).has_value());
    CrlCache bytesCache;
    for (int i = 0; i < 6; ++i) bytesCache.insert("http://big/" + std::to_string(i), Bytes(std::size_t{4} << 20, 1), expiry);
    CHECK(bytesCache.totalBytes() <= (std::size_t{16} << 20));
    CHECK(bytesCache.size() <= 4);
    bytesCache.insert("http://huge/", Bytes(std::size_t{17} << 20, 1), expiry);
    CHECK(!bytesCache.find("http://huge/", Clock::now()).has_value());
    CHECK(!cache.find("http://x/49", expiry + std::chrono::hours(1)).has_value()); // expired
    // Thread safety (meaningful under TSan / ASan).
    CrlCache shared;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&shared, t, expiry] {
            for (int i = 0; i < 300; ++i) {
                const std::string url = "http://t/" + std::to_string((t * 7 + i) % 40);
                shared.insert(url, Bytes(64, 2), expiry);
                shared.find(url, Clock::now());
                if (i % 97 == 0) shared.clear();
            }
        });
    for (auto& th : threads) th.join();
    CHECK(shared.size() <= 32);
}

// ---- checker: verdicts -------------------------------------------------------------------

RIVET_TEST(checkerOcspVerdicts) {
    Env env;
    auto out = runCheck(env);
    dump(out);
    CHECK(out.status == RevocationStatus::Good);
    CHECK(out.certs.size() == 1 && out.certs[0].source == RevocationSource::Ocsp);
    CHECK(out.certs[0].acceptedOcsp.has_value());
    CHECK(out.acceptable());
    CHECK_EQ(env.transport->crlCalls.load(), 0); // OCSP answered: the CRL was not needed

    OcspCfg c;
    c.kind = Kind::Revoked;
    c.reason = OCSP_REVOKED_STATUS_KEYCOMPROMISE;
    env.transport->setOcsp(c);
    auto rev = runCheck(env);
    CHECK(rev.status == RevocationStatus::Revoked);
    CHECK(!rev.acceptable());
    CHECK(rev.certs[0].revokedAt.has_value() && rev.certs[0].reasonCode.has_value() && *rev.certs[0].reasonCode == 1);

    // OCSP "unknown" falls through to the CRL (here: lists the leaf as revoked).
    c.kind = Kind::Unknown;
    env.transport->setOcsp(c);
    auto viaCrl = runCheck(env);
    dump(viaCrl);
    CHECK(viaCrl.status == RevocationStatus::Revoked);
    CHECK(viaCrl.certs[0].source == RevocationSource::Crl);
}

RIVET_TEST(checkerUnknownIsNeverGood) {
    Env env;
    OcspCfg c;
    c.kind = Kind::Unknown;
    env.transport->setOcsp(c);
    RevocationPolicy ocspOnly;
    ocspOnly.mode = RevocationMode::OcspOnly;
    auto soft = runCheck(env, ocspOnly);
    dump(soft);
    CHECK(soft.status == RevocationStatus::Unknown);
    CHECK(soft.certs[0].status == RevocationStatus::Unknown && !soft.certs[0].unknownReason.empty());
    CHECK(soft.acceptable()); // soft fail: reported, not blocking
    ocspOnly.hardFail = true;
    auto hard = runCheck(env, ocspOnly);
    CHECK(hard.status == RevocationStatus::Unknown && !hard.acceptable());

    // Unusable evidence (stale, bad nonce, wrong signer) is Unknown, with the reason recorded.
    for (int variant = 0; variant < 3; ++variant) {
        OcspCfg bad;
        if (variant == 0) { bad.thisUpdateOffsetSec = -7200; bad.nextUpdateOffsetSec = -3600; }
        if (variant == 1) bad.nonce = NonceMode::Wrong;
        if (variant == 2) bad.signer = &env.pki->stranger;
        env.transport->setOcsp(bad);
        RevocationPolicy p;
        p.mode = RevocationMode::OcspOnly;
        auto o = runCheck(env, p);
        CHECK(o.status == RevocationStatus::Unknown);
        CHECK(!o.certs[0].ocsp.empty() && !o.certs[0].ocsp[0].verified);
        CHECK(!o.certs[0].unknownReason.empty());
    }
    // RequireAny fails closed even when the caller left hardFail false.
    OcspCfg stale;
    stale.thisUpdateOffsetSec = -7200;
    stale.nextUpdateOffsetSec = -3600;
    env.transport->setOcsp(stale);
    env.transport->setCrl(bytesOf("junk"));
    RevocationPolicy req;
    req.mode = RevocationMode::RequireAny;
    auto closed = runCheck(env, req);
    dump(closed);
    CHECK(closed.status == RevocationStatus::Unknown && closed.hardFail && !closed.acceptable());
}

RIVET_TEST(checkerModes) {
    // CRL fallback when OCSP is down.
    {
        Env env;
        env.transport->ocspHttpStatus = 500;
        CrlCfg c;
        c.revoked = {&env.pki->leaf2};
        env.transport->setCrl(makeCrl(*env.pki, c));
        auto out = runCheck(env);
        dump(out);
        CHECK(out.status == RevocationStatus::Good && out.certs[0].source == RevocationSource::Crl);
        CHECK(env.transport->ocspCalls.load() >= 1 && env.transport->crlCalls.load() == 1);
        RevocationPolicy ocspOnly;
        ocspOnly.mode = RevocationMode::OcspOnly;
        auto o = runCheck(env, ocspOnly);
        CHECK(o.status == RevocationStatus::Unknown);
    }
    // CrlOnly never asks the responder.
    {
        Env env;
        RevocationPolicy p;
        p.mode = RevocationMode::CrlOnly;
        auto out = runCheck(env, p);
        CHECK(out.status == RevocationStatus::Revoked); // the default CRL lists the leaf
        CHECK_EQ(env.transport->ocspCalls.load(), 0);
        auto good = runCheck(env, p, std::nullopt, nullptr);
        (void)good;
        const auto chain2 = env.pki->chain2();
        auto g2 = runCheck(env, p, std::nullopt, &chain2);
        CHECK(g2.status == RevocationStatus::Good);
    }
    // Off: zero traffic, NotChecked.
    {
        Env env;
        RevocationPolicy p;
        p.mode = RevocationMode::Off;
        auto out = runCheck(env, p);
        CHECK(out.status == RevocationStatus::NotChecked && out.acceptable());
        CHECK_EQ(env.transport->calls.load(), 0);
    }
    // Chain of three: intermediate and leaf are both checked.
    {
        Env env;
        CertSpec is;
        is.cn = "Intermediate";
        is.keyKind = "ec256";
        is.ca = true;
        is.issuer = &env.pki->ca;
        is.extraExt = {{NID_info_access, std::string("OCSP;URI:") + kOcspUrl}};
        TestKey inter = makeCert(is);
        CertSpec ls;
        ls.cn = "Deep Leaf";
        ls.keyKind = "ec256";
        ls.issuer = &inter;
        ls.extraExt = {{NID_info_access, std::string("OCSP;URI:") + kOcspUrl}};
        TestKey deep = makeCert(ls);
        std::vector<CertificateInfo> chain{*env.pki->provider->parseCertificate(certDer(deep)),
                                           *env.pki->provider->parseCertificate(certDer(inter)), env.pki->caInfo};
        // The test responder signs with the CA; the leaf's answer must come from its issuer (the intermediate), so
        // it is rejected, while the intermediate's own status (signed by the CA) is good.
        RevocationPolicy p;
        p.mode = RevocationMode::OcspOnly;
        auto out = runCheck(env, p, std::nullopt, &chain);
        dump(out);
        CHECK_EQ(out.certs.size(), std::size_t{2});
        CHECK(out.certs[1].status == RevocationStatus::Good);
        CHECK(out.certs[0].status == RevocationStatus::Unknown);
        CHECK(out.status == RevocationStatus::Unknown);
    }
}

RIVET_TEST(checkerHistoricalEvidence) {
    Env env;
    const auto signedAt = Clock::now() - std::chrono::hours(24 * 10);
    RevocationPolicy ocspOnly;
    ocspOnly.mode = RevocationMode::OcspOnly;
    // Fresh "good" response proves the certificate was not revoked 10 days ago.
    auto good = runCheck(env, ocspOnly, signedAt);
    dump(good);
    CHECK(good.status == RevocationStatus::Good);
    // Revoked 5 days ago: after the signing time, so the signature stands.
    OcspCfg c;
    c.kind = Kind::Revoked;
    c.revokedAgoSec = 3600L * 24 * 5;
    env.transport->setOcsp(c);
    auto later = runCheck(env, ocspOnly, signedAt);
    dump(later);
    CHECK(later.status == RevocationStatus::Good);
    // Revoked before the signing time: revoked.
    c.revokedAgoSec = 3600L * 24 * 20;
    env.transport->setOcsp(c);
    auto before = runCheck(env, ocspOnly, signedAt);
    CHECK(before.status == RevocationStatus::Revoked);
    // The same later revocation seen with live validation is, of course, revoked.
    c.revokedAgoSec = 3600L * 24 * 5;
    env.transport->setOcsp(c);
    auto live = runCheck(env, ocspOnly);
    CHECK(live.status == RevocationStatus::Revoked);
    // Evidence that ended before the signing time says nothing about it.
    OcspCfg old;
    old.thisUpdateOffsetSec = -3600L * 24 * 30;
    old.nextUpdateOffsetSec = -3600L * 24 * 25;
    env.transport->setOcsp(old);
    auto stale = runCheck(env, ocspOnly, signedAt);
    CHECK(stale.status == RevocationStatus::Unknown);
    // A CRL published after signing that lists the certificate as revoked only after signing.
    Env crlEnv;
    CrlCfg cc;
    cc.revoked = {&crlEnv.pki->leaf};
    cc.revokedAgoSec = 3600L * 24 * 3;
    crlEnv.transport->setCrl(makeCrl(*crlEnv.pki, cc));
    RevocationPolicy crlOnly;
    crlOnly.mode = RevocationMode::CrlOnly;
    auto crlLater = runCheck(crlEnv, crlOnly, signedAt);
    dump(crlLater);
    CHECK(crlLater.status == RevocationStatus::Good);
}

// ---- URLs, gates, SSRF ----------------------------------------------------------------------

RIVET_TEST(checkerSkipsUnsafeSchemes) {
    auto pki = std::make_shared<Pki>("ec256", "OCSP;URI:ldap://dir.example.com/ocsp,OCSP;URI:file:///etc/passwd",
                                      "URI:ldap://dir.example.com/cn=ca,URI:file:///etc/crl.pem");
    Env env(pki);
    auto out = runCheck(env);
    dump(out);
    // The http responder from the base certificate is still used; the others are skipped with a note.
    std::size_t skipped = 0;
    for (const std::string& n : out.certs[0].notes)
        if (n.find("skipped") != std::string::npos) ++skipped;
    CHECK(skipped >= 2);
    for (const auto& rq : env.transport->requests) CHECK(rq.url.scheme == "http" || rq.url.scheme == "https");
    for (const auto& rq : env.transport->requests) CHECK(rq.url.host == "ocsp.example.com" || rq.url.host == "crl.example.com");

    // Only unusable URLs: Unknown, zero traffic.
    CertSpec ls;
    ls.cn = "Only LDAP";
    ls.keyKind = "ec256";
    ls.issuer = &pki->ca;
    ls.extraExt = {{NID_info_access, "OCSP;URI:ldap://dir.example.com/ocsp"}, {NID_crl_distribution_points, "URI:file:///etc/crl.pem"}};
    TestKey k = makeCert(ls);
    Env env2(pki);
    std::vector<CertificateInfo> chain{*pki->provider->parseCertificate(certDer(k)), pki->caInfo};
    auto out2 = runCheck(env2, {}, std::nullopt, &chain);
    dump(out2);
    CHECK(out2.status == RevocationStatus::Unknown);
    CHECK_EQ(env2.transport->calls.load(), 0);
}

RIVET_TEST(checkerSsrfAndGate) {
    // AIA / CDP pointing at loopback and the cloud metadata address: refused, zero connections.
    auto pki = std::make_shared<Pki>("ec256", "", "");
    CertSpec ls;
    ls.cn = "Evil Leaf";
    ls.keyKind = "ec256";
    ls.issuer = &pki->ca;
    ls.extraExt = {{NID_info_access, "OCSP;URI:http://127.0.0.1:8080/ocsp,OCSP;URI:http://169.254.169.254/latest/meta-data"},
                   {NID_crl_distribution_points, "URI:http://127.0.0.1/crl,URI:http://169.254.169.254/crl,URI:http://localhost/crl"}};
    TestKey k = makeCert(ls);
    Env env(pki);
    std::vector<CertificateInfo> chain{*pki->provider->parseCertificate(certDer(k)), pki->caInfo};
    auto out = runCheck(env, {}, std::nullopt, &chain);
    dump(out);
    CHECK_EQ(env.transport->calls.load(), 0);
    CHECK(out.status == RevocationStatus::Unknown);
    CHECK(!out.certs[0].unknownReason.empty());

    // Gate Off: no traffic at all, Unknown (not good), reason mentions the gate.
    Env off;
    off.gate->setMode(net::OnlineValidationMode::Off);
    auto o = runCheck(off);
    dump(o);
    CHECK_EQ(off.transport->calls.load(), 0);
    CHECK(o.status == RevocationStatus::Unknown);
    // Ask, denied by the default prompt: still no traffic, and the user is not asked once per URL.
    Env ask;
    ask.gate->setMode(net::OnlineValidationMode::Ask);
    auto a = runCheck(ask);
    CHECK_EQ(ask.transport->calls.load(), 0);
    CHECK(a.status == RevocationStatus::Unknown);
    // The network layer being absent behaves like Off.
    RevocationChecker offline(off.pki->provider, nullptr);
    auto none = offline.check(off.pki->chain(), Clock::now(), {}, CancelToken{});
    CHECK(none.status == RevocationStatus::Unknown);
    // Cancelled before the first request.
    Env cancelEnv;
    CancelToken cancelled;
    cancelled.cancel();
    auto checker = makeChecker(cancelEnv);
    auto c = checker.check(cancelEnv.pki->chain(), Clock::now(), {}, cancelled);
    CHECK(c.status == RevocationStatus::Unknown);
    CHECK_EQ(cancelEnv.transport->calls.load(), 0);
}

RIVET_TEST(checkerTimeoutsAreUnknown) {
    Env env;
    auto slow = std::make_shared<net::MockTransport>();
    net::MockReply late;
    late.latency = std::chrono::seconds(60);
    slow->script(kOcspUrl, late);
    slow->script(kCrlUrl, late);
    net::NetworkPolicy quick;
    quick.allowHttp = true;
    quick.totalTimeout = std::chrono::milliseconds(150);
    RevocationChecker checker(env.pki->provider, std::make_shared<net::NetworkClient>(quick, slow, env.gate));
    auto out = checker.check(env.pki->chain(), Clock::now(), {}, CancelToken{});
    dump(out);
    CHECK(out.status == RevocationStatus::Unknown);
    CHECK(slow->callCount() >= 2);
}

// ---- offline evidence ----------------------------------------------------------------------------

RIVET_TEST(checkerOfflineStoreNeedsNoNetwork) {
    Env env;
    // Gather evidence "online" first.
    auto online = runCheck(env);
    CHECK(online.status == RevocationStatus::Good && !online.certs[0].ocsp.empty());
    auto store = std::make_shared<VectorEvidenceStore>();
    store->addOcsp(online.certs[0].ocsp[0].rawResponseDer);
    CrlCfg c;
    c.revoked = {&env.pki->leaf};
    store->addCrl(makeCrl(*env.pki, c));

    Env offline(env.pki); // a transport that must stay silent
    RevocationPolicy p;
    p.allowNetwork = false;
    p.store = store;
    auto out = runCheck(offline, p);
    dump(out);
    CHECK(out.status == RevocationStatus::Good);
    CHECK(out.certs[0].source == RevocationSource::Ocsp);
    CHECK_EQ(offline.transport->calls.load(), 0);

    // CRL only, from the store.
    auto crlStore = std::make_shared<VectorEvidenceStore>();
    crlStore->addCrl(makeCrl(*env.pki, c));
    p.store = crlStore;
    p.mode = RevocationMode::CrlOnly;
    auto revoked = runCheck(offline, p);
    CHECK(revoked.status == RevocationStatus::Revoked);
    CHECK_EQ(offline.transport->calls.load(), 0);

    // Empty store, network off: Unknown, still no traffic.
    p.store = std::make_shared<VectorEvidenceStore>();
    p.mode = RevocationMode::PreferOcspThenCrl;
    auto empty = runCheck(offline, p);
    CHECK(empty.status == RevocationStatus::Unknown);
    CHECK_EQ(offline.transport->calls.load(), 0);

    // Evidence for another certificate does not vouch for this one.
    Env other(env.pki);
    auto good2 = [&] {
        RevocationPolicy q;
        const auto chain2 = other.pki->chain2();
        return runCheck(other, q, std::nullopt, &chain2);
    }();
    CHECK(good2.status == RevocationStatus::Good);
    auto foreign = std::make_shared<VectorEvidenceStore>();
    foreign->addOcsp(good2.certs[0].ocsp[0].rawResponseDer);
    p.store = foreign;
    auto mismatch = runCheck(offline, p);
    CHECK(mismatch.status == RevocationStatus::Unknown);

    // Embedded evidence is judged at the validation time like live evidence: expired evidence is unusable "now".
    Env aged(env.pki);
    OcspCfg old;
    old.thisUpdateOffsetSec = -3600L * 24 * 30;
    old.nextUpdateOffsetSec = -3600L * 24 * 25;
    aged.transport->setOcsp(old);
    auto agedStore = std::make_shared<VectorEvidenceStore>();
    {
        auto req = buildOcspRequest(*aged.pki->provider, aged.pki->leafInfo, aged.pki->caInfo);
        agedStore->addOcsp(makeOcspResponse(*aged.pki, old, req->der));
    }
    RevocationPolicy live;
    live.allowNetwork = false;
    live.store = agedStore;
    live.mode = RevocationMode::OcspOnly;
    CHECK(runCheck(offline, live).status == RevocationStatus::Unknown);
    // ...but it speaks for a signing time inside its validity window.
    auto during = runCheck(offline, live, Clock::now() - std::chrono::hours(24 * 27));
    dump(during);
    CHECK(during.status == RevocationStatus::Good);
}

// ---- integration with the validation model -------------------------------------------------------------

RIVET_TEST(applyRevocationIsAdditive) {
    Env env;
    ValidationResult base;
    base.state = SignatureState::ValidTrusted;
    base.crypto = CryptoCheck::Valid;
    base.certificate = CertificateStatus::Valid;
    base.trust = TrustStatus::Trusted;
    base.integrity = IntegrityStatus::Intact;
    CHECK(!base.revocationDetail.has_value()); // default: untouched
    CHECK(foldState(base) == SignatureState::ValidTrusted);

    ValidationResult good = base;
    applyRevocation(good, runCheck(env));
    CHECK(good.revocation == RevocationStatus::Good && good.state == SignatureState::ValidTrusted);
    CHECK(good.revocationDetail.has_value() && good.revocationDetail->status == RevocationStatus::Good);

    OcspCfg c;
    c.kind = Kind::Revoked;
    env.transport->setOcsp(c);
    ValidationResult revoked = base;
    applyRevocation(revoked, runCheck(env));
    CHECK(revoked.revocation == RevocationStatus::Revoked && revoked.state == SignatureState::Revoked);
    CHECK(revoked.revocationDetail.has_value() && revoked.revocationDetail->revokedAt.has_value());

    c.kind = Kind::Unknown;
    env.transport->setOcsp(c);
    RevocationPolicy ocspOnly;
    ocspOnly.mode = RevocationMode::OcspOnly;
    ValidationResult soft = base;
    applyRevocation(soft, runCheck(env, ocspOnly));
    CHECK(soft.revocation == RevocationStatus::Unknown && soft.state == SignatureState::ValidTrusted);
    CHECK(!soft.notes.empty());
    ocspOnly.hardFail = true;
    ValidationResult hard = base;
    applyRevocation(hard, runCheck(env, ocspOnly));
    CHECK(hard.revocation == RevocationStatus::Unknown && hard.state == SignatureState::UnableToValidate);

    // A signature that is already invalid stays invalid whatever revocation says.
    ValidationResult broken = base;
    broken.crypto = CryptoCheck::Invalid;
    broken.state = foldState(broken);
    RevocationOutcome fine;
    fine.status = RevocationStatus::Good;
    applyRevocation(broken, fine);
    CHECK(broken.state == SignatureState::InvalidSignature);
}

namespace {
class QueueDispatcher final : public core::IMainThreadDispatcher {
public:
    void post(std::function<void()> task) override {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_all();
    }
    bool drainOne() {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            if (!cv_.wait_for(lock, std::chrono::seconds(30), [&] { return !queue_.empty(); })) return false;
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
        return true;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
};
} // namespace

RIVET_TEST(checkerAsyncDeliversOnTheMainThread) {
    Env env;
    core::TaskScheduler scheduler(2);
    QueueDispatcher dispatcher;
    core::AsyncScope scope;
    auto checker = makeChecker(env);
    std::optional<RevocationStatus> status;
    std::thread::id doneThread;
    CHECK(checker.checkAsync(scheduler, dispatcher, scope, CancelToken{}, env.pki->chain(), Clock::now(), {},
                             [&](CryptoResult<RevocationOutcome> r) {
                                 doneThread = std::this_thread::get_id();
                                 if (r) status = r->status;
                             }));
    CHECK(dispatcher.drainOne());
    CHECK(status.has_value() && *status == RevocationStatus::Good);
    CHECK(doneThread == std::this_thread::get_id());
    // Cancelled: reported as Cancelled, no traffic.
    CancelToken cancel;
    cancel.cancel();
    const int before = env.transport->calls.load();
    CryptoErrorCode code = CryptoErrorCode::Internal;
    CHECK(checker.checkAsync(scheduler, dispatcher, scope, cancel, env.pki->chain(), Clock::now(), {},
                             [&](CryptoResult<RevocationOutcome> r) { code = r ? CryptoErrorCode::Internal : r.error().code; }));
    CHECK(dispatcher.drainOne());
    CHECK_EQ(code, CryptoErrorCode::Cancelled);
    CHECK_EQ(env.transport->calls.load(), before);
    scope.closeAndWait();
    CHECK(!checker.checkAsync(scheduler, dispatcher, scope, CancelToken{}, env.pki->chain(), Clock::now(), {}, [](CryptoResult<RevocationOutcome>) {}));
}

RIVET_TEST(revocationWithRsaIssuer) {
    auto pki = std::make_shared<Pki>("rsa2048");
    Env env(pki);
    CHECK(runCheck(env).status == RevocationStatus::Good);
    RevocationPolicy crl;
    crl.mode = RevocationMode::CrlOnly;
    CHECK(runCheck(env, crl).status == RevocationStatus::Revoked);
}

// ---- fuzz-ish ----------------------------------------------------------------------------------------

RIVET_TEST(mutatedEvidenceNeverCrashes) {
    Env env;
    auto req = buildOcspRequest(*env.pki->provider, env.pki->leafInfo, env.pki->caInfo);
    CHECK(req.has_value());
    OcspCfg delegated;
    delegated.signer = &env.pki->responder;
    const Bytes ocsp = makeOcspResponse(*env.pki, delegated, req->der);
    const Bytes crl = makeCrl(*env.pki, CrlCfg{.revoked = {&env.pki->leaf}});
    EvidenceClock clock;
    // The unmodified inputs verify (so the mutations start from something real).
    CHECK(verifyOcspResponse(ocsp, *env.pki->provider, env.pki->leafInfo, env.pki->caInfo, req->nonce, clock).verified);
    auto ext = parseRevocationExtensions(env.pki->leafInfo.der);
    CHECK(ext.has_value());
    CHECK(verifyCrl(crl, *env.pki->provider, env.pki->leafInfo, env.pki->caInfo, *ext, clock).verified);
    std::size_t accepted = 0;
    for (const Bytes& m : mutations(ocsp, 1234, 200)) {
        OcspEvidence e = verifyOcspResponse(m, *env.pki->provider, env.pki->leafInfo, env.pki->caInfo, req->nonce, clock);
        if (e.verified) ++accepted; // a mutation inside an ignored field may still verify; that is fine
    }
    for (const Bytes& m : mutations(crl, 4321, 200)) {
        CrlEvidence e = verifyCrl(m, *env.pki->provider, env.pki->leafInfo, env.pki->caInfo, *ext, clock);
        if (e.verified) ++accepted;
    }
    std::fprintf(stderr, "    400 mutations, %zu still verified\n", accepted);
    CHECK(accepted < 400);
    // Degenerate inputs.
    for (const Bytes& m : {Bytes{}, Bytes{0x30}, Bytes{0x30, 0x80}, Bytes(1000, 0xFF), Bytes{0x30, 0x03, 0x0A, 0x01, 0x00}}) {
        CHECK(!verifyOcspResponse(m, *env.pki->provider, env.pki->leafInfo, env.pki->caInfo, std::nullopt, clock).verified);
        CHECK(!verifyCrl(m, *env.pki->provider, env.pki->leafInfo, env.pki->caInfo, *ext, clock).verified);
    }
    // Mutated certificate extensions never crash the parser either.
    std::size_t parsed = 0;
    for (const Bytes& m : mutations(env.pki->leafInfo.der, 99, 200))
        if (parseRevocationExtensions(m).has_value()) ++parsed;
    std::fprintf(stderr, "    %zu of 200 mutated certificates still parsed\n", parsed);
}
