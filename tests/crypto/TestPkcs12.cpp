// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "TestSupport.hpp"

using namespace rivet::crypto;
using namespace rivet::crypto::testing;

RIVET_TEST(pkcs12RoundTripOfSelfSignedIdentity) {
    auto provider = makeDefaultProvider();
    CHECK(provider->isAvailable());
    SelfSignedRequest req;
    req.commonName = "Ivan Petrov";
    req.email = "ivan@example.org";
    req.organization = "Example LLC";
    req.key = KeyAlgorithm::EcdsaP256;
    auto id = provider->createSelfSignedIdentity(req);
    CHECK(id.has_value());
    if (!id) return;
    const auto& cert = (*id)->certificate();
    CHECK_EQ(cert.commonName.value_or(""), std::string("Ivan Petrov"));
    CHECK_EQ(cert.email.value_or(""), std::string("ivan@example.org"));
    CHECK_EQ(cert.organization.value_or(""), std::string("Example LLC"));
    CHECK(!cert.isCa);
    CHECK(cert.hasKeyUsage);
    CHECK((cert.keyUsage & keyusage::kDigitalSignature) != 0);
    CHECK((cert.keyUsage & keyusage::kNonRepudiation) != 0);
    CHECK(cert.isSelfIssued());
    CHECK(!cert.russian.any()); // Russian attributes only when present
    CHECK(cert.validAt(std::chrono::system_clock::now()));
    CHECK((*id)->isExportable());

    auto p12 = provider->exportPkcs12(**id, SecretBuffer(std::string_view("s3cret")));
    CHECK(p12.has_value());
    if (!p12) return;
    auto back = provider->importPkcs12(*p12, SecretBuffer(std::string_view("s3cret")));
    CHECK(back.has_value());
    if (!back) return;
    CHECK_EQ(back->identities.size(), std::size_t{1});
    CHECK_EQ(back->identities[0]->certificate().sha256Fingerprint, cert.sha256Fingerprint);
}

RIVET_TEST(pkcs12WrongPasswordIsTyped) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    const Bytes p12 = makeP12(key, {}, "right");
    const auto r = provider->importPkcs12(p12, SecretBuffer(std::string_view("wrong")));
    CHECK(!r.has_value());
    if (!r) CHECK_EQ(r.error().code, CryptoErrorCode::WrongPassword);
    const auto empty = provider->importPkcs12(p12, SecretBuffer());
    CHECK(!empty.has_value());
}

RIVET_TEST(pkcs12CorruptDataIsTyped) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    Bytes p12 = makeP12(key, {}, "pw");
    const auto garbage = provider->importPkcs12(Bytes{1, 2, 3, 4, 5}, SecretBuffer(std::string_view("pw")));
    CHECK(!garbage.has_value());
    if (!garbage) CHECK_EQ(garbage.error().code, CryptoErrorCode::CorruptData);
    Bytes truncated(p12.begin(), p12.begin() + static_cast<std::ptrdiff_t>(p12.size() / 2));
    const auto t = provider->importPkcs12(truncated, SecretBuffer(std::string_view("pw")));
    CHECK(!t.has_value());
    if (!t) CHECK_EQ(t.error().code, CryptoErrorCode::CorruptData);
    const auto none = provider->importPkcs12(Bytes{}, SecretBuffer(std::string_view("pw")));
    CHECK(!none.has_value());
}

RIVET_TEST(pkcs12WithoutPrivateKeyIsTyped) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    const Bytes p12 = makeP12(key, {}, "pw", /*withKey=*/false);
    const auto r = provider->importPkcs12(p12, SecretBuffer(std::string_view("pw")));
    CHECK(!r.has_value());
    if (!r) CHECK_EQ(r.error().code, CryptoErrorCode::NoPrivateKey);
}

RIVET_TEST(pkcs12ImportsChainAndRussianAttributes) {
    auto provider = makeDefaultProvider();
    const auto ca = makeCert({.cn = "Test CA", .ca = true});
    CertSpec spec;
    spec.cn = "Petrov I I";
    spec.inn = "123456789012";
    spec.snils = "11223344595";
    spec.issuer = &ca;
    const auto leaf = makeCert(spec);
    auto id = makeIdentity(*provider, leaf, {&ca});
    CHECK(id != nullptr);
    if (!id) return;
    CHECK_EQ(id->chainDer().size(), std::size_t{2});
    const auto& c = id->certificate();
    CHECK_EQ(c.russian.inn.value_or(""), std::string("123456789012"));
    CHECK_EQ(c.russian.snils.value_or(""), std::string("11223344595"));
    CHECK(!c.russian.ogrn.has_value());
    CHECK(c.russian.any());
}

RIVET_TEST(pkcs12NonExportableIdentityRefusesExport) {
    auto provider = makeDefaultProvider();
    const auto key = makeCert({});
    auto id = makeIdentity(*provider, key);
    CHECK(id != nullptr);
    if (!id) return;
    CHECK(id->isExportable());
    // A null provider cannot export anything.
    auto nullp = makeNullProvider();
    const auto r = nullp->exportPkcs12(*id, SecretBuffer(std::string_view("x")));
    CHECK(!r.has_value());
}
