// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "crypto/providers/platform/PlatformIdentityStore.hpp"

#include <cstdio>
#include <string>

#if defined(RIVET_CRYPTO_PLATFORM_MACOS) && defined(RIVET_TEST_CRYPTO_OPENSSL)
#define RIVET_PLATFORM_KEYCHAIN_TESTS 1
#include "TestSupport.hpp"
#include "crypto/providers/platform/MacKeychainStoreTesting.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <openssl/pkcs12.h>

#include <algorithm>
#include <filesystem>
#include <unistd.h>
#endif

using namespace rivet::crypto;

RIVET_TEST(platformFactoryReportsAvailability) {
    auto store = makePlatformIdentityStore();
    CHECK(store != nullptr);
    if (!store) return;
    CHECK_EQ(store->assurance(), ProviderAssurance::PlatformKeystore);
    CHECK_EQ(store->isAvailable(), platformStoreSupported());
    if (!platformStoreSupported()) {
        CHECK_EQ(store->unavailableReason(), std::string("no platform keystore on this OS: use PKCS#11 or files"));
        CancelToken cancel;
        auto listed = store->list(cancel);
        CHECK(!listed.has_value());
        if (!listed) CHECK_EQ(listed.error().code, CryptoErrorCode::BackendUnavailable);
        auto opened = store->open("anything", nullptr, cancel);
        CHECK(!opened.has_value());
        if (!opened) CHECK_EQ(opened.error().code, CryptoErrorCode::BackendUnavailable);
    }
}

#if defined(RIVET_CRYPTO_PLATFORM_MACOS)
RIVET_TEST(platformDefaultStoreEnumerationIsTyped) {
    // Only asserts "does not crash and returns a typed result". Nothing about
    // the user's keychain contents is read back or printed.
    auto store = makePlatformIdentityStore();
    CHECK(store->isAvailable());
    CancelToken cancel;
    auto listed = store->list(cancel);
    if (!listed) CHECK(!toString(listed.error().code).empty());
    CancelToken cancelled;
    cancelled.cancel();
    auto none = store->list(cancelled);
    CHECK(!none.has_value());
    if (!none) CHECK_EQ(none.error().code, CryptoErrorCode::Cancelled);
    auto missing = store->open("0000000000000000000000000000000000000000000000000000000000000000", nullptr, cancel);
    CHECK(!missing.has_value());
}
#endif

#ifdef RIVET_PLATFORM_KEYCHAIN_TESTS

namespace {

namespace fs = std::filesystem;
using namespace rivet::crypto::testing;

// PKCS#12 with 3DES/SHA-1 bags, which Security.framework imports reliably
// (OpenSSL 3 defaults to AES-256/PBES2 and a SHA-256 MAC).
Bytes legacyP12(const TestKey& leaf, const TestKey* ca, const std::string& pw) {
    STACK_OF(X509)* stack = sk_X509_new_null();
    if (ca) {
        X509_up_ref(ca->cert.get());
        sk_X509_push(stack, ca->cert.get());
    }
    P12Ptr p12(PKCS12_create(pw.c_str(), "rivet-test", leaf.pkey.get(), leaf.cert.get(), stack,
                             NID_pbe_WithSHA1And3_Key_TripleDES_CBC, NID_pbe_WithSHA1And3_Key_TripleDES_CBC, 2048, 1, 0));
    sk_X509_pop_free(stack, X509_free);
    if (!p12) return {};
    unsigned char* buf = nullptr;
    const int n = i2d_PKCS12(p12.get(), &buf);
    Bytes out(buf, buf + n);
    OPENSSL_free(buf);
    return out;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
// The legacy file-keychain API is the only way to build a throwaway keychain
// at a chosen path without touching the user's keychains; Apple deprecated it
// but it still works. Deprecation warnings are suppressed ONLY in this test.
struct TempKeychain {
    fs::path dir;
    fs::path file;
    SecKeychainRef keychain = nullptr;
    std::string failure;

    TempKeychain() {
        SecKeychainSetUserInteractionAllowed(false); // never hang on a UI prompt
        dir = fs::temp_directory_path() / ("rivet_keychain_test_" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        file = dir / "test.keychain-db";
        const char* pw = "rivet-test-pw";
        const OSStatus st = SecKeychainCreate(file.c_str(), static_cast<UInt32>(std::char_traits<char>::length(pw)), pw, false,
                                              nullptr, &keychain);
        if (st != errSecSuccess || !keychain) {
            failure = "SecKeychainCreate failed (OSStatus " + std::to_string(st) + ")";
            keychain = nullptr;
        }
    }
    ~TempKeychain() {
        if (keychain) {
            SecKeychainDelete(keychain);
            CFRelease(keychain);
        }
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    TempKeychain(const TempKeychain&) = delete;
    TempKeychain& operator=(const TempKeychain&) = delete;

    bool importP12(const Bytes& p12, const std::string& password) {
        CFDataRef data = CFDataCreate(nullptr, p12.data(), static_cast<CFIndex>(p12.size()));
        CFStringRef pw = CFStringCreateWithCString(nullptr, password.c_str(), kCFStringEncodingUTF8);
        const void* keys[] = {kSecImportExportPassphrase, kSecImportExportKeychain};
        const void* values[] = {pw, keychain};
        CFDictionaryRef options = CFDictionaryCreate(nullptr, keys, values, 2, &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
        CFArrayRef items = nullptr;
        const OSStatus st = SecPKCS12Import(data, options, &items);
        if (items) CFRelease(items);
        CFRelease(options);
        CFRelease(pw);
        CFRelease(data);
        if (st != errSecSuccess) failure = "SecPKCS12Import failed (OSStatus " + std::to_string(st) + ")";
        return st == errSecSuccess;
    }
};
#pragma clang diagnostic pop

SignatureScheme schemeFor(SignatureAlgorithm a) { return schemeOf(a); }

} // namespace

RIVET_TEST(platformTempKeychainSignAndVerify) {
    TempKeychain tk;
    if (!tk.keychain) {
        std::printf("    SKIP: cannot create a temporary keychain here: %s\n", tk.failure.c_str());
        return;
    }
    auto provider = makeDefaultProvider();
    const auto ca = makeCert({.cn = "Keychain Test CA", .ca = true});
    CertSpec rspec;
    rspec.cn = "Keychain RSA Signer";
    rspec.inn = "007707083893";
    rspec.issuer = &ca;
    const auto rsa = makeCert(rspec);
    const auto ec = makeCert({.cn = "Keychain EC Signer", .keyKind = "ec256"});
    const auto rsaCertDer = certDer(rsa);
    const auto ecCertDer = certDer(ec);

    if (!tk.importP12(legacyP12(rsa, &ca, "pw"), "pw") || !tk.importP12(legacyP12(ec, nullptr, "pw"), "pw")) {
        std::printf("    SKIP: cannot import test identities into the temporary keychain: %s\n", tk.failure.c_str());
        return;
    }

    auto store = makeMacKeychainStoreForTesting(tk.file);
    CHECK(store->isAvailable());
    CancelToken cancel;
    auto listed = store->list(cancel);
    CHECK(listed.has_value());
    if (!listed) {
        std::printf("    list failed: %s\n", listed.error().message.c_str());
        return;
    }
    CHECK_EQ(listed->size(), std::size_t{2});

    auto parsedRsa = provider->parseCertificate(rsaCertDer);
    auto parsedEc = provider->parseCertificate(ecCertDer);
    CHECK(parsedRsa.has_value() && parsedEc.has_value());
    if (!parsedRsa || !parsedEc) return;

    for (const IdentityDescriptor& d : *listed) {
        CHECK_EQ(d.location.kind, KeyLocationKind::Platform);
        CHECK(!d.location.label.empty());
        CHECK(d.privateKeyAvailable);
        CHECK(!d.certificateExpired);
        CHECK_EQ(d.providerName, std::string("macos-keychain"));
        const bool isRsa = d.certificate.publicKeyType == PublicKeyType::Rsa;
        const CertificateInfo& expected = isRsa ? *parsedRsa : *parsedEc;
        CHECK_EQ(d.id, expected.sha256Fingerprint);
        CHECK_EQ(d.certificate.der, expected.der);
        CHECK_EQ(d.certificate.subject, expected.subject);

        auto opened = store->open(d.id, nullptr, cancel);
        CHECK(opened.has_value());
        if (!opened) {
            std::printf("    open failed: %s\n", opened.error().message.c_str());
            continue;
        }
        SigningIdentity& id = **opened;
        CHECK_EQ(id.providerName(), std::string_view("macos-keychain"));
        CHECK(!id.isExportable());
        CHECK_EQ(id.keyType(), expected.publicKeyType);
        CHECK_EQ(id.keyBits(), expected.keyBits);
        CHECK(!id.chainDer().empty());
        CHECK_EQ(id.chainDer()[0], expected.der);
        if (isRsa) std::printf("    note: chain length for the RSA identity = %zu\n", id.chainDer().size());

        const auto supported = id.supportedSignatures();
        CHECK(!supported.empty());
        const Bytes message = {'r', 'i', 'v', 'e', 't', ' ', 's', 'i', 'g', 'n'};
        for (const SignatureAlgorithm alg : supported) {
            CHECK_EQ(keyTypeOf(alg), expected.publicKeyType);
            auto sig = id.sign(alg, message, cancel);
            CHECK(sig.has_value());
            if (!sig) {
                std::printf("    sign(%s) failed: %s\n", std::string(toString(alg)).c_str(), sig.error().message.c_str());
                continue;
            }
            CHECK(provider->verifySignature(expected, schemeFor(alg), message, *sig).has_value());
            // tampered message must not verify
            Bytes other = message;
            other[0] ^= 1;
            CHECK(!provider->verifySignature(expected, schemeFor(alg), other, *sig).has_value());

            auto digest = provider->digest(digestOf(alg), message);
            CHECK(digest.has_value());
            if (!digest) continue;
            auto dsig = id.signDigest(alg, *digest, cancel);
            CHECK(dsig.has_value());
            if (dsig) CHECK(provider->verifySignature(expected, schemeFor(alg), message, *dsig).has_value());
            CHECK(!id.signDigest(alg, Bytes(7, 1), cancel).has_value()); // wrong digest length
        }

        // GOST is not provided by Security.framework; a key of the other type is refused.
        auto gost = id.sign(SignatureAlgorithm::Gost3410_2012_256, message, cancel);
        CHECK(!gost.has_value());
        if (!gost) {
            // Refused either by the key-policy gate or because Security.framework lacks GOST.
            const auto c = gost.error().code;
            if (c != CryptoErrorCode::UnsupportedAlgorithm && c != CryptoErrorCode::KeyMismatch)
                std::printf("    unexpected GOST refusal code: %s\n", std::string(toString(c)).c_str());
            CHECK(c == CryptoErrorCode::UnsupportedAlgorithm || c == CryptoErrorCode::KeyMismatch);
        }
        const SignatureAlgorithm other = isRsa ? SignatureAlgorithm::EcdsaSha256 : SignatureAlgorithm::RsaPkcs1Sha256;
        auto mismatch = id.sign(other, message, cancel);
        CHECK(!mismatch.has_value());
        if (!mismatch) CHECK_EQ(mismatch.error().code, CryptoErrorCode::KeyMismatch);

        CancelToken cancelled;
        cancelled.cancel();
        auto stopped = id.sign(supported.front(), message, cancelled);
        CHECK(!stopped.has_value());
        if (!stopped) CHECK_EQ(stopped.error().code, CryptoErrorCode::Cancelled);
    }

    auto missing = store->open("ff" + std::string(62, '0'), nullptr, cancel);
    CHECK(!missing.has_value());
    if (!missing) CHECK_EQ(missing.error().code, CryptoErrorCode::CertificateNotFound);
}

#else

RIVET_TEST(platformKeychainTestSkipped) {
    std::printf("    SKIP: macOS keychain tests need macOS and RIVET_ENABLE_CRYPTO (OpenSSL) in this build\n");
}

#endif
