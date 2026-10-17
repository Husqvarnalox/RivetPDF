// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "TestSupport.hpp"

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

namespace {
CertificateInfo parse(CryptoProvider& p, const TestKey& k) {
    auto r = p.parseCertificate(certDer(k));
    CHECK(r.has_value());
    return r ? *r : CertificateInfo{};
}
} // namespace

RIVET_TEST(certificateInfoFields) {
    auto provider = makeDefaultProvider();
    const auto k = makeCert({.cn = "Anna Ivanova", .inn = "500100732259", .snils = "12345678901"});
    const auto info = parse(*provider, k);
    CHECK_EQ(info.commonName.value_or(""), std::string("Anna Ivanova"));
    CHECK_EQ(info.publicKeyType, PublicKeyType::Rsa);
    CHECK_EQ(info.keyBits, 2048);
    CHECK(!info.isCa);
    CHECK(info.hasKeyUsage);
    CHECK_EQ(info.sha256Fingerprint.size(), std::size_t{64});
    CHECK_EQ(info.sha1Fingerprint.size(), std::size_t{40});
    CHECK(!info.subjectKeyId.empty());
    CHECK(info.russian.inn.has_value());
    CHECK_EQ(info.russian.inn.value_or(""), std::string("500100732259"));
    CHECK_EQ(info.russian.snils.value_or(""), std::string("12345678901"));
    CHECK(!info.russian.ogrn.has_value());
    CHECK(info.isSelfIssued());

    const auto ec = parse(*provider, makeCert({.cn = "EC", .keyKind = "ec384"}));
    CHECK_EQ(ec.publicKeyType, PublicKeyType::Ec);
    CHECK_EQ(ec.keyBits, 384);
    CHECK(!ec.russian.any());
}

RIVET_TEST(certificateParseRejectsGarbage) {
    auto provider = makeDefaultProvider();
    CHECK(!provider->parseCertificate(Bytes{}).has_value());
    CHECK(!provider->parseCertificate(Bytes{0x30, 0x03, 0x02, 0x01, 0x00}).has_value());
    auto r = provider->parseCertificate(Bytes(64, 0xFF));
    CHECK(!r.has_value());
}

RIVET_TEST(chainBuildingWithAnchors) {
    auto provider = makeDefaultProvider();
    const auto root = makeCert({.cn = "Root", .ca = true});
    CertSpec ispec;
    ispec.cn = "Intermediate";
    ispec.ca = true;
    ispec.issuer = &root;
    const auto inter = makeCert(ispec);
    CertSpec lspec;
    lspec.cn = "Leaf";
    lspec.issuer = &inter;
    const auto leaf = makeCert(lspec);

    const auto rootInfo = parse(*provider, root);
    const auto interInfo = parse(*provider, inter);
    const auto leafInfo = parse(*provider, leaf);

    TrustAnchorSet anchors;
    anchors.add(rootInfo);
    const auto full = buildChain(leafInfo, {interInfo}, anchors, *provider);
    CHECK_EQ(full.chain.size(), std::size_t{3});
    CHECK(full.complete);
    CHECK(full.anchored);

    // Missing intermediate: incomplete, not anchored.
    const auto gap = buildChain(leafInfo, {}, anchors, *provider);
    CHECK(!gap.complete);
    CHECK(!gap.anchored);

    // Complete but not anchored: pool holds the root, anchor set is empty.
    const auto noAnchor = buildChain(leafInfo, {interInfo, rootInfo}, TrustAnchorSet{}, *provider);
    CHECK(noAnchor.complete);
    CHECK(!noAnchor.anchored);

    // A leaf is never accepted as an issuer.
    const auto other = makeCert({.cn = "Other"});
    CertSpec fake;
    fake.cn = "Fake child";
    fake.issuer = &other; // non-CA issuer
    const auto fakeChild = makeCert(fake);
    const auto otherInfo = parse(*provider, other);
    const auto bad = buildChain(parse(*provider, fakeChild), {otherInfo}, TrustAnchorSet{}, *provider);
    CHECK(!bad.complete);
}

RIVET_TEST(chainRequiresValidSignatureOnLink) {
    auto provider = makeDefaultProvider();
    const auto root = makeCert({.cn = "Root", .ca = true});
    const auto impostor = makeCert({.cn = "Root", .ca = true}); // same subject, different key
    CertSpec lspec;
    lspec.cn = "Leaf";
    lspec.issuer = &root;
    const auto leaf = makeCert(lspec);
    TrustAnchorSet anchors;
    anchors.add(parse(*provider, impostor));
    const auto r = buildChain(parse(*provider, leaf), {}, anchors, *provider);
    CHECK(!r.anchored);
}

RIVET_TEST(validateCertificateReportsExpiryAndTrust) {
    auto provider = makeDefaultProvider();
    const auto root = makeCert({.cn = "Root", .ca = true});
    CertSpec lspec;
    lspec.cn = "Leaf";
    lspec.issuer = &root;
    const auto leaf = makeCert(lspec);
    CertSpec espec;
    espec.cn = "Expired";
    espec.issuer = &root;
    espec.notBeforeOffsetSec = -7200;
    espec.notAfterOffsetSec = -3600;
    const auto expired = makeCert(espec);

    TrustAnchorSet anchors;
    anchors.add(parse(*provider, root));
    const auto now = std::chrono::system_clock::now();

    const auto ok = validateCertificate(parse(*provider, leaf), {}, anchors, *provider, now);
    CHECK(ok.certificate == CertificateStatus::Valid);
    CHECK(ok.trust == TrustStatus::Trusted);

    const auto ex = validateCertificate(parse(*provider, expired), {}, anchors, *provider, now);
    CHECK(ex.certificate == CertificateStatus::Expired);
    CHECK(ex.trust == TrustStatus::Trusted);

    const auto early = validateCertificate(parse(*provider, leaf), {}, anchors, *provider, now - std::chrono::hours(48));
    CHECK(early.certificate == CertificateStatus::NotYetValid);

    const auto untrusted = validateCertificate(parse(*provider, leaf), {}, TrustAnchorSet{}, *provider, now);
    CHECK(untrusted.trust == TrustStatus::Untrusted);
}

RIVET_TEST(anchorSetValidatorUsesProvider) {
    std::shared_ptr<CryptoProvider> provider = makeDefaultProvider();
    const auto root = makeCert({.cn = "Root", .ca = true});
    CertSpec lspec;
    lspec.cn = "Leaf";
    lspec.issuer = &root;
    const auto leaf = makeCert(lspec);
    TrustAnchorSet anchors;
    anchors.add(parse(*provider, root));
    AnchorSetValidator validator(provider, anchors);
    CertificateValidationRequest req;
    req.certificate = parse(*provider, leaf);
    const auto v = validator.validate(req);
    CHECK(v.trust == TrustStatus::Trusted);
    CHECK(v.certificate == CertificateStatus::Valid);
}
