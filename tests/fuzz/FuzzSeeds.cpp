// SPDX-License-Identifier: MPL-2.0
// Seed corpora, generated at run time from the existing test-support builders
// (throwaway keys, in-process PKI / TSA / OCSP). Nothing is committed to disk
// and no real key material is involved.
#include "FuzzTargets.hpp"

#include "LtvTestSupport.hpp"
#include "EncryptTestSupport.hpp"
#include "TestSupport.hpp"
#include "TestTimeStampSupport.hpp"
#include "crypto/Cms.hpp"
#include "crypto/Der.hpp"
#include "crypto/remote/JsonLite.hpp"
#include "crypto/revocation/Ocsp.hpp"
#include "crypto/timestamp/TimeStamp.hpp"
#include "revocation/TestRevocationSupport.hpp"

#include <string>

namespace rivet::fuzz {

namespace {

namespace cr = rivet::crypto;
namespace ct = rivet::crypto::testing;
namespace dr = rivet::crypto::der;

Bytes B(std::string_view s) { return Bytes(s.begin(), s.end()); }
Bytes B(const cr::Bytes& b) { return Bytes(b.begin(), b.end()); }

cr::CryptoProvider& provider() {
    static const std::shared_ptr<cr::CryptoProvider> p = cr::makeDefaultProvider();
    return *p;
}

cr::Bytes sha256Of(std::string_view message) {
    auto d = provider().digest(cr::DigestAlgorithm::Sha256, cr::viewOf(message));
    return d ? *d : cr::Bytes{};
}

// TimeStampResp for `digest`, built by the test TSA.
cr::Bytes tsResponse(const ct::Tsa& tsa, const cr::Bytes& digest, const ct::TsaCfg& cfg = {}) {
    cr::TimeStampRequestOptions o;
    o.includeNonce = !cfg.dropNonce;
    auto req = cr::buildTimeStampRequest(provider(), cr::DigestAlgorithm::Sha256, digest, o);
    if (!req) return {};
    return ct::buildResponseFor(provider(), tsa, req->der, cfg);
}

cr::Bytes tokenOf(const cr::Bytes& response) {
    auto r = cr::parseTimeStampResponse(response);
    return r && r->tokenDer ? *r->tokenDer : cr::Bytes{};
}

} // namespace

std::vector<Bytes> seedsDer() {
    std::vector<Bytes> out;
    out.push_back(B(dr::sequence({dr::integerFromInt(5), dr::oidElement("1.2.840.113549.1.7.2"), dr::nullValue(),
                                  dr::octetString(cr::viewOf("abc"))})));
    out.push_back(B(dr::sequence({dr::sequence({dr::oidElement("2.5.4.3"), dr::tlv(dr::kUtf8String, cr::viewOf("name"))}),
                                  dr::timeElement(std::chrono::system_clock::now())})));
    out.push_back(B(dr::setOf({dr::integerFromInt(300), dr::integerFromInt(-1), dr::integerFromInt(0)})));
    // BER with indefinite lengths: SEQUENCE { OCTET STRING "hi" } EOC
    out.push_back(Bytes{0x30, 0x80, 0x04, 0x02, 'h', 'i', 0x00, 0x00});
    // Constructed (segmented) OCTET STRING.
    out.push_back(Bytes{0x24, 0x80, 0x04, 0x01, 'a', 0x04, 0x01, 'b', 0x00, 0x00});
    out.push_back(Bytes{0x17, 0x0D, '2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z'});
    out.push_back(Bytes{0x18, 0x0F, '2', '0', '2', '4', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0', 'Z'});
    out.push_back(Bytes{0x06, 0x09, 0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x01, 0x0B});
    // Deep nesting.
    cr::Bytes nested = dr::nullValue();
    for (int i = 0; i < 20; ++i) nested = dr::sequence({nested});
    out.push_back(B(nested));
    ct::CertSpec spec;
    spec.keyKind = "ec256";
    out.push_back(B(ct::certDer(ct::makeCert(spec))));
    return out;
}

std::vector<Bytes> seedsX509() {
    std::vector<Bytes> out;
    ct::CertSpec ca;
    ca.cn = "Fuzz CA";
    ca.keyKind = "ec256";
    ca.ca = true;
    const ct::TestKey caKey = ct::makeCert(ca);
    out.push_back(B(ct::certDer(caKey)));
    ct::CertSpec leaf;
    leaf.cn = "Fuzz Leaf";
    leaf.keyKind = "ec256";
    leaf.issuer = &caKey;
    leaf.extraExt = {{NID_info_access, "OCSP;URI:http://ocsp.example.com/ocsp,caIssuers;URI:http://ca.example.com/ca.cer"},
                     {NID_crl_distribution_points, "URI:http://crl.example.com/ca.crl"}};
    out.push_back(B(ct::certDer(ct::makeCert(leaf))));
    ct::CertSpec ru;
    ru.cn = "Fuzz RSA";
    ru.keyKind = "rsa2048";
    ru.inn = "123456789012";
    ru.snils = "11223344595";
    ru.email = "fuzz@example.com";
    out.push_back(B(ct::certDer(ct::makeCert(ru))));
    ct::CertSpec p384;
    p384.keyKind = "ec384";
    p384.extKeyUsage = "critical,timeStamping";
    out.push_back(B(ct::certDer(ct::makeCert(p384))));
    return out;
}

std::vector<Bytes> seedsCms() {
    std::vector<Bytes> out;
    ct::CertSpec spec;
    spec.cn = "Fuzz CMS Signer";
    spec.keyKind = "ec256";
    const ct::TestKey key = ct::makeCert(spec);
    auto id = ct::makeIdentity(provider(), key);
    if (!id) return out;
    const cr::Bytes content(kCmsMessage.begin(), kCmsMessage.end());
    cr::CmsSignOptions options;
    auto detached = cr::signCms(provider(), *id, cr::ContentSource::fromBytes(content), true, options);
    auto attached = cr::signCms(provider(), *id, cr::ContentSource::fromBytes(content), false, options);
    options.cadesBes = false;
    options.includeSigningTime = false;
    auto bare = cr::signCms(provider(), *id, cr::ContentSource::fromBytes(content), true, options);
    for (auto* r : {&detached, &attached, &bare})
        if (*r)
            if (auto der = cr::encodeCms(**r)) out.push_back(B(*der));
    if (detached) {
        // With a signature time-stamp and a countersignature.
        auto tsa = ct::makeTsa(provider(), ct::tsaSpec());
        cr::CmsSignedData stamped = *detached;
        if (auto imprint = cr::signatureTimestampImprint(provider(), stamped.signers.at(0), cr::DigestAlgorithm::Sha256)) {
            const cr::Bytes token = tokenOf(tsResponse(*tsa, *imprint));
            if (!token.empty() && cr::attachSignatureTimestamp(stamped, 0, token, provider()))
                if (auto der = cr::encodeCms(stamped)) {
                    out.push_back(B(*der));
                    std::string pem = "-----BEGIN CMS-----\n" + crypto::remote::base64Encode(*der) + "\n-----END CMS-----\n";
                    out.push_back(B(pem));
                }
        }
        cr::CmsSignedData counter = *detached;
        if (cr::addCountersignature(counter, 0, provider(), *id))
            if (auto der = cr::encodeCms(counter)) out.push_back(B(*der));
    }
    return out;
}

std::vector<Bytes> seedsTimestamp() {
    std::vector<Bytes> out;
    const cr::Bytes digest = sha256Of(kTimestampMessage);
    auto tsa = ct::makeTsa(provider(), ct::tsaSpec());
    ct::TsaCfg base;
    const cr::Bytes response = tsResponse(*tsa, digest, base);
    out.push_back(B(response));
    out.push_back(B(tokenOf(response))); // bare ContentInfo
    ct::TsaCfg c;
    c.ess = 1;
    c.accuracy = true;
    c.tsaName = true;
    c.fractionMicros = 123456;
    out.push_back(B(tsResponse(*tsa, digest, c)));
    c = {};
    c.ess = 3;
    c.embedCerts = false;
    c.dropNonce = true;
    out.push_back(B(tsResponse(*tsa, digest, c)));
    c = {};
    c.status = 2;
    c.statusText = "request rejected";
    c.failInfo = cr::pkifailure::kBadAlg | cr::pkifailure::kSystemFailure;
    out.push_back(B(tsResponse(*tsa, digest, c)));
    c = {};
    c.grantedNoToken = true;
    out.push_back(B(tsResponse(*tsa, digest, c)));
    c = {};
    c.nonce = cr::Bytes{0x01, 0x02, 0x03};
    out.push_back(B(tsResponse(*tsa, digest, c)));
    return out;
}

std::vector<Bytes> seedsRevocation() {
    std::vector<Bytes> out;
    const ct::rev::Pki pki;
    const cr::Bytes certDer = pki.leafInfo.der;
    const cr::Bytes caDer = pki.caInfo.der;
    auto req = cr::buildOcspRequest(provider(), pki.leafInfo, pki.caInfo);
    if (req) {
        auto ocsp = [&](ct::rev::OcspCfg cfg) {
            out.push_back(frameRevocation(certDer, caDer, ct::rev::makeOcspResponse(pki, cfg, req->der)));
        };
        ocsp({});
        ct::rev::OcspCfg r;
        r.kind = ct::rev::Kind::Revoked;
        ocsp(r);
        r = {};
        r.kind = ct::rev::Kind::Unknown;
        ocsp(r);
        r = {};
        r.signer = &pki.responder;
        ocsp(r);
        r.byKey = true;
        ocsp(r);
        r = {};
        r.nextUpdateOffsetSec.reset();
        r.nonce = ct::rev::NonceMode::Omit;
        ocsp(r);
        r = {};
        r.responseStatus = 3; // tryLater
        ocsp(r);
        r = {};
        r.signer = &pki.otherResponder;
        ocsp(r);
    }
    auto crl = [&](ct::rev::CrlCfg cfg) { out.push_back(frameRevocation(certDer, caDer, ct::rev::makeCrl(pki, cfg))); };
    ct::rev::CrlCfg c;
    c.revoked = {&pki.leaf};
    crl(c);
    c = {};
    crl(c);
    c.onlyCaCerts = true;
    crl(c);
    c = {};
    c.deltaIndicator = true;
    crl(c);
    c = {};
    c.criticalUnknownExt = true;
    c.revoked = {&pki.leaf2};
    crl(c);
    return out;
}

std::vector<Bytes> seedsPdfSignature() {
    using namespace rivet::pdf::sign::testing;
    std::vector<Bytes> out;
    out.push_back(B(makeFixture()));
    FixtureSpec spec;
    spec.emptySigField = true;
    spec.textField = true;
    spec.xrefStream = true;
    spec.objectStream = true;
    out.push_back(B(makeFixture(spec)));
    LtvEnv env;
    for (const pdf::sign::PadesLevel level : {pdf::sign::PadesLevel::BaselineB, pdf::sign::PadesLevel::BaselineLT,
                                              pdf::sign::PadesLevel::BaselineLTA}) {
        auto signed_ = pdf::sign::signPdf(makeFixture(), env.request(level), *env.provider, *env.identity);
        if (signed_) out.push_back(B(signed_->data));
    }
    return out;
}

std::vector<Bytes> seedsPdfEncrypt() {
    namespace en = rivet::test::enc;
    std::vector<Bytes> out;
    const std::string base = en::samplePdf();
    const std::string noInfo = en::samplePdf("");
    const std::string tricky = en::buildPdf({"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                                             "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 10 10] /Contents 4 0 R >>",
                                             en::stream("/Filter /ASCIIHexDecode", "48656C6C6F>"),
                                             "[ (a) <62> /N 3 (nested (paren) ok) ]"},
                                            "/ID [<0102030405060708090A0B0C0D0E0F10> <0102030405060708090A0B0C0D0E0F10>]");
    for (const std::string* pdf : {&base, &noInfo, &tricky}) {
        for (const std::uint8_t opt : {std::uint8_t{0x0E}, std::uint8_t{0x3F}, std::uint8_t{0x00}, std::uint8_t{0x0B}}) {
            Bytes b{opt};
            b.insert(b.end(), pdf->begin(), pdf->end());
            out.push_back(std::move(b));
        }
    }
    return out;
}

std::vector<Bytes> seedsNet() {
    std::vector<Bytes> out;
    auto mode = [&](char m, std::string_view body) {
        Bytes b{static_cast<std::uint8_t>(m)};
        b.insert(b.end(), body.begin(), body.end());
        out.push_back(std::move(b));
    };
    for (std::string_view url : {"https://example.com/path?q=1#frag", "http://user@host:8080/a/b/../c", "https://[2001:db8::1]:8443/x",
                                 "http://2130706433/", "http://0x7f.1/", "http://127.1/", "https://[::ffff:127.0.0.1]/",
                                 "http://[::1]/", "http://169.254.169.254/latest/", "https://EXAMPLE.com.:443/%7Efoo",
                                 "http://localhost./", "https://a.b/\xE2\x82\xAC", "ftp://example.com/"})
        mode(0, url);
    mode(1, "https://example.com/a/b\n../c?x");
    mode(1, "https://example.com/a\n//evil.example.org/p");
    mode(1, "https://example.com/a\nhttp://127.0.0.1/");
    mode(1, "http://example.com/a\n?only=query");
    for (std::string_view ip : {"127.0.0.1", "0x7f000001", "017700000001", "2130706433", "::1", "::ffff:10.0.0.1",
                                "fe80::1%eth0", "2001:db8::", "1.2.3", "256.1.1.1", "::", "64:ff9b::7f00:1"})
        mode(2, ip);
    mode(3, "*.example.com\nwww.example.com");
    mode(3, "example.com\nEXAMPLE.COM.");
    mode(4, "Content-Type: text/plain\nLocation: /next\nX-Bad: a\r\nb\n");
    mode(4, "Host: evil\nContent-Length: 5\nAuthorization: Bearer x\n");
    // Redirect chains: [policy][status idx, len, location]...
    auto chain = [&](std::uint8_t policy, std::initializer_list<std::pair<int, std::string_view>> hops) {
        Bytes b{5, policy};
        for (const auto& [status, loc] : hops) {
            b.push_back(static_cast<std::uint8_t>(status));
            b.push_back(static_cast<std::uint8_t>(loc.size()));
            b.insert(b.end(), loc.begin(), loc.end());
        }
        out.push_back(std::move(b));
    };
    chain(0x31, {{1, "https://example.com/b"}, {2, "/c"}, {0, ""}});
    chain(0x33, {{1, "http://example.com/b"}, {5, "http://127.0.0.1/"}});
    chain(0x35, {{1, "https://other.example.org/x"}, {3, "https://example.com/a"}, {1, "https://example.com/a"}});
    chain(0x7B, {{1, "https://example.com/b"}, {1, "https://[::1]/"}, {1, "http://169.254.169.254/latest/meta-data"}});
    chain(0x30, {{4, "//example.com/c"}});
    return out;
}

std::vector<Bytes> seedsJson() {
    std::vector<Bytes> out;
    for (std::string_view doc :
         {R"({"access_token":"abc","expires_in":3600,"token_type":"Bearer"})",
          R"({"a":[1,2.5,-3e2,true,false,null,"xé😀\n"],"b":{"c":{"d":[]}}})", R"([[[[[[[[[1]]]]]]]]])",
          R"({"error":"invalid_grant","error_description":"expired \"code\""})", R"({"n":12345678901234567890})",
          R"({"s":"YWJj","k":"a b&c=d"})", "YWJjZGVmZw==", "  {\"x\" : [ ] }  "})
        out.push_back(B(doc));
    return out;
}

} // namespace rivet::fuzz
