// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "crypto/Algorithms.hpp"
#include "crypto/Providers.hpp"
#include "crypto/Validation.hpp"

using namespace rivet::crypto;

RIVET_TEST(algorithmMappings) {
    CHECK_EQ(digestOf(SignatureAlgorithm::RsaPssSha384), DigestAlgorithm::Sha384);
    CHECK_EQ(keyTypeOf(SignatureAlgorithm::EcdsaSha512), PublicKeyType::Ec);
    CHECK_EQ(digestSize(DigestAlgorithm::Sha512), std::size_t{64});
    CHECK(isLegacy(DigestAlgorithm::Sha1));
    CHECK(!isLegacy(DigestAlgorithm::Sha256));
    CHECK(isGost(SignatureAlgorithm::Gost3410_2012_256));
    CHECK_EQ(*digestFromOid(oid::kSha256), DigestAlgorithm::Sha256);
    CHECK(!digestFromOid("1.2.3.4").has_value());
}

RIVET_TEST(signatureSchemeRoundTrip) {
    using S = SignatureAlgorithm;
    for (S a : {S::RsaPkcs1Sha256, S::RsaPkcs1Sha384, S::RsaPkcs1Sha512, S::RsaPssSha256, S::RsaPssSha384,
                S::RsaPssSha512, S::EcdsaSha256, S::EcdsaSha384, S::EcdsaSha512, S::Gost3410_2012_256,
                S::Gost3410_2012_512}) {
        const auto back = signatureAlgorithmOf(schemeOf(a));
        CHECK(back.has_value());
        if (back) CHECK_EQ(*back, a);
    }
    SignatureScheme legacy = schemeOf(SignatureAlgorithm::RsaPkcs1Sha256);
    legacy.digest = DigestAlgorithm::Sha1;
    CHECK(!signatureAlgorithmOf(legacy).has_value()); // SHA-1 cannot be expressed as a creatable algorithm
}

RIVET_TEST(creationPolicyRefusesWeak) {
    CHECK(checkCreationAllowed(DigestAlgorithm::Sha256).has_value());
    const auto sha1 = checkCreationAllowed(DigestAlgorithm::Sha1);
    CHECK(!sha1);
    CHECK_EQ(sha1.error().code, CryptoErrorCode::WeakAlgorithm);

    const auto weakRsa = checkCreationAllowed(SignatureAlgorithm::RsaPkcs1Sha256, PublicKeyType::Rsa, 1024);
    CHECK(!weakRsa);
    CHECK_EQ(weakRsa.error().code, CryptoErrorCode::WeakAlgorithm);
    CHECK(checkCreationAllowed(SignatureAlgorithm::RsaPkcs1Sha256, PublicKeyType::Rsa, 2048).has_value());
    CHECK(checkCreationAllowed(SignatureAlgorithm::EcdsaSha256, PublicKeyType::Ec, 256).has_value());

    const auto mismatch = checkCreationAllowed(SignatureAlgorithm::EcdsaSha256, PublicKeyType::Rsa, 3072);
    CHECK_EQ(mismatch.error().code, CryptoErrorCode::KeyMismatch);
}

RIVET_TEST(verificationFlagsWeakLegacy) {
    CHECK(isWeakForVerification(DigestAlgorithm::Sha1, PublicKeyType::Rsa, 2048));
    CHECK(isWeakForVerification(DigestAlgorithm::Sha256, PublicKeyType::Rsa, 1024));
    CHECK(!isWeakForVerification(DigestAlgorithm::Sha256, PublicKeyType::Rsa, 2048));
    CHECK(!isWeakForVerification(DigestAlgorithm::Sha256, PublicKeyType::Ec, 256));
}

RIVET_TEST(validationStateFolding) {
    ValidationResult ok;
    ok.crypto = CryptoCheck::Valid;
    ok.integrity = IntegrityStatus::Intact;
    ok.certificate = CertificateStatus::Valid;
    ok.trust = TrustStatus::Trusted;
    CHECK_EQ(foldState(ok), SignatureState::ValidTrusted);

    ValidationResult untrusted = ok;
    untrusted.trust = TrustStatus::Untrusted;
    CHECK_EQ(foldState(untrusted), SignatureState::ValidUntrusted);

    ValidationResult expired = ok;
    expired.certificate = CertificateStatus::Expired;
    CHECK_EQ(foldState(expired), SignatureState::ValidExpiredNow);
    expired.timestamp = TimestampStatus::Valid;
    CHECK_EQ(foldState(expired), SignatureState::ValidAtTrustedTimestamp);

    ValidationResult weak = ok;
    weak.policy = PolicyStatus::WeakLegacyAlgorithm;
    CHECK_EQ(foldState(weak), SignatureState::WeakLegacyAlgorithm);

    ValidationResult modified = ok;
    modified.integrity = IntegrityStatus::Modified;
    CHECK_EQ(foldState(modified), SignatureState::DocumentModified);

    ValidationResult invalid = ok;
    invalid.crypto = CryptoCheck::Invalid;
    CHECK_EQ(foldState(invalid), SignatureState::InvalidSignature);

    ValidationResult unsupported = ok;
    unsupported.crypto = CryptoCheck::Unsupported;
    CHECK_EQ(foldState(unsupported), SignatureState::UnsupportedAlgorithm);

    ValidationResult revoked = ok;
    revoked.revocation = RevocationStatus::Revoked;
    CHECK_EQ(foldState(revoked), SignatureState::Revoked);

    ValidationResult permitted = ok;
    permitted.modification = ModificationStatus::Permitted;
    CHECK_EQ(foldState(permitted), SignatureState::PermittedModifications);

    CHECK_EQ(foldState(ValidationResult{}), SignatureState::UnableToValidate);
}

RIVET_TEST(nullProviderFailsEverythingExplicitly) {
    auto provider = makeNullProvider();
    CHECK(!provider->isAvailable());
    const auto hasher = provider->createHasher(DigestAlgorithm::Sha256);
    CHECK(!hasher);
    CHECK_EQ(hasher.error().code, CryptoErrorCode::BackendUnavailable);
    const auto id = provider->createSelfSignedIdentity(SelfSignedRequest{"x", {}, {}, KeyAlgorithm::Rsa2048, {}, {}});
    CHECK_EQ(id.error().code, CryptoErrorCode::BackendUnavailable);
    const auto p12 = provider->importPkcs12(Bytes{1, 2, 3}, SecretBuffer("pw"));
    CHECK_EQ(p12.error().code, CryptoErrorCode::BackendUnavailable);
}

RIVET_TEST(cryptoErrorBridgesToCoreError) {
    const auto e = toCoreError(CryptoError{CryptoErrorCode::WrongPassword, "nope"});
    CHECK_EQ(e.code, rivet::core::ErrorCode::PasswordRequired);
    CHECK_EQ(e.subsystem, "crypto");
}

RIVET_TEST(inMemoryCertificateStoreDeduplicates) {
    InMemoryCertificateStore store;
    CertificateInfo c;
    c.sha256Fingerprint = "aa";
    store.add(c);
    store.add(c);
    CHECK_EQ(store.all().size(), std::size_t{1});
    CHECK(store.findBySha256("aa").has_value());
    CHECK(!store.findBySha256("bb").has_value());
}
