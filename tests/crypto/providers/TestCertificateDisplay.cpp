// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "TestCertBuilder.hpp"
#include "crypto/CertificateDisplay.hpp"

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

namespace {

const auto kMid = std::chrono::system_clock::time_point(std::chrono::sys_days{std::chrono::year{2026} / 6 / 1});

CertBuild fullGost() {
    CertBuild b;
    b.subject = {
        {"2.5.4.3", "Иванов Иван Иванович"},
        {"2.5.4.4", "Иванов"},
        {"2.5.4.42", "Иван Иванович"},
        {"2.5.4.10", "ООО Ромашка"},
        {"2.5.4.12", "Директор"},
        {"2.5.4.6", "RU", 0x13},
        {"2.5.4.7", "Москва"},
        {"2.5.4.8", "77 г. Москва"},
        {"1.2.840.113549.1.9.1", "ivanov@example.test", 0x16},
        {"1.2.643.3.131.1.1", "007707083893"},
        {"1.2.643.100.3", "12345678901"},
        {"1.2.643.100.1", "1027700132195"},
        {"1.2.643.100.5", "304500116000157"},
        {"1.2.643.100.4", "7707083893"},
    };
    b.issuer = {{"2.5.4.3", "Тестовый УЦ"}, {"2.5.4.10", "Тест Траст"}};
    b.spkiAlgOid = "1.2.643.7.1.1.1.2";
    b.spkiParams = der::sequence({der::oidElement("1.2.643.7.1.2.1.2.1"), der::oidElement("1.2.643.7.1.1.2.3")});
    b.keyBits = der::octetString(Bytes(128, 0x44));
    b.policies = {"1.2.643.100.113.1", "1.2.643.100.113.2"};
    return b;
}

KeyLocation location(KeyLocationKind kind, std::string label, std::string device = {}) {
    KeyLocation l;
    l.kind = kind;
    l.label = std::move(label);
    l.device = std::move(device);
    return l;
}

} // namespace

RIVET_TEST(displayGostCertificateAllAttributes) {
    const Bytes der = buildCertificate(fullGost());
    auto info = parseX509(der, [](DigestAlgorithm, ByteView) -> CryptoResult<Bytes> { return Bytes(32, 0x0A); });
    CHECK(info.has_value());
    if (!info) return;
    const CertificateDisplay d = makeCertificateDisplay(*info, nullptr, nullptr, false, kMid);
    CHECK_EQ(d.subjectName.value_or(""), std::string("Иванов Иван Иванович"));
    CHECK_EQ(d.personName.value_or(""), std::string("Иванов Иван Иванович"));
    CHECK_EQ(d.organization.value_or(""), std::string("ООО Ромашка"));
    CHECK_EQ(d.title.value_or(""), std::string("Директор"));
    CHECK_EQ(d.country.value_or(""), std::string("RU"));
    CHECK_EQ(d.locality.value_or(""), std::string("Москва"));
    CHECK_EQ(d.region.value_or(""), std::string("77 г. Москва"));
    CHECK_EQ(d.email.value_or(""), std::string("ivanov@example.test"));
    CHECK_EQ(d.inn.value_or(""), std::string("007707083893"));
    CHECK_EQ(d.snils.value_or(""), std::string("12345678901"));
    CHECK_EQ(d.ogrn.value_or(""), std::string("1027700132195"));
    CHECK_EQ(d.ogrnip.value_or(""), std::string("304500116000157"));
    CHECK_EQ(d.innLe.value_or(""), std::string("7707083893"));
    CHECK_EQ(d.issuerName.value_or(""), std::string("Тестовый УЦ"));
    CHECK_EQ(d.issuerOrganization.value_or(""), std::string("Тест Траст"));
    CHECK_EQ(d.serialHex.value_or(""), std::string("010203"));
    std::string expectedFingerprint;
    for (int i = 0; i < 32; ++i) expectedFingerprint += "0a";
    CHECK_EQ(d.sha256Fingerprint, expectedFingerprint);
    CHECK_EQ(d.keyAlgorithmLabel, std::string("GOST R 34.10-2012 512"));
    CHECK_EQ(d.certificatePolicyOids.size(), std::size_t{2});
    CHECK(!d.expired);
    CHECK(!d.notYetValid);
    CHECK(d.validFrom == info->notBefore);
    CHECK(d.validTo == info->notAfter);
    CHECK_EQ(d.location.kind, KeyLocationKind::Unknown);
    CHECK(!d.privateKeyAvailable);

    // Same result with caller-supplied extras.
    X509Extras ex;
    auto again = parseX509(der, {}, &ex);
    CHECK(again.has_value());
    if (again) CHECK_EQ(makeCertificateDisplay(*again, &ex, nullptr, true, kMid).personName.value_or(""), d.personName.value_or("x"));
}

RIVET_TEST(displayOnlyCommonName) {
    CertBuild b;
    b.subject = {{"2.5.4.3", "Only CN"}};
    b.issuer = {{"2.5.4.10", "Issuer Org Only"}};
    auto info = parseX509(buildCertificate(b));
    CHECK(info.has_value());
    if (!info) return;
    const CertificateDisplay d = makeCertificateDisplay(*info, nullptr, nullptr, false, kMid);
    CHECK_EQ(d.subjectName.value_or(""), std::string("Only CN"));
    CHECK(!d.personName && !d.organization && !d.title && !d.country && !d.locality && !d.region && !d.email);
    CHECK(!d.inn && !d.snils && !d.ogrn && !d.ogrnip && !d.innLe);
    CHECK_EQ(d.issuerName.value_or(""), std::string("Issuer Org Only")); // falls back to the issuer O
    CHECK_EQ(d.issuerOrganization.value_or(""), std::string("Issuer Org Only"));
    CHECK(d.certificatePolicyOids.empty());
    CHECK_EQ(d.keyAlgorithmLabel, std::string("RSA"));
}

RIVET_TEST(displayWithoutRussianAttributes) {
    CertBuild b;
    b.subject = {{"2.5.4.3", "Jane Doe"}, {"2.5.4.4", "Doe"}, {"2.5.4.10", "Example Inc"}, {"2.5.4.6", "US", 0x13}};
    b.issuer = {{"2.5.4.3", "Example CA"}};
    b.spkiAlgOid = "1.2.840.10045.2.1";
    b.spkiParams = der::oidElement("1.2.840.10045.3.1.7");
    b.keyBits = Bytes(65, 0x04);
    auto info = parseX509(buildCertificate(b));
    CHECK(info.has_value());
    if (!info) return;
    const CertificateDisplay d = makeCertificateDisplay(*info, nullptr, nullptr, false, kMid);
    CHECK(!d.inn && !d.snils && !d.ogrn && !d.ogrnip && !d.innLe);
    CHECK_EQ(d.personName.value_or(""), std::string("Doe")); // surname only: given name is not invented
    CHECK_EQ(d.country.value_or(""), std::string("US"));
    CHECK_EQ(d.keyAlgorithmLabel, std::string("EC P-256"));
    CHECK_EQ(d.issuerName.value_or(""), std::string("Example CA"));
}

RIVET_TEST(displayLocationPassthrough) {
    CertBuild b;
    b.subject = {{"2.5.4.3", "Holder"}};
    b.issuer = {{"2.5.4.3", "CA"}};
    auto info = parseX509(buildCertificate(b));
    CHECK(info.has_value());
    if (!info) return;
    struct Row {
        KeyLocation loc;
        bool available;
    };
    const std::vector<Row> rows = {
        {location(KeyLocationKind::Token, "Rutoken ECP", "Reader 0"), true},
        {location(KeyLocationKind::Cloud, "cloud-signer"), false},
        {location(KeyLocationKind::Container, "container-1"), true},
        {location(KeyLocationKind::Platform, "Login keychain"), true},
        {location(KeyLocationKind::Software, "memory"), false},
    };
    for (const Row& r : rows) {
        const CertificateDisplay d = makeCertificateDisplay(*info, nullptr, &r.loc, r.available, kMid);
        CHECK_EQ(d.location.kind, r.loc.kind);
        CHECK_EQ(d.location.label, r.loc.label);
        CHECK_EQ(d.location.device, r.loc.device);
        CHECK_EQ(d.privateKeyAvailable, r.available);

        IdentityDescriptor id;
        id.id = "x";
        id.certificate = *info;
        id.location = r.loc;
        id.privateKeyAvailable = r.available;
        const CertificateDisplay d2 = makeCertificateDisplay(id, nullptr, kMid);
        CHECK_EQ(d2.location.kind, r.loc.kind);
        CHECK_EQ(d2.location.device, r.loc.device);
        CHECK_EQ(d2.privateKeyAvailable, r.available);
        CHECK_EQ(d2.subjectName.value_or(""), std::string("Holder"));
    }
}

RIVET_TEST(displayExpiredAndNotYetValid) {
    CertBuild b;
    b.subject = {{"2.5.4.3", "Holder"}};
    b.issuer = {{"2.5.4.3", "CA"}};
    b.notBefore = std::chrono::sys_days{std::chrono::year{2025} / 1 / 1};
    b.notAfter = std::chrono::sys_days{std::chrono::year{2026} / 1 / 1};
    auto info = parseX509(buildCertificate(b));
    CHECK(info.has_value());
    if (!info) return;
    const auto at = [](int y, unsigned m) {
        return std::chrono::system_clock::time_point(std::chrono::sys_days{std::chrono::year{y} / m / 15});
    };
    const auto before = makeCertificateDisplay(*info, nullptr, nullptr, false, at(2024, 6));
    CHECK(before.notYetValid && !before.expired);
    const auto during = makeCertificateDisplay(*info, nullptr, nullptr, false, at(2025, 6));
    CHECK(!during.notYetValid && !during.expired);
    const auto after = makeCertificateDisplay(*info, nullptr, nullptr, false, at(2026, 6));
    CHECK(!after.notYetValid && after.expired);
}

RIVET_TEST(displayUnparseableCertificateStillFillsInfoFields) {
    CertificateInfo c;
    c.commonName = "Synthetic";
    c.der = Bytes{0x01, 0x02}; // not a certificate: extras-based fields stay empty
    const CertificateDisplay d = makeCertificateDisplay(c, nullptr, nullptr, false, kMid);
    CHECK_EQ(d.subjectName.value_or(""), std::string("Synthetic"));
    CHECK(!d.personName && !d.issuerName && !d.title);
    CHECK(!d.serialHex.has_value());
    CHECK(d.keyAlgorithmLabel.empty());
}
