// SPDX-License-Identifier: MPL-2.0
// Certification path builder for historical validation (ADR-0044, ADR-0045).

#include "RivetTest.h"

#include "TestSupport.hpp"
#include "crypto/PathBuilder.hpp"

#include <algorithm>
#include <random>

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

namespace {

constexpr long kDay = 3600L * 24;

CertificateInfo parse(CryptoProvider& p, const TestKey& k) {
    auto r = p.parseCertificate(certDer(k));
    CHECK(r.has_value());
    return r ? *r : CertificateInfo{};
}

struct Pki {
    std::shared_ptr<CryptoProvider> provider = makeDefaultProvider();
    TestKey root, inter, leaf;
    TrustAnchorSet anchors;
    CertificateInfo rootInfo, interInfo, leafInfo;

    Pki() {
        CertSpec rs{.cn = "PB Root", .keyKind = "ec256", .ca = true, .notBeforeOffsetSec = -400 * kDay};
        root = makeCert(rs);
        CertSpec is{.cn = "PB Inter", .keyKind = "ec256", .ca = true, .notBeforeOffsetSec = -300 * kDay, .issuer = &root};
        inter = makeCert(is);
        CertSpec ls{.cn = "PB Leaf", .keyKind = "ec256", .notBeforeOffsetSec = -100 * kDay, .issuer = &inter};
        leaf = makeCert(ls);
        rootInfo = parse(*provider, root);
        interInfo = parse(*provider, inter);
        leafInfo = parse(*provider, leaf);
        anchors.add(rootInfo);
    }
};

PathResult build(Pki& p, const CertificateInfo& leaf, std::vector<CertificateInfo> pool,
                 std::chrono::system_clock::time_point at = std::chrono::system_clock::now(),
                 const TrustAnchorSet* anchors = nullptr) {
    PathOptions o;
    o.anchors = anchors ? anchors : &p.anchors;
    o.pool = std::move(pool);
    o.validationTime = at;
    PathResult r = buildValidationPath(leaf, o, *p.provider);
    std::printf("    path: anchored=%d failure=%s len=%zu", r.anchored, std::string(toString(r.failure)).c_str(), r.chain.size());
    for (const std::string& s : r.reasons) std::printf("\n      reason: %s", s.c_str());
    std::printf("\n");
    return r;
}

// A certificate with explicit subject/issuer names and keys (to build cross-signed loops).
TestKey rawCert(const std::string& subjectCn, EVP_PKEY* subjectKey, const std::string& issuerCn, EVP_PKEY* issuerKey) {
    TestKey out;
    out.cert.reset(X509_new());
    X509* c = out.cert.get();
    X509_set_version(c, 2);
    static long serial = 9000;
    ASN1_INTEGER_set(X509_get_serialNumber(c), ++serial);
    X509_gmtime_adj(X509_getm_notBefore(c), -100 * kDay);
    X509_gmtime_adj(X509_getm_notAfter(c), 100 * kDay);
    X509_set_pubkey(c, subjectKey);
    auto name = [](const std::string& cn) {
        X509_NAME* n = X509_NAME_new();
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
        return n;
    };
    X509_NAME* s = name(subjectCn);
    X509_NAME* i = name(issuerCn);
    X509_set_subject_name(c, s);
    X509_set_issuer_name(c, i);
    X509_NAME_free(s);
    X509_NAME_free(i);
    addExt(c, nullptr, NID_basic_constraints, "critical,CA:TRUE");
    addExt(c, nullptr, NID_key_usage, "critical,keyCertSign,cRLSign");
    X509_sign(c, issuerKey, EVP_sha256());
    return out;
}

} // namespace

RIVET_TEST(pathBuilderBuildsAnAnchoredPathInAnyPoolOrder) {
    Pki p;
    for (const auto& pool : {std::vector<CertificateInfo>{p.interInfo}, std::vector<CertificateInfo>{p.rootInfo, p.interInfo, p.leafInfo},
                             std::vector<CertificateInfo>{p.leafInfo, p.interInfo, p.rootInfo, p.interInfo}}) {
        const PathResult r = build(p, p.leafInfo, pool);
        CHECK(r.anchored);
        CHECK(r.complete);
        CHECK(r.failure == PathFailure::None);
        CHECK_EQ(r.chain.size(), std::size_t{3});
        if (r.chain.size() == 3) {
            CHECK(r.chain[0].sha256Fingerprint == p.leafInfo.sha256Fingerprint);
            CHECK(r.chain[1].sha256Fingerprint == p.interInfo.sha256Fingerprint);
            CHECK(r.chain[2].sha256Fingerprint == p.rootInfo.sha256Fingerprint);
        }
    }
}

RIVET_TEST(pathBuilderReportsAMissingIntermediate) {
    Pki p;
    const PathResult r = build(p, p.leafInfo, {});
    CHECK(!r.anchored);
    CHECK(r.failure == PathFailure::NoIssuer);
    CHECK(!r.reasons.empty());
}

RIVET_TEST(pathBuilderFetchesTheMissingIssuerThroughTheCallback) {
    Pki p;
    PathOptions o;
    o.anchors = &p.anchors;
    int calls = 0;
    o.fetchIssuers = [&](const CertificateInfo&) {
        ++calls;
        return std::vector<Bytes>{certDer(p.inter)};
    };
    const PathResult r = buildValidationPath(p.leafInfo, o, *p.provider);
    CHECK(r.anchored);
    CHECK_EQ(calls, 1);
    CHECK_EQ(r.fetched.size(), std::size_t{1});
    // No callback, no path.
    o.fetchIssuers = nullptr;
    CHECK(!buildValidationPath(p.leafInfo, o, *p.provider).anchored);
    // A callback returning garbage or the wrong certificate never yields a path.
    o.fetchIssuers = [&](const CertificateInfo&) { return std::vector<Bytes>{Bytes{1, 2, 3}, certDer(p.leaf)}; };
    CHECK(!buildValidationPath(p.leafInfo, o, *p.provider).anchored);
}

RIVET_TEST(pathBuilderRejectsLoopsWithoutHanging) {
    Pki p;
    PkeyPtr ka = makeKey("ec256"), kb = makeKey("ec256"), kl = makeKey("ec256");
    const TestKey a = rawCert("Loop A", ka.get(), "Loop B", kb.get()); // A is issued by B
    const TestKey b = rawCert("Loop B", kb.get(), "Loop A", ka.get()); // B is issued by A
    // A leaf issued by A.
    TestKey leaf;
    leaf.cert.reset(X509_new());
    {
        X509* c = leaf.cert.get();
        X509_set_version(c, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(c), 9999);
        X509_gmtime_adj(X509_getm_notBefore(c), -kDay);
        X509_gmtime_adj(X509_getm_notAfter(c), kDay);
        X509_set_pubkey(c, kl.get());
        X509_NAME* s = X509_NAME_new();
        X509_NAME_add_entry_by_txt(s, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>("Loop Leaf"), -1, -1, 0);
        X509_set_subject_name(c, s);
        X509_set_issuer_name(c, X509_get_subject_name(a.cert.get()));
        X509_NAME_free(s);
        addExt(c, nullptr, NID_basic_constraints, "critical,CA:FALSE");
        X509_sign(c, ka.get(), EVP_sha256());
    }
    const PathResult r = build(p, parse(*p.provider, leaf), {parse(*p.provider, a), parse(*p.provider, b)});
    CHECK(!r.anchored);
    CHECK(r.failure == PathFailure::Loop || r.failure == PathFailure::NoIssuer || r.failure == PathFailure::TooLong);
}

RIVET_TEST(pathBuilderEnforcesBasicConstraintsAndKeyUsage) {
    Pki p;
    // Non-CA intermediate.
    {
        CertSpec nonCa{.cn = "Not A CA", .keyKind = "ec256", .ca = false, .issuer = &p.root};
        const TestKey bad = makeCert(nonCa);
        CertSpec ls{.cn = "Under Non-CA", .keyKind = "ec256", .issuer = &bad};
        const TestKey leaf = makeCert(ls);
        const PathResult r = build(p, parse(*p.provider, leaf), {parse(*p.provider, bad)});
        CHECK(!r.anchored);
        CHECK(r.failure == PathFailure::NotACa);
    }
    // CA without keyCertSign.
    {
        CertSpec noKu{.cn = "No keyCertSign", .keyKind = "ec256", .ca = true, .issuer = &p.root, .keyUsage = "critical,digitalSignature"};
        const TestKey bad = makeCert(noKu);
        CertSpec ls{.cn = "Under No-KU", .keyKind = "ec256", .issuer = &bad};
        const TestKey leaf = makeCert(ls);
        const PathResult r = build(p, parse(*p.provider, leaf), {parse(*p.provider, bad)});
        CHECK(!r.anchored);
        CHECK(r.failure == PathFailure::KeyUsage);
    }
    // pathLenConstraint 0 on the intermediate, with a sub-CA below it.
    {
        CertSpec i0{.cn = "PathLen0", .keyKind = "ec256", .ca = true, .issuer = &p.root, .basicConstraints = "critical,CA:TRUE,pathlen:0"};
        const TestKey i0k = makeCert(i0);
        CertSpec sub{.cn = "Sub CA", .keyKind = "ec256", .ca = true, .issuer = &i0k};
        const TestKey subk = makeCert(sub);
        CertSpec ls{.cn = "Deep Leaf", .keyKind = "ec256", .issuer = &subk};
        const TestKey leaf = makeCert(ls);
        const PathResult r = build(p, parse(*p.provider, leaf), {parse(*p.provider, i0k), parse(*p.provider, subk)});
        CHECK(!r.anchored);
        CHECK(r.failure == PathFailure::PathLength);
        // pathlen:1 allows exactly that shape.
        CertSpec i1{.cn = "PathLen1", .keyKind = "ec256", .ca = true, .issuer = &p.root, .basicConstraints = "critical,CA:TRUE,pathlen:1"};
        const TestKey i1k = makeCert(i1);
        CertSpec sub1{.cn = "Sub CA 1", .keyKind = "ec256", .ca = true, .issuer = &i1k};
        const TestKey sub1k = makeCert(sub1);
        CertSpec l1{.cn = "Deep Leaf 1", .keyKind = "ec256", .issuer = &sub1k};
        const TestKey leaf1 = makeCert(l1);
        const PathResult ok = build(p, parse(*p.provider, leaf1), {parse(*p.provider, i1k), parse(*p.provider, sub1k)});
        CHECK(ok.anchored);
        CHECK_EQ(ok.chain.size(), std::size_t{4});
    }
}

RIVET_TEST(pathBuilderJudgesValidityAtTheGivenTimeNotNow) {
    Pki p;
    // Valid from 200 days ago to 100 days ago: expired now, valid historically.
    CertSpec ls{.cn = "Expired Leaf", .keyKind = "ec256", .notBeforeOffsetSec = -200 * kDay, .notAfterOffsetSec = -100 * kDay, .issuer = &p.inter};
    const TestKey leaf = makeCert(ls);
    const CertificateInfo li = parse(*p.provider, leaf);
    const auto now = std::chrono::system_clock::now();
    const PathResult past = build(p, li, {p.interInfo}, now - std::chrono::hours(24 * 150));
    CHECK(past.anchored);
    const PathResult current = build(p, li, {p.interInfo}, now);
    CHECK(!current.anchored);
    CHECK(current.failure == PathFailure::NotValidAtTime);
    const PathResult early = build(p, li, {p.interInfo}, now - std::chrono::hours(24 * 250));
    CHECK(!early.anchored);
    CHECK(early.failure == PathFailure::NotValidAtTime);
}

RIVET_TEST(pathBuilderRejectsAnUntrustedRootAndRecordsLimitations) {
    Pki p;
    TrustAnchorSet other;
    const TestKey stranger = makeCert({.cn = "Other Root", .keyKind = "ec256", .ca = true});
    other.add(parse(*p.provider, stranger));
    const PathResult r = build(p, p.leafInfo, {p.interInfo, p.rootInfo}, std::chrono::system_clock::now(), &other);
    CHECK(!r.anchored);
    CHECK(r.failure == PathFailure::NotAnchored || r.failure == PathFailure::NoIssuer);

    // nameConstraints are not enforced; the path says so.
    CertSpec nc{.cn = "NC CA", .keyKind = "ec256", .ca = true, .issuer = &p.root};
    nc.extraExt = {{NID_name_constraints, "critical,permitted;DNS:example.com"}};
    const TestKey nck = makeCert(nc);
    const TestKey leaf = makeCert({.cn = "NC Leaf", .keyKind = "ec256", .issuer = &nck});
    const PathResult lim = build(p, parse(*p.provider, leaf), {parse(*p.provider, nck)});
    CHECK(lim.anchored);
    CHECK(!lim.limitations.empty());
}

RIVET_TEST(pathBuilderHonoursMaxLengthAndCancellation) {
    Pki p;
    PathOptions o;
    o.anchors = &p.anchors;
    o.pool = {p.interInfo};
    o.maxLength = 2;
    CHECK(!buildValidationPath(p.leafInfo, o, *p.provider).anchored);
    o.maxLength = 3;
    CHECK(buildValidationPath(p.leafInfo, o, *p.provider).anchored);
    // Random garbage pools never crash.
    std::mt19937 rng(7);
    for (int i = 0; i < 20; ++i) {
        PathOptions g;
        g.anchors = &p.anchors;
        CertificateInfo junk = p.interInfo;
        junk.der.resize(rng() % junk.der.size());
        junk.issuerDer.clear();
        g.pool = {junk, p.interInfo};
        (void)buildValidationPath(p.leafInfo, g, *p.provider);
    }
}
