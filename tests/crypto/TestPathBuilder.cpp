// SPDX-License-Identifier: MPL-2.0
// Certification path builder for historical validation (ADR-0044, ADR-0045).

#include "RivetTest.h"

#include "TestSupport.hpp"
#include "crypto/Der.hpp"
#include "crypto/NameConstraints.hpp"
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

    // Certificate policies are not evaluated; the path says so.
    const PathResult lim = build(p, p.leafInfo, {p.interInfo});
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

// ---- nameConstraints (RFC 5280 4.2.1.10) ---------------------------------------------

namespace {

// Builds root -> NC CA (carrying `constraint`) -> leaf (with `leafSan`, empty: none) and builds the path.
PathResult ncPath(Pki& p, const std::string& constraint, const std::string& leafSan, const std::string& leafCn = "NC Leaf",
                  std::vector<CertSpec::RawExt> raw = {}) {
    CertSpec nc{.cn = "NC CA", .keyKind = "ec256", .ca = true, .issuer = &p.root};
    if (!constraint.empty()) nc.extraExt = {{NID_name_constraints, constraint}};
    nc.rawExt = std::move(raw);
    const TestKey nck = makeCert(nc);
    CertSpec ls{.cn = leafCn, .keyKind = "ec256", .issuer = &nck};
    if (!leafSan.empty()) ls.extraExt = {{NID_subject_alt_name, leafSan}};
    const TestKey leaf = makeCert(ls);
    return build(p, parse(*p.provider, leaf), {parse(*p.provider, nck)});
}

} // namespace

RIVET_TEST(nameConstraintsDnsPermittedAndExcluded) {
    Pki p;
    const std::string permit = "critical,permitted;DNS:example.com";
    PathResult r = ncPath(p, permit, "DNS:example.com");
    CHECK(r.anchored);
    r = ncPath(p, permit, "DNS:www.example.com,DNS:A.B.EXAMPLE.COM");
    CHECK(r.anchored);
    r = ncPath(p, permit, "DNS:www.example.com,DNS:evil.org");
    CHECK(!r.anchored);
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, permit, "DNS:badexample.com"); // not on a label boundary
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, permit, "DNS:*.org"); // a wildcard reaching outside the subtree
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, permit, ""); // no names of the constrained type: nothing to check
    CHECK(r.anchored);

    const std::string exclude = "critical,excluded;DNS:internal.example.com";
    r = ncPath(p, exclude, "DNS:www.example.com");
    CHECK(r.anchored);
    r = ncPath(p, exclude, "DNS:db.internal.example.com");
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "critical,permitted;DNS:.example.com", "DNS:example.com"); // leading dot: subdomains only
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "critical,permitted;DNS:.example.com", "DNS:a.example.com");
    CHECK(r.anchored);
    // A constraint on another type does not restrict DNS names.
    r = ncPath(p, "critical,permitted;IP:10.0.0.0/255.0.0.0", "DNS:anything.test");
    CHECK(r.anchored);
}

RIVET_TEST(nameConstraintsEmailIpAndUri) {
    Pki p;
    PathResult r = ncPath(p, "critical,permitted;email:example.com", "email:bob@example.com");
    CHECK(r.anchored);
    r = ncPath(p, "critical,permitted;email:example.com", "email:bob@sub.example.com"); // host form: exact host only
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "critical,permitted;email:.example.com", "email:bob@sub.example.com");
    CHECK(r.anchored);
    r = ncPath(p, "critical,permitted;email:.example.com", "email:bob@example.com");
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "critical,excluded;email:bob@example.com", "email:bob@example.com");
    CHECK(r.failure == PathFailure::NameConstraints);

    r = ncPath(p, "critical,permitted;IP:10.0.0.0/255.0.0.0", "IP:10.1.2.3");
    CHECK(r.anchored);
    r = ncPath(p, "critical,permitted;IP:10.0.0.0/255.0.0.0", "IP:11.1.2.3");
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "critical,permitted;IP:10.0.0.0/255.0.0.0", "IP:::1"); // other family: outside the permitted set
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "critical,excluded;IP:192.168.0.0/255.255.0.0", "IP:192.168.4.4");
    CHECK(r.failure == PathFailure::NameConstraints);

    r = ncPath(p, "critical,permitted;URI:.example.com", "URI:https://user@a.example.com:8443/x?y#z");
    CHECK(r.anchored);
    r = ncPath(p, "critical,permitted;URI:.example.com", "URI:https://evil.org/");
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "critical,permitted;URI:.example.com", "URI:urn:isbn:123"); // no authority host
    CHECK(r.failure == PathFailure::NameConstraints);
}

RIVET_TEST(nameConstraintsDirectoryNameAndSubjectEmail) {
    Pki p;
    namespace d = rivet::crypto::der;
    auto cnName = [](const std::string& cn) {
        const Bytes v(cn.begin(), cn.end());
        return d::sequence({d::setOf({d::sequence({d::oidElement("2.5.4.3"), d::tlv(d::kUtf8String, v)})})});
    };
    auto nc = [&](const char* tagKind, const Bytes& name) {
        const Bytes gn = d::tlv(0xA4, name);
        const Bytes subtree = d::sequence({gn});
        return std::vector<CertSpec::RawExt>{{NID_name_constraints, true, d::sequence({d::tlv(tagKind[0] == 'p' ? 0xA0 : 0xA1, subtree)})}};
    };
    PathResult r = ncPath(p, "", "", "NC Leaf", nc("permitted", cnName("NC Leaf")));
    CHECK(r.anchored);
    r = ncPath(p, "", "", "NC Leaf", nc("permitted", cnName("Somebody Else")));
    CHECK(r.failure == PathFailure::NameConstraints);
    r = ncPath(p, "", "", "nc  LEAF", nc("permitted", cnName("NC Leaf"))); // case and space insensitive
    CHECK(r.anchored);
    r = ncPath(p, "", "", "NC Leaf", nc("excluded", cnName("NC Leaf")));
    CHECK(r.failure == PathFailure::NameConstraints);
    // An empty directoryName constraint matches every subject.
    r = ncPath(p, "", "", "NC Leaf", nc("excluded", d::sequence({})));
    CHECK(r.failure == PathFailure::NameConstraints);
}

RIVET_TEST(nameConstraintsMalformedOrUnsupportedFailClosed) {
    Pki p;
    namespace d = rivet::crypto::der;
    // An empty NameConstraints carries no subtree: refused as malformed.
    PathResult r = ncPath(p, "", "", "NC Leaf", {{NID_name_constraints, true, d::sequence({})}});
    CHECK(!r.anchored);
    CHECK(r.failure == PathFailure::NameConstraints);
    // minimum != 0 is forbidden by RFC 5280.
    const Bytes dns = d::tlv(0x82, Bytes{'a', '.', 'b'});
    const Bytes subtree = d::sequence({dns, d::tlv(0x80, d::integerFromInt(1))});
    r = ncPath(p, "", "", "NC Leaf", {{NID_name_constraints, true, d::sequence({d::tlv(0xA0, subtree)})}});
    CHECK(r.failure == PathFailure::NameConstraints);
    // A constrained otherName type that the leaf uses cannot be evaluated: refused.
    const Bytes other = d::tlv(0xA0, d::concat({d::oidElement("1.2.3.4"), d::tlv(0xA0, d::tlv(d::kUtf8String, Bytes{'x'}))}));
    const Bytes otherTree = d::sequence({other});
    r = ncPath(p, "", "DNS:x.test,otherName:1.2.3.4;UTF8:x",
               "NC Leaf", {{NID_name_constraints, true, d::sequence({d::tlv(0xA1, otherTree)})}});
    CHECK(r.failure == PathFailure::NameConstraints);
}

RIVET_TEST(nameConstraintsOnTheTrustAnchorAndSelfIssuedExemption) {
    // A constraint carried by the root applies to everything below it.
    const auto provider = makeDefaultProvider();
    CertSpec rs{.cn = "NC Root", .keyKind = "ec256", .ca = true, .notBeforeOffsetSec = -400 * kDay};
    rs.extraExt = {{NID_name_constraints, "critical,permitted;DNS:example.com"}};
    const TestKey root = makeCert(rs);
    CertSpec is{.cn = "NC Inter", .keyKind = "ec256", .ca = true, .issuer = &root};
    const TestKey inter = makeCert(is);
    auto leafWith = [&](const std::string& san) {
        CertSpec ls{.cn = "L", .keyKind = "ec256", .issuer = &inter};
        ls.extraExt = {{NID_subject_alt_name, san}};
        return makeCert(ls);
    };
    TrustAnchorSet anchors;
    anchors.add(parse(*provider, root));
    auto run = [&](const TestKey& leaf) {
        PathOptions o;
        o.anchors = &anchors;
        o.pool = {parse(*provider, inter)};
        return buildValidationPath(parse(*provider, leaf), o, *provider);
    };
    CHECK(run(leafWith("DNS:ok.example.com")).anchored);
    const PathResult bad = run(leafWith("DNS:bad.org"));
    CHECK(!bad.anchored);
    CHECK(bad.failure == PathFailure::NameConstraints);
    CHECK(hasNameConstraintsExtension(certDer(root)));
    CHECK(!hasNameConstraintsExtension(certDer(inter)));
}
