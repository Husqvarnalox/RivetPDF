// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "TestCertBuilder.hpp"
#include "crypto/Qualification.hpp"
#include "crypto/X509Parse.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

namespace {

constexpr const char* kQualifiedPolicy = "1.2.643.100.113.1";
const auto kNow = std::chrono::system_clock::time_point(std::chrono::sys_days{std::chrono::year{2026} / 3 / 1});

// Deterministic non-cryptographic stand-in for a digest: tests need distinct,
// stable fingerprints, not security.
CryptoResult<Bytes> fakeDigest(DigestAlgorithm a, ByteView d) {
    const std::size_t n = a == DigestAlgorithm::Sha1 ? 20 : 32;
    Bytes out(n);
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < n; ++i) {
        for (const std::uint8_t b : d) h = (h ^ b) * 1099511628211ULL;
        h ^= (i + 1) * 0x9E3779B97F4A7C15ULL;
        out[i] = static_cast<std::uint8_t>(h >> 56);
    }
    return out;
}

std::string hex(const Bytes& b) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t v : b) {
        out += kDigits[v >> 4];
        out += kDigits[v & 0xF];
    }
    return out;
}

struct Scenario {
    bool gost = true;
    bool identity = true;         // INN present with the CN
    bool signKeyUsage = true;
    bool withAki = true;          // leaf carries AKI == issuer SKI
    bool akiMismatch = false;     // AKI present but different from the issuer SKI
    bool policyInLeaf = true;     // leaf carries the qualified policy OID
    bool issuerInList = true;
    bool policyInList = true;
    bool cspInList = true;
    bool includeList = true;
    bool includeIssuer = true;
    ProviderAssurance provider = ProviderAssurance::CertifiedCspDeclared;
    std::string providerId = "cryptopro";
    bool expired = false;
};

struct Built {
    CertificateInfo leaf;
    CertificateInfo issuer;
    std::string listText;
    AccreditedCaList list;
    SignatureClassification result;
};

Built run(const Scenario& s) {
    Built out;
    CertBuild issuer;
    issuer.subject = {{"2.5.4.3", "Test Accredited CA"}, {"2.5.4.10", "Test Trust Org"}};
    issuer.issuer = issuer.subject;
    issuer.ca = true;
    issuer.keyUsage = keyusage::kKeyCertSign | keyusage::kCrlSign;
    issuer.ski = Bytes{0x10, 0x20, 0x30, 0x40};
    out.issuer = *parseX509(buildCertificate(issuer), fakeDigest);

    CertBuild leaf;
    leaf.subject = {{"2.5.4.3", "Иванов Иван"}};
    if (s.identity) leaf.subject.push_back({"1.2.643.3.131.1.1", "007707083893"});
    leaf.issuer = issuer.subject;
    if (s.gost) {
        leaf.spkiAlgOid = "1.2.643.7.1.1.1.1";
        leaf.spkiParams = der::sequence({der::oidElement("1.2.643.2.2.35.1"), der::oidElement("1.2.643.2.2.30.1")});
        leaf.keyBits = der::octetString(Bytes(64, 0x33));
        leaf.sigAlgOid = "1.2.643.7.1.1.3.2";
    } else {
        leaf.keyBits = Bytes{0x30, 0x06, 0x02, 0x01, 0x05, 0x02, 0x01, 0x03};
    }
    if (s.signKeyUsage) leaf.keyUsage = keyusage::kDigitalSignature | keyusage::kNonRepudiation;
    else leaf.keyUsage = keyusage::kKeyEncipherment;
    if (s.withAki) leaf.aki = s.akiMismatch ? Bytes{0x99, 0x98} : issuer.ski;
    if (s.policyInLeaf) leaf.policies = {"1.2.3.4", kQualifiedPolicy};
    if (s.expired) leaf.notAfter = std::chrono::system_clock::time_point(std::chrono::sys_days{std::chrono::year{2025} / 1 / 1});
    out.leaf = *parseX509(buildCertificate(leaf), fakeDigest);

    out.listText = "rivet-accredited-ca-list 1\n# test fixture, not a real list\nupdated 2026-01-01\nsource unit test\n";
    if (s.issuerInList)
        out.listText += "ca sha256=" + out.issuer.sha256Fingerprint + " ski=" + hex(out.issuer.subjectKeyId) +
                        " name=Test Accredited CA\n";
    if (s.policyInList) out.listText += std::string("policy ") + kQualifiedPolicy + "\n";
    if (s.cspInList) out.listText += "csp cryptopro\n";
    auto list = AccreditedCaList::parse(out.listText);
    CHECK(list.has_value());
    if (list) out.list = std::move(*list);

    ClassificationInput in;
    in.signer = &out.leaf;
    in.keyType = out.leaf.publicKeyType;
    in.provider = s.provider;
    in.providerId = s.providerId;
    in.validAt = kNow;
    in.list = s.includeList ? &out.list : nullptr;
    in.chain.push_back(out.leaf); // unrelated order: leaf first, issuer after an unrelated cert
    if (s.includeIssuer) in.chain.push_back(out.issuer);
    out.result = classify(in);
    return out;
}

void expectErr(std::string_view text, CryptoErrorCode code, std::string_view needle = {}) {
    auto r = AccreditedCaList::parse(text);
    CHECK(!r.has_value());
    if (!r) {
        CHECK_EQ(r.error().code, code);
        if (!needle.empty()) CHECK(r.error().message.find(needle) != std::string::npos);
    }
}

} // namespace

RIVET_TEST(classifyEveryClassIsReachable) {
    CHECK_EQ(classify({}).level, SignatureClass::Unknown);

    ClassificationInput rsa;
    rsa.keyType = PublicKeyType::Rsa;
    CHECK_EQ(classify(rsa).level, SignatureClass::SimpleCrypto);

    ClassificationInput gost;
    gost.signature = SignatureAlgorithm::Gost3410_2012_512;
    CHECK_EQ(classify(gost).level, SignatureClass::GostDigitalSignature);

    // identity + custody, RSA key on a token: advanced
    Scenario adv;
    adv.gost = false;
    adv.provider = ProviderAssurance::HardwareToken;
    adv.includeList = false;
    CHECK_EQ(run(adv).result.level, SignatureClass::AdvancedUnqualified);

    const Built q = run({});
    CHECK_EQ(q.result.level, SignatureClass::QualifiedEvidence);
    const auto& e = q.result.evidence;
    CHECK(e.gostAlgorithm && e.identityAttributes && e.signatureKeyUsage && e.nonExportableKeyCustody);
    CHECK(e.issuerInAccreditedList && e.qualifiedPolicyPresent && e.certifiedCspDeclared && e.certificateValidAtTime);
    CHECK(!q.result.reasons.empty());
    CHECK_EQ(q.result.disclaimer, std::string(kLegalDisclaimer));
}

RIVET_TEST(classifyNegativeCases) {
    struct Case {
        const char* name;
        Scenario s;
        SignatureClass expected;
    };
    std::vector<Case> cases;
    auto add = [&](const char* name, SignatureClass expected, auto mutate) {
        Scenario s;
        mutate(s);
        cases.push_back({name, s, expected});
    };
    add("gost oid only", SignatureClass::GostDigitalSignature, [](Scenario& s) {
        s.identity = false;
        s.signKeyUsage = false;
        s.provider = ProviderAssurance::Software;
        s.includeList = false;
    });
    add("policy present, issuer not listed", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.issuerInList = false; });
    add("issuer listed, no policy in list", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.policyInList = false; });
    add("issuer listed, leaf lacks policy", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.policyInLeaf = false; });
    add("all but csp declared (token)", SignatureClass::AdvancedUnqualified,
        [](Scenario& s) { s.provider = ProviderAssurance::HardwareToken; });
    add("csp declared but not listed", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.cspInList = false; });
    add("csp id differs from list", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.providerId = "other"; });
    add("empty provider id", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.providerId.clear(); });
    add("software provider, everything else", SignatureClass::GostDigitalSignature,
        [](Scenario& s) { s.provider = ProviderAssurance::Software; });
    add("expired certificate", SignatureClass::GostDigitalSignature, [](Scenario& s) { s.expired = true; });
    add("issuer by name only, no AKI", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.withAki = false; });
    add("issuer by name only, AKI differs", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.akiMismatch = true; });
    add("issuer missing from chain", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.includeIssuer = false; });
    add("no list", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.includeList = false; });
    add("empty list", SignatureClass::AdvancedUnqualified, [](Scenario& s) {
        s.issuerInList = s.policyInList = s.cspInList = false;
    });
    add("no signing key usage", SignatureClass::GostDigitalSignature, [](Scenario& s) { s.signKeyUsage = false; });
    add("no identity attributes", SignatureClass::GostDigitalSignature, [](Scenario& s) { s.identity = false; });
    add("rsa with full list evidence", SignatureClass::AdvancedUnqualified, [](Scenario& s) { s.gost = false; });
    add("rsa software, no identity", SignatureClass::SimpleCrypto, [](Scenario& s) {
        s.gost = false;
        s.identity = false;
        s.provider = ProviderAssurance::Software;
    });
    add("platform keystore with all evidence", SignatureClass::AdvancedUnqualified,
        [](Scenario& s) { s.provider = ProviderAssurance::PlatformKeystore; });
    for (const Case& c : cases) {
        const Built b = run(c.s);
        if (b.result.level != c.expected) std::fprintf(stderr, "    case failed: %s\n", c.name);
        CHECK_EQ(b.result.level, c.expected);
    }
}

RIVET_TEST(classifyIssuerSelfAndTamperedList) {
    // The signer itself in the "chain" never counts as its issuer.
    Built q = run({});
    ClassificationInput in;
    in.signer = &q.leaf;
    in.provider = ProviderAssurance::CertifiedCspDeclared;
    in.providerId = "cryptopro";
    in.validAt = kNow;
    in.list = &q.list;
    in.chain = {q.leaf};
    CHECK_EQ(classify(in).level, SignatureClass::AdvancedUnqualified);
    in.chain = {q.leaf, q.issuer};
    CHECK_EQ(classify(in).level, SignatureClass::QualifiedEvidence);

    // One altered fingerprint digit: the list still parses but no longer lists the issuer
    // (the SKI is also dropped from the tampered line, as an attacker would not know it).
    std::string text = q.listText;
    const std::size_t at = text.find("sha256=") + 7;
    text[at] = text[at] == 'a' ? 'b' : 'a';
    const std::size_t ski = text.find(" ski=");
    text.erase(ski, text.find(" name=") - ski);
    auto tampered = AccreditedCaList::parse(text);
    CHECK(tampered.has_value());
    if (tampered) {
        in.list = &*tampered;
        CHECK_EQ(classify(in).level, SignatureClass::AdvancedUnqualified);
    }
    // A corrupted fingerprint digit that is not hex is a parse error.
    std::string broken = q.listText;
    broken[broken.find("sha256=") + 7] = 'z';
    expectErr(broken, CryptoErrorCode::CorruptData, "line 5");
}

RIVET_TEST(classifyNameStringsAreStable) {
    CHECK_EQ(machineKey(SignatureClass::QualifiedEvidence), std::string_view("qualified_evidence"));
    CHECK_EQ(machineKey(SignatureClass::Unknown), std::string_view("unknown"));
    CHECK_EQ(machineKey(SignatureClass::SimpleCrypto), std::string_view("simple_crypto"));
    CHECK_EQ(machineKey(SignatureClass::GostDigitalSignature), std::string_view("gost_digital_signature"));
    CHECK_EQ(machineKey(SignatureClass::AdvancedUnqualified), std::string_view("advanced_unqualified"));
    CHECK(!toString(SignatureClass::QualifiedEvidence).empty());
}

RIVET_TEST(accreditedListParsesAndMerges) {
    const std::string fp(64, 'A');
    auto r = AccreditedCaList::parse("\r\n# leading comment\r\nrivet-accredited-ca-list 1\r\n"
                                     "updated 2026-02-03\r\nsource Test, fixture # not a comment\r\n"
                                     "ca sha256=" + fp + " name=First CA  \r\n"
                                     "ca sha256=" + std::string(64, 'a') + " ski=0A0b name=Second name\r\n"
                                     "ca sha256=" + std::string(64, 'b') + "\r\n"
                                     "   policy 1.2.643.100.113.1\r\npolicy 1.2.643.100.113.1\r\ncsp cryptopro\r\n");
    CHECK(r.has_value());
    if (!r) return;
    CHECK_EQ(r->formatVersion, 1);
    CHECK_EQ(r->updated, std::string("2026-02-03"));
    CHECK_EQ(r->source, std::string("Test, fixture # not a comment"));
    CHECK_EQ(r->entries.size(), std::size_t{2}); // A and a are the same fingerprint (lower-cased) and merge
    CHECK_EQ(r->entries[0].sha256Fingerprint, std::string(64, 'a'));
    CHECK_EQ(r->entries[0].name, std::string("First CA"));
    CHECK_EQ(r->entries[0].subjectKeyId, (Bytes{0x0A, 0x0B}));
    CHECK(r->entries[1].name.empty());
    CHECK_EQ(r->qualifiedPolicyOids.size(), std::size_t{1});
    CHECK(r->qualifiedPolicy("1.2.643.100.113.1"));
    CHECK(!r->qualifiedPolicy("1.2.3"));
    CHECK(r->certifiedCspProviderIds.contains("cryptopro"));

    CertificateInfo c;
    c.sha256Fingerprint = std::string(64, 'b');
    CHECK(r->matchesIssuer(c));
    c.sha256Fingerprint.clear();
    CHECK(!r->matchesIssuer(c));
    c.subjectKeyId = Bytes{0x0A, 0x0B};
    CHECK(r->matchesIssuer(c));
    c.subjectKeyId = Bytes{0x0A};
    CHECK(!r->matchesIssuer(c));

    auto header = AccreditedCaList::parse("rivet-accredited-ca-list 1");
    CHECK(header.has_value());
    if (header) CHECK(header->entries.empty());
}

RIVET_TEST(accreditedListRejectsMalformed) {
    const std::string fp(64, '1');
    expectErr("", CryptoErrorCode::CorruptData);
    expectErr("# only a comment\n", CryptoErrorCode::CorruptData);
    expectErr("rivet-accredited-ca-list 2\n", CryptoErrorCode::CorruptData, "line 1");
    expectErr("rivet-accredited-ca-list\n", CryptoErrorCode::CorruptData, "line 1");
    expectErr("something else\n", CryptoErrorCode::CorruptData, "line 1");
    expectErr("rivet-accredited-ca-list 1\n\n# c\nbogus x\n", CryptoErrorCode::CorruptData, "line 4");
    expectErr("rivet-accredited-ca-list 1\nupdated 2026\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\nupdated 2026-01-01\nupdated 2026-01-02\n", CryptoErrorCode::CorruptData, "line 3");
    expectErr("rivet-accredited-ca-list 1\nsource\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\nca name=x\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\nca sha256=abcd name=x\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\nca sha256=" + fp + " ski=xyz\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\nca sha256=" + fp + " junk\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\nca sha256=" + fp + " ski=01\nca sha256=" + fp + " ski=02\n",
              CryptoErrorCode::CorruptData, "line 3");
    expectErr("rivet-accredited-ca-list 1\npolicy 1..2\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\npolicy abc\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\ncsp bad id\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr("rivet-accredited-ca-list 1\ncsp\n", CryptoErrorCode::CorruptData, "line 2");
    expectErr(std::string("rivet-accredited-ca-list 1\nsource a") + '\0' + "b\n", CryptoErrorCode::CorruptData, "line 2");
}

RIVET_TEST(accreditedListBounds) {
    // oversized: more than 1 MiB
    std::string big = "rivet-accredited-ca-list 1\n";
    big.append(AccreditedCaList::kMaxBytes, '#');
    expectErr(big, CryptoErrorCode::LimitExceeded);
    // too many lines (each line is small, total stays below 1 MiB)
    std::string many = "rivet-accredited-ca-list 1\n";
    for (std::size_t i = 0; i < AccreditedCaList::kMaxLines; ++i) many += "#\n";
    expectErr(many, CryptoErrorCode::LimitExceeded);
    // exactly at the line bound is accepted
    std::string ok = "rivet-accredited-ca-list 1\n";
    for (std::size_t i = 1; i < AccreditedCaList::kMaxLines; ++i) ok += "#\n";
    CHECK(AccreditedCaList::parse(ok).has_value());
}

RIVET_TEST(accreditedListFileLoad) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "rivet_ca_list_test";
    std::error_code ec;
    fs::create_directories(dir, ec);

    auto missing = AccreditedCaList::loadFile(dir / "does-not-exist.txt");
    CHECK(!missing.has_value());
    if (!missing) CHECK_EQ(missing.error().code, CryptoErrorCode::Io);

    const fs::path file = dir / "list.txt";
    {
        std::ofstream out(file, std::ios::binary);
        out << "rivet-accredited-ca-list 1\nsource file test\npolicy 1.2.3.4\n";
    }
    auto loaded = AccreditedCaList::loadFile(file);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK_EQ(loaded->source, std::string("file test"));
        CHECK(loaded->qualifiedPolicy("1.2.3.4"));
    }
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << "not a list\n";
    }
    auto bad = AccreditedCaList::loadFile(file);
    CHECK(!bad.has_value());
    if (!bad) CHECK_EQ(bad.error().code, CryptoErrorCode::CorruptData);
    fs::remove_all(dir, ec);
}
