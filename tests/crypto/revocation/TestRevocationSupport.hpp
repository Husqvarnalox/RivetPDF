// SPDX-License-Identifier: MPL-2.0
#pragma once

// Test-only PKI, OCSP responder and CRL issuer built directly on OpenSSL, so
// that the code under test is checked against independently produced data.

#include "TestSupport.hpp"
#include "net/Client.hpp"
#include "net/Transport.hpp"

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/ocsp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace rivet::crypto::testing::rev {

constexpr const char* kOcspUrl = "http://ocsp.example.com/ocsp";
constexpr const char* kCrlUrl = "http://crl.example.com/ca.crl";

inline Bytes bytesOf(std::string_view s) { return Bytes(s.begin(), s.end()); }

struct OcspRespDel { void operator()(OCSP_RESPONSE* p) const { OCSP_RESPONSE_free(p); } };
struct OcspReqDel { void operator()(OCSP_REQUEST* p) const { OCSP_REQUEST_free(p); } };
struct BasicDel { void operator()(OCSP_BASICRESP* p) const { OCSP_BASICRESP_free(p); } };
struct CrlDel { void operator()(X509_CRL* p) const { X509_CRL_free(p); } };

// ---- PKI ---------------------------------------------------------------------------

struct Pki {
    std::shared_ptr<CryptoProvider> provider;
    TestKey ca;            // trust anchor, signs everything below
    TestKey leaf;          // revocation URLs point at kOcspUrl / kCrlUrl
    TestKey leaf2;         // a second, never-revoked certificate of the same CA
    TestKey responder;     // delegated OCSP responder (id-kp-OCSPSigning), issued by ca
    TestKey responderNoEku;// issued by ca, no OCSPSigning
    TestKey responderNoNocheck; // issued by ca, OCSPSigning but no id-pkix-ocsp-nocheck
    TestKey otherCa;       // unrelated CA
    TestKey otherResponder;// OCSPSigning, issued by otherCa
    TestKey stranger;      // unrelated self-signed key
    CertificateInfo caInfo, leafInfo, leaf2Info;

    explicit Pki(const std::string& keyKind = "ec256", const std::string& leafExtraAia = {},
                 const std::string& leafExtraCdp = {}) {
        provider = makeDefaultProvider();
        CertSpec caSpec;
        caSpec.cn = "Revocation Test CA";
        caSpec.keyKind = keyKind;
        caSpec.ca = true;
        caSpec.notBeforeOffsetSec = -3600L * 24 * 400;
        ca = makeCert(caSpec);

        CertSpec ls;
        ls.cn = "Revocation Test Leaf";
        ls.keyKind = "ec256";
        ls.issuer = &ca;
        ls.notBeforeOffsetSec = -3600L * 24 * 200;
        std::string aia = std::string("OCSP;URI:") + kOcspUrl;
        if (!leafExtraAia.empty()) aia = leafExtraAia + "," + aia;
        std::string cdp = std::string("URI:") + kCrlUrl;
        if (!leafExtraCdp.empty()) cdp = leafExtraCdp + "," + cdp;
        ls.extraExt = {{NID_info_access, aia}, {NID_crl_distribution_points, cdp}};
        leaf = makeCert(ls);
        ls.cn = "Revocation Test Leaf 2";
        leaf2 = makeCert(ls);

        CertSpec rs;
        rs.cn = "Delegated OCSP Responder";
        rs.keyKind = "ec256";
        rs.issuer = &ca;
        rs.extKeyUsage = "OCSPSigning";
        rs.notBeforeOffsetSec = -3600L * 24 * 100;
        rs.notAfterOffsetSec = 3600L * 24 * 30;
        rs.extraExt = {{NID_id_pkix_OCSP_noCheck, "NULL"}};
        responder = makeCert(rs);
        rs.cn = "Responder Without Nocheck";
        rs.extraExt.clear();
        responderNoNocheck = makeCert(rs);
        rs.cn = "Responder Without EKU";
        rs.extKeyUsage.clear();
        responderNoEku = makeCert(rs);

        CertSpec oc;
        oc.cn = "Other CA";
        oc.keyKind = "ec256";
        oc.ca = true;
        otherCa = makeCert(oc);
        CertSpec orr;
        orr.cn = "Other CA Responder";
        orr.keyKind = "ec256";
        orr.issuer = &otherCa;
        orr.extKeyUsage = "OCSPSigning";
        otherResponder = makeCert(orr);
        CertSpec st;
        st.cn = "Stranger";
        st.keyKind = "ec256";
        stranger = makeCert(st);

        caInfo = *provider->parseCertificate(certDer(ca));
        leafInfo = *provider->parseCertificate(certDer(leaf));
        leaf2Info = *provider->parseCertificate(certDer(leaf2));
    }

    std::vector<CertificateInfo> chain() const { return {leafInfo, caInfo}; }
    std::vector<CertificateInfo> chain2() const { return {leaf2Info, caInfo}; }
};

inline std::string serialHex(const TestKey& k) {
    BIGNUM* bn = ASN1_INTEGER_to_BN(X509_get_serialNumber(k.cert.get()), nullptr);
    char* hex = BN_bn2hex(bn);
    std::string out = hex;
    OPENSSL_free(hex);
    BN_free(bn);
    return out;
}

// ---- OCSP responses ------------------------------------------------------------------

enum class Kind { Good, Revoked, Unknown };
enum class NonceMode { Echo, Omit, Wrong };

struct OcspCfg {
    Kind kind = Kind::Good;
    int reason = OCSP_REVOKED_STATUS_KEYCOMPROMISE;
    long revokedAgoSec = 3600L * 24;      // revocation time = now - this
    long thisUpdateOffsetSec = -60;       // relative to now
    std::optional<long> nextUpdateOffsetSec = 3600L * 24 * 7;
    int responseStatus = OCSP_RESPONSE_STATUS_SUCCESSFUL;
    const TestKey* signer = nullptr;      // null: the CA signs itself (certificate not included)
    bool includeSignerCert = true;        // only meaningful for a delegated / foreign signer
    bool byKey = false;
    NonceMode nonce = NonceMode::Echo;
    bool rejectSha256CertId = false;      // answer malformedRequest to a SHA-256 CertID
    int rejectStatus = OCSP_RESPONSE_STATUS_MALFORMEDREQUEST;
    std::optional<std::string> rawBody;   // replaces the whole body
};

inline Bytes derOf(OCSP_RESPONSE* r) {
    unsigned char* buf = nullptr;
    const int n = i2d_OCSP_RESPONSE(r, &buf);
    Bytes out(buf, buf + n);
    OPENSSL_free(buf);
    return out;
}

inline Bytes statusOnlyResponse(int status) {
    std::unique_ptr<OCSP_RESPONSE, OcspRespDel> r(OCSP_response_create(status, nullptr));
    return derOf(r.get());
}

// Builds the response to `requestDer` as OpenSSL would.
inline Bytes makeOcspResponse(const Pki& pki, const OcspCfg& cfg, ByteView requestDer) {
    const unsigned char* p = requestDer.data();
    std::unique_ptr<OCSP_REQUEST, OcspReqDel> req(d2i_OCSP_REQUEST(nullptr, &p, static_cast<long>(requestDer.size())));
    if (!req) return statusOnlyResponse(OCSP_RESPONSE_STATUS_MALFORMEDREQUEST);
    if (cfg.responseStatus != OCSP_RESPONSE_STATUS_SUCCESSFUL) return statusOnlyResponse(cfg.responseStatus);

    std::unique_ptr<OCSP_BASICRESP, BasicDel> bs(OCSP_BASICRESP_new());
    for (int i = 0; i < OCSP_request_onereq_count(req.get()); ++i) {
        OCSP_CERTID* cid = OCSP_onereq_get0_id(OCSP_request_onereq_get0(req.get(), i));
        ASN1_OBJECT* md = nullptr;
        OCSP_id_get0_info(nullptr, &md, nullptr, nullptr, cid);
        if (cfg.rejectSha256CertId && OBJ_obj2nid(md) == NID_sha256) return statusOnlyResponse(cfg.rejectStatus);
        ASN1_TIME* thisUpd = X509_gmtime_adj(nullptr, cfg.thisUpdateOffsetSec);
        ASN1_TIME* nextUpd = cfg.nextUpdateOffsetSec ? X509_gmtime_adj(nullptr, *cfg.nextUpdateOffsetSec) : nullptr;
        ASN1_TIME* revTime = cfg.kind == Kind::Revoked ? X509_gmtime_adj(nullptr, -cfg.revokedAgoSec) : nullptr;
        const int st = cfg.kind == Kind::Good ? V_OCSP_CERTSTATUS_GOOD
                       : cfg.kind == Kind::Revoked ? V_OCSP_CERTSTATUS_REVOKED
                                                   : V_OCSP_CERTSTATUS_UNKNOWN;
        OCSP_basic_add1_status(bs.get(), cid, st, cfg.kind == Kind::Revoked ? cfg.reason : 0, revTime, thisUpd, nextUpd);
        ASN1_TIME_free(thisUpd);
        if (nextUpd) ASN1_TIME_free(nextUpd);
        if (revTime) ASN1_TIME_free(revTime);
    }
    if (cfg.nonce == NonceMode::Echo) OCSP_copy_nonce(bs.get(), req.get());
    else if (cfg.nonce == NonceMode::Wrong) OCSP_basic_add1_nonce(bs.get(), const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>("not-the-nonce!!!")), 16);

    const TestKey& signer = cfg.signer ? *cfg.signer : pki.ca;
    unsigned long flags = 0;
    if (!cfg.signer || !cfg.includeSignerCert) flags |= OCSP_NOCERTS;
    if (cfg.byKey) flags |= OCSP_RESPID_KEY;
    OCSP_basic_sign(bs.get(), signer.cert.get(), signer.pkey.get(), EVP_sha256(), nullptr, flags);
    std::unique_ptr<OCSP_RESPONSE, OcspRespDel> resp(OCSP_response_create(OCSP_RESPONSE_STATUS_SUCCESSFUL, bs.get()));
    return derOf(resp.get());
}

// ---- CRLs -----------------------------------------------------------------------------

struct CrlCfg {
    const TestKey* issuer = nullptr;      // null: the CA
    std::vector<const TestKey*> revoked;  // serials to list
    int reason = 1;                       // keyCompromise; <0: no reasonCode entry extension
    long revokedAgoSec = 3600L * 24;
    long thisUpdateOffsetSec = -3600;
    std::optional<long> nextUpdateOffsetSec = 3600L * 24 * 7;
    bool onlyCaCerts = false;             // issuingDistributionPoint onlyContainsCACerts
    bool criticalUnknownExt = false;
    bool deltaIndicator = false;
    bool corruptSignature = false;
    int crlNumber = 7;
};

inline Bytes makeCrl(const Pki& pki, const CrlCfg& cfg) {
    const TestKey& issuer = cfg.issuer ? *cfg.issuer : pki.ca;
    std::unique_ptr<X509_CRL, CrlDel> crl(X509_CRL_new());
    X509_CRL_set_version(crl.get(), 1);
    X509_CRL_set_issuer_name(crl.get(), X509_get_subject_name(issuer.cert.get()));
    ASN1_TIME* last = X509_gmtime_adj(nullptr, cfg.thisUpdateOffsetSec);
    X509_CRL_set1_lastUpdate(crl.get(), last);
    ASN1_TIME_free(last);
    if (cfg.nextUpdateOffsetSec) {
        ASN1_TIME* next = X509_gmtime_adj(nullptr, *cfg.nextUpdateOffsetSec);
        X509_CRL_set1_nextUpdate(crl.get(), next);
        ASN1_TIME_free(next);
    }
    for (const TestKey* k : cfg.revoked) {
        X509_REVOKED* r = X509_REVOKED_new();
        X509_REVOKED_set_serialNumber(r, X509_get_serialNumber(k->cert.get()));
        ASN1_TIME* when = X509_gmtime_adj(nullptr, -cfg.revokedAgoSec);
        X509_REVOKED_set_revocationDate(r, when);
        ASN1_TIME_free(when);
        if (cfg.reason >= 0) {
            ASN1_ENUMERATED* e = ASN1_ENUMERATED_new();
            ASN1_ENUMERATED_set(e, cfg.reason);
            X509_REVOKED_add1_ext_i2d(r, NID_crl_reason, e, 0, 0);
            ASN1_ENUMERATED_free(e);
        }
        X509_CRL_add0_revoked(crl.get(), r);
    }
    ASN1_INTEGER* num = ASN1_INTEGER_new();
    ASN1_INTEGER_set(num, cfg.crlNumber);
    X509_CRL_add1_ext_i2d(crl.get(), NID_crl_number, num, 0, 0);
    ASN1_INTEGER_free(num);
    if (cfg.onlyCaCerts) {
        ISSUING_DIST_POINT* idp = ISSUING_DIST_POINT_new();
        idp->onlyCA = 0xFF;
        X509_CRL_add1_ext_i2d(crl.get(), NID_issuing_distribution_point, idp, 1, 0);
        ISSUING_DIST_POINT_free(idp);
    }
    if (cfg.deltaIndicator) {
        ASN1_INTEGER* base = ASN1_INTEGER_new();
        ASN1_INTEGER_set(base, 3);
        X509_CRL_add1_ext_i2d(crl.get(), NID_delta_crl, base, 1, 0);
        ASN1_INTEGER_free(base);
    }
    if (cfg.criticalUnknownExt) {
        ASN1_OBJECT* oid = OBJ_txt2obj("1.3.6.1.4.1.99999.1", 1);
        ASN1_OCTET_STRING* val = ASN1_OCTET_STRING_new();
        const unsigned char nul[2] = {0x05, 0x00};
        ASN1_OCTET_STRING_set(val, nul, 2);
        X509_EXTENSION* ext = X509_EXTENSION_create_by_OBJ(nullptr, oid, 1, val);
        X509_CRL_add_ext(crl.get(), ext, -1);
        X509_EXTENSION_free(ext);
        ASN1_OBJECT_free(oid);
        ASN1_OCTET_STRING_free(val);
    }
    X509_CRL_sort(crl.get());
    X509_CRL_sign(crl.get(), issuer.pkey.get(), EVP_sha256());
    unsigned char* buf = nullptr;
    const int n = i2d_X509_CRL(crl.get(), &buf);
    Bytes out(buf, buf + n);
    OPENSSL_free(buf);
    if (cfg.corruptSignature) out.back() ^= 0x01;
    return out;
}

// ---- transport -------------------------------------------------------------------------

// Serves the OCSP responder URL and the CRL URL (anything else: 404). Every
// call is counted; nothing leaves the process.
class RevTransport final : public net::INetworkTransport {
public:
    explicit RevTransport(std::shared_ptr<Pki> p) : pki(std::move(p)) {}

    core::Result<net::HttpResponse> perform(const net::HttpRequest& request, const net::NetworkPolicy&, net::CancelToken) override {
        ++calls;
        net::HttpResponse r;
        std::string url = request.url.host;
        {
            std::lock_guard lock(mutex);
            lastRequest = request;
            requests.push_back(request);
        }
        if (url == "ocsp.example.com") {
            ++ocspCalls;
            if (ocspSleep.count() > 0) std::this_thread::sleep_for(ocspSleep);
            r.status = ocspHttpStatus;
            r.headers.emplace_back("Content-Type", ocspContentType);
            std::lock_guard lock(mutex);
            if (ocsp.rawBody) {
                r.body = *ocsp.rawBody;
            } else {
                const Bytes body(request.body.begin(), request.body.end());
                const Bytes resp = makeOcspResponse(*pki, ocsp, body);
                r.body.assign(resp.begin(), resp.end());
            }
        } else if (url == "crl.example.com") {
            ++crlCalls;
            r.status = crlHttpStatus;
            r.headers.emplace_back("Content-Type", crlContentType);
            std::lock_guard lock(mutex);
            r.body.assign(crlBody.begin(), crlBody.end());
        } else {
            r.status = 404;
        }
        return r;
    }

    void setOcsp(const OcspCfg& c) { std::lock_guard lock(mutex); ocsp = c; }
    void setCrl(const Bytes& der) { std::lock_guard lock(mutex); crlBody = der; }
    std::size_t sha1RequestsSeen() const {
        std::lock_guard lock(mutex);
        std::size_t n = 0;
        for (const auto& rq : requests) {
            if (rq.url.host != "ocsp.example.com") continue;
            const unsigned char* p = reinterpret_cast<const unsigned char*>(rq.body.data());
            std::unique_ptr<OCSP_REQUEST, OcspReqDel> req(d2i_OCSP_REQUEST(nullptr, &p, static_cast<long>(rq.body.size())));
            if (!req || OCSP_request_onereq_count(req.get()) < 1) continue;
            ASN1_OBJECT* md = nullptr;
            OCSP_id_get0_info(nullptr, &md, nullptr, nullptr, OCSP_onereq_get0_id(OCSP_request_onereq_get0(req.get(), 0)));
            if (OBJ_obj2nid(md) == NID_sha1) ++n;
        }
        return n;
    }

    std::shared_ptr<Pki> pki;
    OcspCfg ocsp;
    Bytes crlBody;
    int ocspHttpStatus = 200;
    int crlHttpStatus = 200;
    std::string ocspContentType = "application/ocsp-response";
    std::string crlContentType = "application/pkix-crl";
    std::chrono::milliseconds ocspSleep{0};
    std::atomic<int> calls{0};
    std::atomic<int> ocspCalls{0};
    std::atomic<int> crlCalls{0};
    net::HttpRequest lastRequest;
    std::vector<net::HttpRequest> requests;
    mutable std::mutex mutex;
};

struct Env {
    std::shared_ptr<Pki> pki;
    std::shared_ptr<RevTransport> transport;
    std::shared_ptr<net::OnlineValidationGate> gate = std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On);
    net::NetworkPolicy policy;
    std::shared_ptr<net::NetworkClient> network;

    explicit Env(std::shared_ptr<Pki> p = nullptr) {
        pki = p ? std::move(p) : std::make_shared<Pki>();
        transport = std::make_shared<RevTransport>(pki);
        policy.allowHttp = true; // OCSP / CRL URLs are http by convention
        CrlCfg crl;
        crl.revoked = {&pki->leaf};
        transport->setCrl(makeCrl(*pki, crl));
        rebuild();
    }
    void rebuild() { network = std::make_shared<net::NetworkClient>(policy, transport, gate); }
};

} // namespace rivet::crypto::testing::rev
