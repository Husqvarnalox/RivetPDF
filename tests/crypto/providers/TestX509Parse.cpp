// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "TestCertBuilder.hpp"
#include "crypto/X509Parse.hpp"

#ifdef RIVET_TEST_CRYPTO_OPENSSL
#include "TestSupport.hpp"
#include "crypto/Providers.hpp"
#endif

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

namespace {

const std::string kSurname = "Иванов";
const std::string kGiven = "Иван Иванович";

CertBuild gostBuild() {
    CertBuild b;
    b.subject = {
        {"2.5.4.3", kSurname + " " + kGiven},
        {"2.5.4.4", kSurname},
        {"2.5.4.42", kGiven},
        {"2.5.4.10", "Test Org", der::kUtf8String},
        {"1.2.643.3.131.1.1", "007707083893"},
        {"1.2.643.100.3", "12345678901"},
        {"1.2.643.100.1", "1027700132195", 0x13}, // PrintableString
        {"1.2.643.100.5", "304500116000157"},
        {"2.5.4.7", std::string("M\xFC") + "nchen", 0x14}, // TeletexString, ISO 8859-1
        {"2.5.4.8", std::string("\x00" "A" "\x00" "b", 4), 0x1E}, // BMPString
    };
    b.issuer = {{"2.5.4.3", "Test Issuing CA"}, {"2.5.4.10", "Test Issuer Org"}};
    b.spkiAlgOid = "1.2.643.7.1.1.1.1";
    b.spkiParams = der::sequence({der::oidElement("1.2.643.2.2.35.1"), der::oidElement("1.2.643.2.2.30.1")});
    b.keyBits = der::octetString(Bytes(64, 0x11));
    b.sigAlgOid = "1.2.643.7.1.1.3.2";
    b.keyUsage = keyusage::kDigitalSignature | keyusage::kNonRepudiation;
    b.policies = {"1.2.643.100.113.1", "1.2.643.100.113.2"};
    b.ski = Bytes{0xAA, 0xBB, 0xCC};
    b.aki = Bytes{0x01, 0x02};
    b.eku = {"1.3.6.1.5.5.7.3.4", "1.3.6.1.5.5.7.3.2"};
    b.ca = false;
    return b;
}

} // namespace

RIVET_TEST(x509GostCertificateParses) {
    const Bytes der = buildCertificate(gostBuild());
    X509Extras ex;
    auto r = parseX509(der, {}, &ex);
    CHECK(r.has_value());
    if (!r) return;
    const CertificateInfo& c = *r;
    CHECK_EQ(c.der, der);
    CHECK_EQ(c.publicKeyType, PublicKeyType::Gost3410_2012_256);
    CHECK_EQ(c.keyBits, 256);
    CHECK_EQ(c.signatureAlgorithmOid, std::string("1.2.643.7.1.1.3.2"));
    CHECK_EQ(c.commonName.value_or(""), kSurname + " " + kGiven);
    CHECK_EQ(c.organization.value_or(""), std::string("Test Org"));
    CHECK(!c.email.has_value());
    CHECK_EQ(c.russian.inn.value_or(""), std::string("007707083893"));
    CHECK_EQ(c.russian.snils.value_or(""), std::string("12345678901"));
    CHECK_EQ(c.russian.ogrn.value_or(""), std::string("1027700132195"));
    CHECK_EQ(c.russian.ogrnip.value_or(""), std::string("304500116000157"));
    CHECK(!c.russian.innLe.has_value());
    CHECK(c.hasKeyUsage);
    CHECK_EQ(c.keyUsage, static_cast<std::uint16_t>(keyusage::kDigitalSignature | keyusage::kNonRepudiation));
    CHECK_EQ(c.extendedKeyUsageOids.size(), std::size_t{2});
    CHECK_EQ(c.extendedKeyUsageOids[0], std::string("1.3.6.1.5.5.7.3.4"));
    CHECK(!c.isCa);
    CHECK(!c.pathLength.has_value());
    CHECK_EQ(c.subjectKeyId, (Bytes{0xAA, 0xBB, 0xCC}));
    CHECK_EQ(c.authorityKeyId, (Bytes{0x01, 0x02}));
    CHECK_EQ(c.serial, (Bytes{0x01, 0x02, 0x03}));
    CHECK_EQ(c.serialHex, std::string("010203"));
    CHECK(c.sha256Fingerprint.empty());
    CHECK(c.issuer == "O=Test Issuer Org,CN=Test Issuing CA");
    CHECK(c.subject.find("INN=007707083893") != std::string::npos);
    CHECK(c.subject.find("OGRNIP=304500116000157,OGRN=1027700132195,SNILS=12345678901,INN=007707083893") != std::string::npos);
    CHECK(c.subject.find("O=Test Org") != std::string::npos);
    CHECK(c.subject.find("\\D0\\98") != std::string::npos); // non-ASCII escaped like OpenSSL RFC 2253
    CHECK(c.notBefore < c.notAfter);
    CHECK(!c.isSelfIssued());

    CHECK_EQ(ex.spkiAlgorithmOid, std::string("1.2.643.7.1.1.1.1"));
    CHECK_EQ(ex.spkiParameterOids.size(), std::size_t{2});
    CHECK_EQ(ex.spkiParameterOids[0], std::string("1.2.643.2.2.35.1"));
    CHECK_EQ(ex.spkiParameterOids[1], std::string("1.2.643.2.2.30.1"));
    CHECK_EQ(ex.publicKeyBits, der::octetString(Bytes(64, 0x11)));
    CHECK(!ex.spkiDer.empty() && ex.spkiDer[0] == der::kSequence);
    CHECK_EQ(ex.certificatePolicyOids.size(), std::size_t{2});
    CHECK_EQ(ex.certificatePolicyOids[1], std::string("1.2.643.100.113.2"));

    CHECK_EQ(ex.subjectAttributes.size(), std::size_t{10});
    CHECK_EQ(ex.subjectAttributes[0].oid, std::string("2.5.4.3"));
    CHECK_EQ(ex.subjectAttributes[1].value, kSurname);
    CHECK_EQ(ex.subjectAttributes[2].value, kGiven);
    CHECK_EQ(ex.subjectAttributes[6].oid, std::string("1.2.643.100.1"));
    CHECK_EQ(ex.subjectAttributes[8].value, std::string("München"));
    CHECK_EQ(ex.subjectAttributes[9].value, std::string("Ab"));
    CHECK_EQ(ex.issuerAttributes.size(), std::size_t{2});
    CHECK_EQ(ex.issuerAttributes[1].value, std::string("Test Issuer Org"));
}

RIVET_TEST(x509Gost512AndLegalEntityInn) {
    CertBuild b = gostBuild();
    b.spkiAlgOid = "1.2.643.7.1.1.1.2";
    b.keyBits = der::octetString(Bytes(128, 0x22));
    b.sigAlgOid = "1.2.643.7.1.1.3.3";
    b.subject = {{"2.5.4.3", "Org CA"}, {"1.2.643.100.4", "7707083893"}};
    auto r = parseX509(buildCertificate(b));
    CHECK(r.has_value());
    if (!r) return;
    CHECK_EQ(r->publicKeyType, PublicKeyType::Gost3410_2012_512);
    CHECK_EQ(r->keyBits, 512);
    CHECK_EQ(r->russian.innLe.value_or(""), std::string("7707083893"));
    CHECK(!r->russian.inn.has_value());
    // 1.2.643.100.4 has no short name: dotted OID with the value's DER dump
    CHECK(r->subject.find("1.2.643.100.4=#0C0A37373037303833383933") != std::string::npos);
}

RIVET_TEST(x509CaExtensionsAndMultiValuedRdn) {
    CertBuild b;
    b.subject = {{"2.5.4.3", "Root"}, {"2.5.4.10", "A+B, Inc ", der::kUtf8String, true}, {"2.5.4.6", "RU"}};
    b.issuer = b.subject;
    b.ca = true;
    b.pathLen = 1;
    b.keyUsage = keyusage::kKeyCertSign | keyusage::kCrlSign | keyusage::kDecipherOnly;
    auto r = parseX509(buildCertificate(b));
    CHECK(r.has_value());
    if (!r) return;
    CHECK(r->isCa);
    CHECK_EQ(r->pathLength.value_or(-1), 1);
    CHECK_EQ(r->keyUsage,
             static_cast<std::uint16_t>(keyusage::kKeyCertSign | keyusage::kCrlSign | keyusage::kDecipherOnly));
    CHECK(r->isSelfIssued());
    CHECK_EQ(r->subject, std::string("C=RU,O=A\\+B\\, Inc\\ +CN=Root"));
    CHECK_EQ(r->publicKeyType, PublicKeyType::Rsa);
}

RIVET_TEST(x509FingerprintsUseDigestFn) {
    const Bytes der = buildCertificate(gostBuild());
    int calls = 0;
    DigestFn fn = [&](DigestAlgorithm a, ByteView) -> CryptoResult<Bytes> {
        ++calls;
        return a == DigestAlgorithm::Sha256 ? Bytes(32, 0xAB) : Bytes(20, 0x0F);
    };
    auto r = parseX509(der, fn);
    CHECK(r.has_value());
    if (!r) return;
    CHECK_EQ(calls, 2);
    std::string expected256;
    for (int i = 0; i < 32; ++i) expected256 += "ab";
    CHECK_EQ(r->sha256Fingerprint, expected256);
    CHECK_EQ(r->sha1Fingerprint, std::string("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"));

    DigestFn failing = [](DigestAlgorithm, ByteView) -> CryptoResult<Bytes> {
        return fail(CryptoErrorCode::BackendUnavailable, "none");
    };
    auto r2 = parseX509(der, failing);
    CHECK(r2.has_value());
    if (r2) CHECK(r2->sha256Fingerprint.empty() && r2->sha1Fingerprint.empty());
}

RIVET_TEST(x509RejectsMalformedInput) {
    auto expectCorrupt = [](ByteView in) {
        auto r = parseX509(in);
        CHECK(!r.has_value());
        if (!r) CHECK_EQ(r.error().code, CryptoErrorCode::CorruptData);
    };
    expectCorrupt(Bytes{});
    expectCorrupt(Bytes{0x30});
    expectCorrupt(Bytes{0x30, 0x03, 0x02, 0x01, 0x00});
    expectCorrupt(Bytes(64, 0xFF));
    expectCorrupt(Bytes(64, 0x00));
    expectCorrupt(Bytes{0x30, 0x80, 0x00, 0x00});
    expectCorrupt(der::sequence({der::sequence({}), der::sequence({}), bitString(Bytes{1})}));
    // oversized input
    expectCorrupt(Bytes((std::size_t{16} << 20) + 1, 0x30));
    // length claims more than present
    Bytes huge{0x30, 0x84, 0x7F, 0xFF, 0xFF, 0xFF, 0x00};
    expectCorrupt(huge);
    // deep nesting
    Bytes nested = Bytes{0x05, 0x00};
    for (int i = 0; i < 100; ++i) nested = der::sequence({nested});
    expectCorrupt(nested);
}

RIVET_TEST(x509TruncationAtEveryOffsetIsTypedError) {
    const Bytes good = buildCertificate(gostBuild());
    CHECK(parseX509(good).has_value());
    for (std::size_t n = 0; n < good.size(); ++n) {
        auto r = parseX509(ByteView(good.data(), n));
        CHECK(!r.has_value());
        if (!r) CHECK_EQ(r.error().code, CryptoErrorCode::CorruptData);
    }
}

RIVET_TEST(x509ByteFlipsNeverCrash) {
    const Bytes good = buildCertificate(gostBuild());
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < good.size(); ++i) {
        for (const std::uint8_t mask : {std::uint8_t{0xFF}, std::uint8_t{0x01}, std::uint8_t{0x80}}) {
            Bytes bad = good;
            bad[i] = static_cast<std::uint8_t>(bad[i] ^ mask);
            X509Extras ex;
            auto r = parseX509(bad, {}, &ex);
            if (r) ++accepted;
            else CHECK(!r.error().message.empty());
        }
    }
    CHECK(accepted < good.size() * 3); // some flips (inside strings) legitimately parse
}

RIVET_TEST(x509TrailingBytesAreIgnored) {
    Bytes good = buildCertificate(gostBuild());
    const std::size_t n = good.size();
    good.push_back(0x00);
    good.push_back(0x01);
    auto r = parseX509(good);
    CHECK(r.has_value());
    if (r) CHECK_EQ(r->der.size(), n);
}

#ifdef RIVET_TEST_CRYPTO_OPENSSL

namespace {

void compareInfo(const CertificateInfo& a, const CertificateInfo& b) {
    CHECK_EQ(a.der, b.der);
    CHECK_EQ(a.subject, b.subject);
    CHECK_EQ(a.issuer, b.issuer);
    CHECK_EQ(a.subjectDer, b.subjectDer);
    CHECK_EQ(a.issuerDer, b.issuerDer);
    CHECK_EQ(a.serial, b.serial);
    CHECK_EQ(a.serialHex, b.serialHex);
    CHECK(a.notBefore == b.notBefore);
    CHECK(a.notAfter == b.notAfter);
    CHECK_EQ(a.hasKeyUsage, b.hasKeyUsage);
    CHECK_EQ(a.keyUsage, b.keyUsage);
    CHECK_EQ(a.extendedKeyUsageOids, b.extendedKeyUsageOids);
    CHECK_EQ(a.isCa, b.isCa);
    CHECK_EQ(a.pathLength, b.pathLength);
    CHECK_EQ(a.sha1Fingerprint, b.sha1Fingerprint);
    CHECK_EQ(a.sha256Fingerprint, b.sha256Fingerprint);
    CHECK_EQ(a.subjectKeyId, b.subjectKeyId);
    CHECK_EQ(a.authorityKeyId, b.authorityKeyId);
    CHECK_EQ(a.publicKeyType, b.publicKeyType);
    CHECK_EQ(a.keyBits, b.keyBits);
    CHECK_EQ(a.signatureAlgorithmOid, b.signatureAlgorithmOid);
    CHECK_EQ(a.commonName, b.commonName);
    CHECK_EQ(a.email, b.email);
    CHECK_EQ(a.organization, b.organization);
    CHECK_EQ(a.russian.inn, b.russian.inn);
    CHECK_EQ(a.russian.snils, b.russian.snils);
    CHECK_EQ(a.russian.ogrn, b.russian.ogrn);
    CHECK_EQ(a.russian.ogrnip, b.russian.ogrnip);
    CHECK_EQ(a.russian.innLe, b.russian.innLe);
}

void expectEquivalent(const Bytes& der) {
    auto provider = makeDefaultProvider();
    DigestFn fn = [provider](DigestAlgorithm a, ByteView d) { return provider->digest(a, d); };
    auto mine = parseX509(der, fn);
    auto theirs = provider->parseCertificate(der);
    CHECK(mine.has_value());
    CHECK(theirs.has_value());
    if (mine && theirs) compareInfo(*mine, *theirs);
}

// A certificate with unusual subject content, built directly with OpenSSL.
Bytes oddSubjectCertificate() {
    PkeyPtr key = makeKey("ec256");
    X509Ptr cert(X509_new());
    X509* c = cert.get();
    X509_set_version(c, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(c), 0x80FF01); // high bit set: leading zero in DER
    X509_gmtime_adj(X509_getm_notBefore(c), -100);
    X509_gmtime_adj(X509_getm_notAfter(c), 100000);
    X509_set_pubkey(c, key.get());
    X509_NAME* name = X509_NAME_new();
    auto add = [&](const char* oid, int type, const std::string& v) {
        ASN1_OBJECT* obj = OBJ_txt2obj(oid, 1);
        X509_NAME_add_entry_by_OBJ(name, obj, type, reinterpret_cast<const unsigned char*>(v.data()),
                                   static_cast<int>(v.size()), -1, 0);
        ASN1_OBJECT_free(obj);
    };
    add("C", V_ASN1_PRINTABLESTRING, "RU");
    add("2.5.4.10", V_ASN1_UTF8STRING, "ООО Ромашка, \"Test\"");
    add("2.5.4.11", V_ASN1_UTF8STRING, " lead and trail ");
    add("2.5.4.3", V_ASN1_UTF8STRING, "#Иванов <ivan>;+x");
    add("1.2.643.100.4", V_ASN1_UTF8STRING, "7707083893");
    add("1.2.643.100.1", V_ASN1_NUMERICSTRING, "1027700132195");
    add("1.2.643.100.5", V_ASN1_UTF8STRING, "304500116000157");
    add("1.2.643.3.131.1.1", V_ASN1_UTF8STRING, "007707083893");
    add("1.2.643.100.3", V_ASN1_UTF8STRING, "12345678901");
    add("emailAddress", V_ASN1_IA5STRING, "ivan@example.test");
    add("1.2.3.4.5", V_ASN1_UTF8STRING, "private");
    X509_set_subject_name(c, name);
    X509_set_issuer_name(c, name);
    X509_NAME_free(name);
    addExt(c, nullptr, NID_basic_constraints, "critical,CA:TRUE,pathlen:2");
    addExt(c, nullptr, NID_key_usage, "critical,digitalSignature,keyAgreement,decipherOnly");
    addExt(c, nullptr, NID_ext_key_usage, "emailProtection,clientAuth,1.2.3.4.5.6");
    addExt(c, nullptr, NID_subject_key_identifier, "hash");
    X509_sign(c, key.get(), EVP_sha384());
    return certDer(TestKey{std::move(key), std::move(cert)});
}

} // namespace

RIVET_TEST(x509EquivalentToOpenSslRsaAndEc) {
    expectEquivalent(certDer(makeCert({.cn = "Anna Ivanova", .inn = "500100732259", .snils = "12345678901",
                                       .email = "anna@example.test"})));
    expectEquivalent(certDer(makeCert({.cn = "RSA 3072", .keyKind = "rsa3072", .sigDigest = "SHA512"})));
    expectEquivalent(certDer(makeCert({.cn = "EC 256", .keyKind = "ec256"})));
    expectEquivalent(certDer(makeCert({.cn = "EC 384", .keyKind = "ec384", .sigDigest = "SHA384"})));
    expectEquivalent(certDer(makeCert({.cn = "Old", .keyKind = "rsa1024", .sigDigest = "SHA1"})));
    expectEquivalent(certDer(makeCert({.cn = "Root", .ca = true})));
}

RIVET_TEST(x509EquivalentToOpenSslIssuedChain) {
    const auto root = makeCert({.cn = "Root", .ca = true});
    CertSpec ispec;
    ispec.cn = "Intermediate";
    ispec.ca = true;
    ispec.issuer = &root;
    const auto inter = makeCert(ispec);
    CertSpec lspec;
    lspec.cn = "Leaf";
    lspec.keyKind = "ec256";
    lspec.issuer = &inter;
    const auto leaf = makeCert(lspec);
    expectEquivalent(certDer(inter));
    expectEquivalent(certDer(leaf));
}

RIVET_TEST(x509EquivalentToOpenSslUnusualSubject) {
    expectEquivalent(oddSubjectCertificate());
}

#else

RIVET_TEST(x509OpenSslEquivalenceSkipped) {
    std::printf("    SKIP: built without OpenSSL (RIVET_TEST_CRYPTO_OPENSSL not defined)\n");
}

#endif
