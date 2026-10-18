// SPDX-License-Identifier: MPL-2.0
#pragma once

// Fixtures for the PAdES B-LT / B-LTA tests: a two-level signer PKI
// (root -> intermediate -> leaf) and a TSA issued by its own CA, all answered
// by one in-process transport (OCSP per issuer, a CRL, AIA caIssuers and the
// TSA). Nothing here touches the real network.

#include "SignTestSupport.hpp"
#include "TestTimeStampSupport.hpp"
#include "crypto/timestamp/TimeStampClient.hpp"
#include "pdf/sign/PdfDss.hpp"
#include "pdf/sign/PdfLtv.hpp"
#include "revocation/TestRevocationSupport.hpp"

#include <map>
#include <mutex>

namespace rivet::pdf::sign::testing {

namespace crypto = rivet::crypto;
namespace net = rivet::net;
namespace ctest = rivet::crypto::testing;

constexpr const char* kOcspInter = "ocsp-inter.example.com";
constexpr const char* kOcspRoot = "ocsp-root.example.com";
constexpr const char* kOcspTsa = "ocsp-tsa.example.com";
constexpr const char* kCaHost = "ca.example.com";
constexpr const char* kCrlHost = "crl.example.com";

inline const crypto::testing::rev::Pki& dummyPki() {
    static const crypto::testing::rev::Pki pki;
    return pki;
}

class LtvTransport final : public net::INetworkTransport {
public:
    LtvTransport(std::shared_ptr<ctest::TsaTransport> tsa, const ctest::TestKey* inter, const ctest::TestKey* root,
                 const ctest::TestKey* tsaCa, Bytes interDer)
        : tsa_(std::move(tsa)), inter_(inter), root_(root), tsaCa_(tsaCa), interDer_(std::move(interDer)) {}

    core::Result<net::HttpResponse> perform(const net::HttpRequest& request, const net::NetworkPolicy& policy,
                                            net::CancelToken cancel) override {
        const std::string& host = request.url.host;
        {
            std::lock_guard lock(mutex);
            ++total;
            ++perHost[host];
        }
        if (host == "tsa.example.com") return tsa_->perform(request, policy, std::move(cancel));
        net::HttpResponse r;
        int forced = 0;
        {
            std::lock_guard lock(mutex);
            if (auto it = failHosts.find(host); it != failHosts.end()) forced = it->second;
        }
        if (forced) {
            r.status = forced;
            r.body = "unavailable";
            return r;
        }
        if (host == kOcspInter || host == kOcspRoot || host == kOcspTsa) {
            const ctest::TestKey* signer = host == kOcspInter ? inter_ : host == kOcspRoot ? root_ : tsaCa_;
            ctest::rev::OcspCfg cfg;
            {
                std::lock_guard lock(mutex);
                cfg = ocsp;
            }
            cfg.signer = signer;
            cfg.includeSignerCert = false;
            const Bytes body(request.body.begin(), request.body.end());
            const Bytes resp = ctest::rev::makeOcspResponse(dummyPki(), cfg, body);
            r.status = 200;
            r.headers.emplace_back("Content-Type", "application/ocsp-response");
            r.body.assign(resp.begin(), resp.end());
        } else if (host == kCrlHost) {
            std::lock_guard lock(mutex);
            r.status = 200;
            r.headers.emplace_back("Content-Type", "application/pkix-crl");
            r.body.assign(crlBody.begin(), crlBody.end());
        } else if (host == kCaHost) {
            r.status = 200;
            r.headers.emplace_back("Content-Type", "application/pkix-cert");
            r.body.assign(interDer_.begin(), interDer_.end());
        } else {
            r.status = 404;
        }
        return r;
    }

    int hostCalls(const std::string& host) {
        std::lock_guard lock(mutex);
        auto it = perHost.find(host);
        return it == perHost.end() ? 0 : it->second;
    }
    int totalCalls() {
        std::lock_guard lock(mutex);
        return total;
    }
    void fail(const std::string& host, int status = 500) {
        std::lock_guard lock(mutex);
        failHosts[host] = status;
    }

    ctest::rev::OcspCfg ocsp;
    Bytes crlBody;
    std::map<std::string, int> failHosts;
    std::map<std::string, int> perHost;
    int total = 0;
    std::mutex mutex;

private:
    std::shared_ptr<ctest::TsaTransport> tsa_;
    const ctest::TestKey* inter_;
    const ctest::TestKey* root_;
    const ctest::TestKey* tsaCa_;
    Bytes interDer_;
};

struct LtvEnv {
    std::shared_ptr<crypto::CryptoProvider> provider = crypto::makeDefaultProvider();
    ctest::TestKey root, inter, leaf, tsaCa;
    std::shared_ptr<ctest::Tsa> tsa;
    std::shared_ptr<ctest::TsaTransport> tsaTransport;
    std::shared_ptr<LtvTransport> transport;
    std::shared_ptr<net::OnlineValidationGate> gate =
        std::make_shared<net::OnlineValidationGate>(net::OnlineValidationMode::On);
    std::shared_ptr<net::NetworkClient> network;
    std::shared_ptr<crypto::TimeStampClient> client;
    std::unique_ptr<crypto::SigningIdentity> identity;
    crypto::TrustAnchorSet anchors;
    int tsaFetches = 0;

    explicit LtvEnv(bool leafHasChain = true) {
        ctest::CertSpec rs;
        rs.cn = "LTV Root CA";
        rs.keyKind = "ec256";
        rs.ca = true;
        rs.notBeforeOffsetSec = -3600L * 24 * 400;
        root = ctest::makeCert(rs);

        ctest::CertSpec is;
        is.cn = "LTV Intermediate CA";
        is.keyKind = "ec256";
        is.ca = true;
        is.issuer = &root;
        is.notBeforeOffsetSec = -3600L * 24 * 300;
        is.extraExt = {{NID_info_access, std::string("OCSP;URI:http://") + kOcspRoot + "/ocsp"},
                       {NID_crl_distribution_points, std::string("URI:http://") + kCrlHost + "/root.crl"}};
        inter = ctest::makeCert(is);

        ctest::CertSpec ls;
        ls.cn = "LTV Signer";
        ls.keyKind = "ec256";
        ls.issuer = &inter;
        ls.notBeforeOffsetSec = -3600L * 24 * 100;
        ls.extraExt = {{NID_info_access, std::string("OCSP;URI:http://") + kOcspInter + "/ocsp,caIssuers;URI:http://" +
                                             kCaHost + "/inter.cer"}};
        leaf = ctest::makeCert(ls);

        ctest::CertSpec cs;
        cs.cn = "LTV TSA CA";
        cs.keyKind = "ec256";
        cs.ca = true;
        cs.notBeforeOffsetSec = -3600L * 24 * 300;
        tsaCa = ctest::makeCert(cs);
        ctest::CertSpec ts = ctest::tsaSpec("LTV TSA");
        ts.extraExt = {{NID_info_access, std::string("OCSP;URI:http://") + kOcspTsa + "/ocsp"}};
        tsa = ctest::makeTsa(*provider, ts, &tsaCa);
        tsaTransport = std::make_shared<ctest::TsaTransport>(provider, tsa);

        transport = std::make_shared<LtvTransport>(tsaTransport, &inter, &root, &tsaCa, ctest::certDer(inter));
        net::NetworkPolicy policy;
        policy.allowHttp = true;
        network = std::make_shared<net::NetworkClient>(policy, transport, gate);
        client = std::make_shared<crypto::TimeStampClient>(network, provider);

        if (auto c = provider->parseCertificate(ctest::certDer(root))) anchors.add(*c);
        identity = leafHasChain ? ctest::makeIdentity(*provider, leaf, {&inter}) : ctest::makeIdentity(*provider, leaf);

        crypto::testing::rev::CrlCfg crl;
        crl.issuer = &root;
        crl.revoked = {}; // nothing revoked
        transport->crlBody = ctest::rev::makeCrl(dummyPki(), crl);
    }

    TimestampFetcher fetcher() {
        return [this](crypto::DigestAlgorithm alg, crypto::ByteView digest,
                      const crypto::CancelToken& cancel) -> crypto::CryptoResult<crypto::TimeStampVerification> {
            ++tsaFetches;
            crypto::TimeStampClientOptions o;
            o.verify.anchors = &tsa->anchors;
            auto r = client->request(ctest::kTsaUrl, alg, digest, o, cancel);
            if (!r) return std::unexpected(r.error());
            return std::move(r->verification);
        };
    }

    LtvOptions ltv(bool network_on = true) const {
        LtvOptions o;
        o.anchors = &anchors;
        o.timestampAnchors = &tsa->anchors;
        if (network_on) o.network = network;
        return o;
    }

    VerifyOptions verifyOpts() const {
        VerifyOptions o;
        o.anchors = &anchors;
        o.timestampAnchors = &tsa->anchors;
        return o;
    }

    SignRequest request(PadesLevel level) {
        SignRequest r = basicRequest();
        r.timestamp.fetch = fetcher();
        r.targetLevel = level;
        r.ltv = ltv();
        return r;
    }

    std::vector<PdfSignatureInfo> verify(const Bytes& pdf, const VerifyOptions* options = nullptr) const {
        auto r = verifyPdfSignatures(pdf, *provider, options ? *options : verifyOpts());
        if (!r) {
            std::printf("    verify error: %s\n", r.error().message.c_str());
            return {};
        }
        return std::move(*r);
    }

    UpgradeRequest upgrade(PadesLevel target) {
        UpgradeRequest u;
        u.target = target;
        u.ltv = ltv();
        u.archiveTimestamp = fetcher();
        return u;
    }
};

inline std::string errText(const SignError& e) {
    std::string s = std::string(toString(e.code)) + ": " + e.message;
    for (const std::string& m : e.missingEvidence) s += "\n      - " + m;
    return s;
}

inline void printLtv(const char* tag, const std::vector<PdfSignatureInfo>& v) {
    for (const PdfSignatureInfo& s : v) {
        std::printf("    [%s] #%zu %s level=%s vri=%d evaluated=%d signerEv=%d tsaEv=%d covered=%d", tag, s.index,
                    std::string(toString(s.kind)).c_str(), std::string(toString(s.level)).c_str(), s.ltv.vriPresent,
                    s.ltv.evaluated, s.ltv.signerEvidenceComplete, s.ltv.timestampEvidenceComplete,
                    s.ltv.dssCoveredByTimestamp);
        for (const std::string& r : s.ltv.reasons) std::printf("\n        reason: %s", r.c_str());
        for (const std::string& r : s.ltv.missing) std::printf("\n        missing: %s", r.c_str());
        std::printf("\n");
    }
}

inline const PdfSignatureInfo* firstSignature(const std::vector<PdfSignatureInfo>& v) {
    for (const PdfSignatureInfo& s : v)
        if (s.kind == SignatureKind::Signature) return &s;
    return nullptr;
}

inline std::size_t countKind(const std::vector<PdfSignatureInfo>& v, SignatureKind k) {
    std::size_t n = 0;
    for (const PdfSignatureInfo& s : v)
        if (s.kind == k) ++n;
    return n;
}

inline DssData dssOf(const Bytes& pdf) {
    auto f = PdfFile::open(pdf);
    if (!f) return {};
    return readDss(*f, 0);
}

} // namespace rivet::pdf::sign::testing
