// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "TestSupport.hpp"
#include "crypto/Cms.hpp"

#include <algorithm>

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

namespace {

Bytes bytesOf(std::string_view s) { return Bytes(s.begin(), s.end()); }

CmsSignOptions opts(std::optional<SignatureAlgorithm> alg = std::nullopt) {
    CmsSignOptions o;
    o.signature = alg;
    return o;
}

SignatureState stateOf(const std::vector<SignerVerification>& v, std::size_t i = 0) { return v.at(i).validation.state; }

// Sign with the given key kind/algorithm, verify attached and detached.
void roundTrip(const std::string& keyKind, std::optional<SignatureAlgorithm> alg) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({.cn = "Signer", .keyKind = keyKind});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    const Bytes content = bytesOf("The quick brown fox jumps over the lazy dog");

    // attached
    auto attached = signCms(*provider, *id, ContentSource::fromBytes(content), false, opts(alg));
    CHECK(attached.has_value());
    if (!attached) return;
    CHECK(!attached->isDetached());
    auto der = encodeCms(*attached);
    CHECK(der.has_value());
    if (!der) return;
    auto reparsed = parseCms(*der);
    CHECK(reparsed.has_value());
    if (!reparsed) return;
    auto v = verifyCms(*reparsed, *provider, nullptr);
    CHECK(v.has_value());
    if (!v) return;
    CHECK_EQ(v->size(), std::size_t{1});
    CHECK_EQ(stateOf(*v), SignatureState::ValidUntrusted); // valid, no trust anchors supplied
    CHECK(v->at(0).validation.crypto == CryptoCheck::Valid);
    CHECK(v->at(0).signer.has_value());

    // detached
    auto detached = signCms(*provider, *id, ContentSource::fromBytes(content), true, opts(alg));
    CHECK(detached.has_value());
    if (!detached) return;
    CHECK(detached->isDetached());
    const auto src = ContentSource::fromBytes(content);
    auto dv = verifyCms(*detached, *provider, &src);
    CHECK(dv.has_value());
    if (dv) CHECK_EQ(stateOf(*dv), SignatureState::ValidUntrusted);
    // Detached without content is a verification-blocking error, not a state.
    auto missing = verifyCms(*detached, *provider, nullptr);
    CHECK(!missing.has_value());
}

} // namespace

RIVET_TEST(cmsRsaPkcs1SignVerify) { roundTrip("rsa2048", SignatureAlgorithm::RsaPkcs1Sha256); }
RIVET_TEST(cmsRsaPkcs1Sha512SignVerify) { roundTrip("rsa3072", SignatureAlgorithm::RsaPkcs1Sha512); }
RIVET_TEST(cmsRsaPssSignVerify) { roundTrip("rsa2048", SignatureAlgorithm::RsaPssSha256); }
RIVET_TEST(cmsRsaPssSha384SignVerify) { roundTrip("rsa2048", SignatureAlgorithm::RsaPssSha384); }
RIVET_TEST(cmsEcdsaP256SignVerify) { roundTrip("ec256", SignatureAlgorithm::EcdsaSha256); }
RIVET_TEST(cmsEcdsaP384SignVerify) { roundTrip("ec384", SignatureAlgorithm::EcdsaSha384); }
RIVET_TEST(cmsDefaultAlgorithmFollowsKeyType) {
    roundTrip("rsa2048", std::nullopt);
    roundTrip("ec256", std::nullopt);
}

RIVET_TEST(cmsCadesBesAttributesPresent) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("x")), true, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    const auto& s = cms->signers.at(0);
    CHECK(s.findSigned(oid::kContentType) != nullptr);
    CHECK(s.findSigned(oid::kMessageDigest) != nullptr);
    CHECK(s.findSigned(oid::kSigningTime) != nullptr);
    CHECK(s.findSigned(oid::kSigningCertificateV2) != nullptr);
    // Without CAdES the ESS attribute is omitted.
    CmsSignOptions plain;
    plain.cadesBes = false;
    auto cms2 = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("x")), true, plain);
    CHECK(cms2.has_value());
    if (cms2) CHECK(cms2->signers.at(0).findSigned(oid::kSigningCertificateV2) == nullptr);
}

RIVET_TEST(cmsDetachedFileSignVerify) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    const auto path = tempDir() / "payload.bin";
    Bytes big(3 * 1024 * 1024 + 17);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 31 + 7);
    writeFile(path, big);
    const auto src = ContentSource::fromFile(path);
    auto cms = signCms(*provider, *id, src, true, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    auto ok = verifyCms(*cms, *provider, &src);
    CHECK(ok.has_value());
    if (ok) CHECK_EQ(stateOf(*ok), SignatureState::ValidUntrusted);
    // Modify one byte on disk.
    big[1000] ^= 0x01;
    writeFile(path, big);
    auto bad = verifyCms(*cms, *provider, &src);
    CHECK(bad.has_value());
    if (bad) CHECK_EQ(stateOf(*bad), SignatureState::DocumentModified);
    // A missing file is an error, not a verdict.
    const auto gone = ContentSource::fromFile(tempDir() / "does-not-exist.bin");
    CHECK(!verifyCms(*cms, *provider, &gone).has_value());
}

RIVET_TEST(cmsTamperedContentIsDocumentModified) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("pay 100")), false, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    (*cms->content)[4] = '9';
    auto v = verifyCms(*cms, *provider, nullptr);
    CHECK(v.has_value());
    if (v) {
        CHECK_EQ(stateOf(*v), SignatureState::DocumentModified);
        CHECK(v->at(0).validation.integrity == IntegrityStatus::Modified);
    }
}

RIVET_TEST(cmsTamperedSignatureIsInvalid) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("data")), false, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    cms->signers[0].signature[5] ^= 0xFF;
    auto v = verifyCms(*cms, *provider, nullptr);
    CHECK(v.has_value());
    if (v) CHECK_EQ(stateOf(*v), SignatureState::InvalidSignature);
}

RIVET_TEST(cmsTamperedSignedAttributeIsInvalid) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("data")), false, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    // Flip a byte inside the signing-time attribute: the digest is intact but
    // the signature over the attributes no longer verifies.
    Bytes& attrs = cms->signers[0].signedAttrsDer;
    const auto* t = cms->signers[0].findSigned(oid::kSigningTime);
    CHECK(t != nullptr);
    if (!t) return;
    // Locate the attribute's last byte in the raw element and change it.
    const auto& a = t->der;
    auto it = std::search(attrs.begin(), attrs.end(), a.begin(), a.end());
    CHECK(it != attrs.end());
    if (it == attrs.end()) return;
    *(it + static_cast<std::ptrdiff_t>(a.size()) - 2) ^= 0x01; // a digit of the time string
    auto der = encodeCms(*cms);
    CHECK(der.has_value());
    if (!der) return;
    auto re = parseCms(*der);
    CHECK(re.has_value());
    if (!re) return;
    auto v = verifyCms(*re, *provider, nullptr);
    CHECK(v.has_value());
    if (v) CHECK_EQ(stateOf(*v), SignatureState::InvalidSignature);
}

RIVET_TEST(cmsMultiSignerCoSign) {
    auto provider = makeDefaultProvider();
    const auto k1 = makeCert({.cn = "Alice"});
    const auto k2 = makeCert({.cn = "Bob", .keyKind = "ec256"});
    auto id1 = makeIdentity(*provider, k1);
    auto id2 = makeIdentity(*provider, k2);
    CHECK(id1 != nullptr && id2 != nullptr);
    if (!id1 || !id2) return;
    const auto src = ContentSource::fromBytes(bytesOf("contract text"));
    auto cms = signCms(*provider, *id1, src, true, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    const Bytes firstSig = cms->signers[0].signature;
    const Bytes firstAttrs = cms->signers[0].signedAttrsDer;
    CHECK(addSigner(*cms, *provider, *id2, &src, opts()).has_value());
    CHECK_EQ(cms->signers.size(), std::size_t{2});
    CHECK(cms->signers[0].signature == firstSig); // existing signer untouched
    CHECK(cms->signers[0].signedAttrsDer == firstAttrs);

    auto der = encodeCms(*cms);
    CHECK(der.has_value());
    if (!der) return;
    auto re = parseCms(*der);
    CHECK(re.has_value());
    if (!re) return;
    auto v = verifyCms(*re, *provider, &src);
    CHECK(v.has_value());
    if (!v) return;
    CHECK_EQ(v->size(), std::size_t{2});
    CHECK_EQ(stateOf(*v, 0), SignatureState::ValidUntrusted);
    CHECK_EQ(stateOf(*v, 1), SignatureState::ValidUntrusted);
    CHECK_EQ(v->at(0).signer->commonName.value_or(""), std::string("Alice"));
    CHECK_EQ(v->at(1).signer->commonName.value_or(""), std::string("Bob"));

    // Tampering with one signer's signature leaves the other valid.
    re->signers[1].signature[3] ^= 0x55;
    auto v2 = verifyCms(*re, *provider, &src);
    CHECK(v2.has_value());
    if (v2) {
        CHECK_EQ(stateOf(*v2, 0), SignatureState::ValidUntrusted);
        CHECK_EQ(stateOf(*v2, 1), SignatureState::InvalidSignature);
    }
}

RIVET_TEST(cmsCountersignature) {
    auto provider = makeDefaultProvider();
    const auto k1 = makeCert({.cn = "Author"});
    const auto k2 = makeCert({.cn = "Witness"});
    auto id1 = makeIdentity(*provider, k1);
    auto id2 = makeIdentity(*provider, k2);
    CHECK(id1 != nullptr && id2 != nullptr);
    if (!id1 || !id2) return;
    auto cms = signCms(*provider, *id1, ContentSource::fromBytes(bytesOf("doc")), false, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    CHECK(addCountersignature(*cms, 0, *provider, *id2, opts()).has_value());
    CHECK(cms->signers[0].findUnsigned(oid::kCounterSignature) != nullptr);
    auto der = encodeCms(*cms);
    CHECK(der.has_value());
    if (!der) return;
    auto re = parseCms(*der);
    CHECK(re.has_value());
    if (!re) return;
    auto v = verifyCms(*re, *provider, nullptr);
    CHECK(v.has_value());
    if (!v) return;
    CHECK_EQ(stateOf(*v), SignatureState::ValidUntrusted);
    CHECK_EQ(v->at(0).countersignatures.size(), std::size_t{1});
    if (!v->at(0).countersignatures.empty()) {
        const auto& c = v->at(0).countersignatures[0];
        CHECK_EQ(c.validation.state, SignatureState::ValidUntrusted);
        CHECK_EQ(c.signer->commonName.value_or(""), std::string("Witness"));
    }
    // Corrupting the original signature value breaks the countersignature too.
    re->signers[0].signature[2] ^= 0xFF;
    auto v2 = verifyCms(*re, *provider, nullptr);
    CHECK(v2.has_value());
    if (v2) {
        CHECK_EQ(stateOf(*v2), SignatureState::InvalidSignature);
        // The countersignature covers the signature value, so it no longer holds.
        if (!v2->at(0).countersignatures.empty()) {
            const auto s = v2->at(0).countersignatures[0].validation.state;
            CHECK(s == SignatureState::DocumentModified || s == SignatureState::InvalidSignature);
        }
    }
}

RIVET_TEST(cmsWeakCreationIsRefused) {
    auto provider = makeDefaultProvider();
    // RSA-1024 identity: refused regardless of digest.
    const auto weakKey = makeCert({.cn = "Weak", .keyKind = "rsa1024"});
    auto weak = makeIdentity(*provider, weakKey);
    CHECK(weak != nullptr);
    if (weak) {
        auto r = signCms(*provider, *weak, ContentSource::fromBytes(bytesOf("x")), false, opts());
        CHECK(!r.has_value());
        if (!r) CHECK_EQ(r.error().code, CryptoErrorCode::WeakAlgorithm);
        auto direct = weak->sign(SignatureAlgorithm::RsaPkcs1Sha256, bytesOf("x"), CancelToken{});
        CHECK(!direct.has_value());
        if (!direct) CHECK_EQ(direct.error().code, CryptoErrorCode::WeakAlgorithm);
    }
    // SHA-1 digest option: refused.
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    CmsSignOptions o;
    o.digest = DigestAlgorithm::Sha1;
    auto r = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("x")), false, o);
    CHECK(!r.has_value());
    if (!r) CHECK_EQ(r.error().code, CryptoErrorCode::WeakAlgorithm);
}

RIVET_TEST(cmsCancelledSigningReportsCancelled) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    CmsSignOptions o;
    o.cancel.cancel();
    auto r = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("x")), false, o);
    CHECK(!r.has_value());
    if (!r) CHECK_EQ(r.error().code, CryptoErrorCode::Cancelled);
}

RIVET_TEST(cmsTrustAnchorsDecideTrust) {
    auto provider = makeDefaultProvider();
    const auto ca = makeCert({.cn = "Root CA", .ca = true});
    CertSpec spec;
    spec.cn = "Leaf";
    spec.issuer = &ca;
    const auto leaf = makeCert(spec);
    auto id = makeIdentity(*provider, leaf, {&ca});
    CHECK(id != nullptr);
    if (!id) return;
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("hello")), false, opts());
    CHECK(cms.has_value());
    if (!cms) return;

    TrustAnchorSet anchors;
    auto caInfo = provider->parseCertificate(certDer(ca));
    CHECK(caInfo.has_value());
    if (!caInfo) return;
    anchors.add(*caInfo);
    CmsVerifyOptions vo;
    vo.anchors = &anchors;
    auto trusted = verifyCms(*cms, *provider, nullptr, vo);
    CHECK(trusted.has_value());
    if (trusted) CHECK_EQ(stateOf(*trusted), SignatureState::ValidTrusted);

    TrustAnchorSet other;
    const auto otherCa = makeCert({.cn = "Other CA", .ca = true});
    auto oi = provider->parseCertificate(certDer(otherCa));
    CHECK(oi.has_value());
    if (oi) other.add(*oi);
    vo.anchors = &other;
    auto untrusted = verifyCms(*cms, *provider, nullptr, vo);
    CHECK(untrusted.has_value());
    if (untrusted) CHECK_EQ(stateOf(*untrusted), SignatureState::ValidUntrusted);

    // A validation time after the leaf expired, with a trusted chain.
    vo.anchors = &anchors;
    vo.validationTime = std::chrono::system_clock::now() + std::chrono::hours(24 * 365 * 3);
    auto expired = verifyCms(*cms, *provider, nullptr, vo);
    CHECK(expired.has_value());
    if (expired) CHECK_EQ(stateOf(*expired), SignatureState::ValidExpiredNow);
}

RIVET_TEST(cmsDescribeMentionsSignerAndAlgorithms) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({.cn = "Describe Me"});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    auto cms = signCms(*provider, *id, ContentSource::fromBytes(bytesOf("x")), true, opts());
    CHECK(cms.has_value());
    if (!cms) return;
    const std::string text = describeCms(*cms, provider.get());
    CHECK(text.find("Describe Me") != std::string::npos);
    CHECK(text.find("etached") != std::string::npos);
}
