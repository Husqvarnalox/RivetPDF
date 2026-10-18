// SPDX-License-Identifier: MPL-2.0
#pragma once

// Test-only RFC 3161 time-stamp authority: tokens are assembled by hand so that
// every field can be bent (nonce, imprint, policy, status, certificates).
// Shared by the crypto time-stamp tests and the PDF signing tests.

#include "TestSupport.hpp"
#include "crypto/Cms.hpp"
#include "crypto/Der.hpp"
#include "crypto/timestamp/TimeStamp.hpp"
#include "net/Transport.hpp"

#include <openssl/bn.h>
#include <openssl/pem.h>
#include <openssl/ts.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::crypto::testing {

constexpr const char* kTsaUrl = "https://tsa.example.com/tsa"; // test-only; the product has no built-in TSA

inline Bytes bytesOf(std::string_view s) { return Bytes(s.begin(), s.end()); }

// ---- test TSA ---------------------------------------------------------------------

struct Tsa {
    TestKey key;
    bool ec = true;
    CertificateInfo info;
    Bytes certDer;
    std::vector<Bytes> chainDer; // embedded in tokens: leaf first
    TrustAnchorSet anchors;      // what the verifier is told to trust
};

inline std::shared_ptr<Tsa> makeTsa(CryptoProvider& provider, CertSpec spec, const TestKey* issuer = nullptr) {
    auto tsa = std::make_shared<Tsa>();
    spec.issuer = issuer;
    tsa->ec = spec.keyKind.rfind("ec", 0) == 0;
    tsa->key = makeCert(spec);
    tsa->certDer = certDer(tsa->key);
    auto info = provider.parseCertificate(tsa->certDer);
    if (info) tsa->info = *info;
    tsa->chainDer.push_back(tsa->certDer);
    if (issuer) {
        tsa->chainDer.push_back(certDer(*issuer));
        auto ca = provider.parseCertificate(certDer(*issuer));
        if (ca) tsa->anchors.add(*ca);
    } else if (info) {
        tsa->anchors.add(*info);
    }
    return tsa;
}

inline CertSpec tsaSpec(const char* cn = "Test TSA") {
    CertSpec s;
    s.cn = cn;
    s.keyKind = "ec256";
    s.notBeforeOffsetSec = -3600L * 24 * 60;
    s.extKeyUsage = "critical,timeStamping";
    return s;
}

struct TsaCfg {
    std::string policy = "1.3.6.1.4.1.99999.1";
    std::optional<std::chrono::system_clock::time_point> genTime;
    int fractionMicros = 0;
    bool dropNonce = false;
    std::optional<Bytes> nonce;                           // replaces the echoed nonce
    std::optional<std::pair<std::string, Bytes>> imprint; // replaces the echoed imprint (oid, digest)
    int ess = 2;                                          // 0 none, 1 ESSCertID (SHA-1), 2 v2 (default hash), 3 v2 (explicit SHA-256)
    bool embedCerts = true;
    bool tsaName = false;
    bool accuracy = false;
    std::optional<int> status;                            // answer with this PKIStatus and no token
    std::uint32_t failInfo = 0;                           // named bits, bit n = 1u << n
    std::string statusText;
    bool grantedNoToken = false;
    int httpStatus = 200;
    std::string contentType = "application/timestamp-reply";
    std::optional<std::string> requestedPolicyEcho;       // ignored
};

struct ParsedReq {
    bool ok = false;
    int version = 0;
    std::string algOid;
    Bytes digest;
    std::optional<Bytes> nonce;
    bool certReq = false;
    std::optional<std::string> policy;
};

inline std::string oidText(const ASN1_OBJECT* o) {
    char buf[128];
    OBJ_obj2txt(buf, sizeof buf, o, 1);
    return buf;
}

inline ParsedReq parseReqWithOpenSsl(ByteView der) {
    ParsedReq out;
    const unsigned char* p = der.data();
    std::unique_ptr<TS_REQ, decltype(&TS_REQ_free)> req(d2i_TS_REQ(nullptr, &p, static_cast<long>(der.size())), &TS_REQ_free);
    if (!req) return out;
    out.ok = true;
    out.version = static_cast<int>(TS_REQ_get_version(req.get()));
    TS_MSG_IMPRINT* mi = TS_REQ_get_msg_imprint(req.get());
    const X509_ALGOR* alg = TS_MSG_IMPRINT_get_algo(mi);
    const ASN1_OBJECT* obj = nullptr;
    X509_ALGOR_get0(&obj, nullptr, nullptr, alg);
    out.algOid = oidText(obj);
    const ASN1_OCTET_STRING* msg = TS_MSG_IMPRINT_get_msg(mi);
    out.digest.assign(ASN1_STRING_get0_data(msg), ASN1_STRING_get0_data(msg) + ASN1_STRING_length(msg));
    if (const ASN1_INTEGER* n = TS_REQ_get_nonce(req.get())) {
        BIGNUM* bn = ASN1_INTEGER_to_BN(n, nullptr);
        Bytes mag(static_cast<std::size_t>(BN_num_bytes(bn)));
        BN_bn2bin(bn, mag.data());
        BN_free(bn);
        out.nonce = der::normalizeInteger(mag);
    }
    out.certReq = TS_REQ_get_cert_req(req.get()) != 0;
    if (const ASN1_OBJECT* pol = TS_REQ_get_policy_id(req.get())) out.policy = oidText(pol);
    return out;
}

inline Bytes genTimeElement(std::chrono::system_clock::time_point t, int micros) {
    const std::time_t tt = static_cast<std::time_t>(
        std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
    std::tm tmv{};
#if defined(_WIN32)
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    std::string text = buf;
    if (micros > 0) {
        char f[16];
        std::snprintf(f, sizeof f, "%06d", micros);
        std::string frac = f;
        while (frac.back() == '0') frac.pop_back();
        text += "." + frac;
    }
    text += "Z";
    return der::tlv(der::kGeneralizedTime, viewOf(text));
}

inline Bytes signWithKey(EVP_PKEY* pkey, ByteView message) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    EVP_DigestSignInit(ctx.get(), nullptr, EVP_sha256(), nullptr, pkey);
    std::size_t len = 0;
    EVP_DigestSign(ctx.get(), nullptr, &len, message.data(), message.size());
    Bytes sig(len);
    EVP_DigestSign(ctx.get(), sig.data(), &len, message.data(), message.size());
    sig.resize(len);
    return sig;
}

inline CmsAttribute makeAttr(std::string_view o, Bytes value) {
    CmsAttribute a;
    a.oid = std::string(o);
    a.der = der::sequence({der::oidElement(o), der::tlv(der::kSet, value)});
    a.values.push_back(std::move(value));
    return a;
}

inline Bytes buildTokenFor(CryptoProvider& provider, const Tsa& tsa, const ParsedReq& req, const TsaCfg& cfg) {
    static long serial = 41;
    const std::string algOid = cfg.imprint ? cfg.imprint->first : req.algOid;
    const Bytes digest = cfg.imprint ? cfg.imprint->second : req.digest;
    const bool gost = algOid.rfind("1.2.643", 0) == 0;
    const Bytes algId = gost ? der::sequence({der::oidElement(algOid)}) : der::sequence({der::oidElement(algOid), der::nullValue()});

    std::vector<Bytes> tst;
    tst.push_back(der::integerFromInt(1));
    tst.push_back(der::oidElement(req.policy ? *req.policy : cfg.policy));
    tst.push_back(der::sequence({algId, der::octetString(digest)}));
    tst.push_back(der::integerFromInt(++serial));
    tst.push_back(genTimeElement(cfg.genTime.value_or(std::chrono::system_clock::now()), cfg.fractionMicros));
    if (cfg.accuracy)
        tst.push_back(der::sequence({der::integerFromInt(1), der::tlv(0x80, Bytes{0x01, 0xF4}), der::tlv(0x81, Bytes{0x2A})}));
    if (cfg.nonce) tst.push_back(der::integer(*cfg.nonce));
    else if (req.nonce && !cfg.dropNonce) tst.push_back(der::integer(*req.nonce));
    if (cfg.tsaName) tst.push_back(der::tlv(der::contextTag(0, true), der::tlv(der::contextTag(4, true), tsa.info.subjectDer)));
    const Bytes tstInfo = der::sequence(tst);

    std::vector<CmsAttribute> attrs;
    attrs.push_back(makeAttr(oid::kContentType, der::oidElement(oid::kTstInfo)));
    attrs.push_back(makeAttr(oid::kMessageDigest, der::octetString(*provider.digest(DigestAlgorithm::Sha256, tstInfo))));
    if (cfg.ess == 1) {
        const Bytes id = der::sequence({der::octetString(*provider.digest(DigestAlgorithm::Sha1, tsa.certDer))});
        attrs.push_back(makeAttr(oid::kSigningCertificateV1, der::sequence({der::sequence({id})})));
    } else if (cfg.ess >= 2) {
        const Bytes hash = der::octetString(*provider.digest(DigestAlgorithm::Sha256, tsa.certDer));
        const Bytes id = cfg.ess == 3 ? der::sequence({der::sequence({der::oidElement(oid::kSha256)}), hash}) : der::sequence({hash});
        attrs.push_back(makeAttr(oid::kSigningCertificateV2, der::sequence({der::sequence({id})})));
    }
    std::vector<Bytes> ders;
    for (const CmsAttribute& a : attrs) ders.push_back(a.der);
    const Bytes attrSet = der::setOf(ders);
    Bytes signedAttrsDer = attrSet;
    signedAttrsDer[0] = der::contextTag(0, true);

    CmsSignerInfo si;
    si.version = 1;
    si.issuerDer = tsa.info.issuerDer;
    si.serial = tsa.info.serial;
    si.sidDer = der::sequence({tsa.info.issuerDer, der::integer(tsa.info.serial)});
    si.digestOid = std::string(oid::kSha256);
    si.digestAlgDer = der::sequence({der::oidElement(oid::kSha256)});
    si.signedAttrs = attrs;
    si.signedAttrsDer = signedAttrsDer;
    si.signatureAlgOid = std::string(tsa.ec ? oid::kEcdsaSha256 : oid::kSha256WithRsa);
    si.signatureAlgDer = tsa.ec ? der::sequence({der::oidElement(oid::kEcdsaSha256)})
                                : der::sequence({der::oidElement(oid::kSha256WithRsa), der::nullValue()});
    si.signature = signWithKey(tsa.key.pkey.get(), attrSet);

    CmsSignedData model;
    model.contentType = std::string(oid::kTstInfo);
    model.content = tstInfo;
    if (cfg.embedCerts && req.certReq) model.certificates = tsa.chainDer;
    model.signers.push_back(std::move(si));
    return *encodeCms(model);
}

inline Bytes buildResponseFor(CryptoProvider& provider, const Tsa& tsa, ByteView reqDer, const TsaCfg& cfg) {
    const ParsedReq req = parseReqWithOpenSsl(reqDer);
    if (cfg.status) {
        std::vector<Bytes> info{der::integerFromInt(*cfg.status)};
        if (!cfg.statusText.empty()) info.push_back(der::sequence({der::tlv(der::kUtf8String, viewOf(cfg.statusText))}));
        if (cfg.failInfo) {
            int top = 0;
            for (int b = 0; b < 32; ++b)
                if (cfg.failInfo & (1u << b)) top = b;
            Bytes bits;
            bits.push_back(static_cast<std::uint8_t>(7 - top % 8));
            bits.resize(static_cast<std::size_t>(top / 8 + 2), 0);
            for (int b = 0; b <= top; ++b)
                if (cfg.failInfo & (1u << b)) bits[1 + static_cast<std::size_t>(b / 8)] |= static_cast<std::uint8_t>(0x80 >> (b % 8));
            info.push_back(der::tlv(der::kBitString, bits));
        }
        return der::sequence({der::sequence(info)});
    }
    if (cfg.grantedNoToken) return der::sequence({der::sequence({der::integerFromInt(0)})});
    return der::sequence({der::sequence({der::integerFromInt(0)}), buildTokenFor(provider, tsa, req, cfg)});
}

class TsaTransport final : public net::INetworkTransport {
public:
    TsaTransport(std::shared_ptr<CryptoProvider> provider, std::shared_ptr<Tsa> tsa)
        : provider_(std::move(provider)), tsa_(std::move(tsa)) {}

    core::Result<net::HttpResponse> perform(const net::HttpRequest& request, const net::NetworkPolicy&,
                                            net::CancelToken) override {
        ++calls;
        lastRequest = request;
        net::HttpResponse r;
        r.status = cfg.httpStatus;
        r.headers.emplace_back("Content-Type", cfg.contentType);
        const Bytes body(request.body.begin(), request.body.end());
        r.body.clear();
        const Bytes resp = buildResponseFor(*provider_, *tsa_, body, cfg);
        r.body.assign(resp.begin(), resp.end());
        return r;
    }

    TsaCfg cfg;
    std::atomic<int> calls{0};
    net::HttpRequest lastRequest;

private:
    std::shared_ptr<CryptoProvider> provider_;
    std::shared_ptr<Tsa> tsa_;
};

} // namespace rivet::crypto::testing
