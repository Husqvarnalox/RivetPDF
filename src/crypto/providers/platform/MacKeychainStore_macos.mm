// SPDX-License-Identifier: MPL-2.0
//
// macOS keychain identity store (Security.framework).
//
// Design:
//  * Identities are enumerated with SecItemCopyMatching(kSecClassIdentity,
//    kSecReturnRef, kSecMatchLimitAll). Only references are fetched: nothing
//    is exported and no key material is read. Private keys are NEVER
//    exported (SecKeyCopyExternalRepresentation is never called on them); the
//    only operation done with a key is SecKeyCreateSignature.
//  * Certificates are parsed with the backend-free parseX509 (GOST-aware);
//    fingerprints come from CommonCrypto (private to this file). The
//    identity id is the SHA-256 fingerprint of the certificate (lower-case hex).
//  * PinPrompt is NOT used by this provider. Access to a keychain key (user
//    consent dialog, keychain password, smart-card PIN of a CryptoTokenKit
//    token) is requested by the operating system's own UI while
//    SecKeyCreateSignature runs; the `prompt` argument of open() is ignored.
//    Without a possibility to show UI (errSecInteractionNotAllowed) the
//    operation fails with Cancelled and an explanatory message.
//  * Chain: SecTrust is used only to collect intermediates present on the
//    machine (network fetching disabled, bounded to 10 certificates, leaf
//    first). Trust decisions are Rivet's (TrustAnchorSet), not the OS's.
//  * The test seam in MacKeychainStoreTesting.hpp restricts the search to one
//    keychain file; it needs the legacy file-keychain API, which Apple
//    deprecated. Those calls are isolated below under a local
//    -Wdeprecated-declarations suppression; production enumeration of the
//    user's keychains uses only current API.

#include "crypto/providers/platform/MacKeychainStoreTesting.hpp"
#include "crypto/providers/platform/PlatformStoreImpl.hpp"

#include "crypto/Algorithms.hpp"
#include "crypto/X509Parse.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rivet::crypto {

namespace {

// ---- RAII for Core Foundation objects -----------------------------------------

template <typename T>
class Cf {
public:
    Cf() = default;
    explicit Cf(T ref) : ref_(ref) {} // takes ownership of a +1 reference
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;
    Cf(Cf&& other) noexcept : ref_(std::exchange(other.ref_, nullptr)) {}
    Cf& operator=(Cf&& other) noexcept {
        if (this != &other) {
            reset();
            ref_ = std::exchange(other.ref_, nullptr);
        }
        return *this;
    }
    ~Cf() { reset(); }

    static Cf retained(T ref) {
        if (ref) CFRetain(ref);
        return Cf(ref);
    }
    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }
    void reset() {
        if (ref_) CFRelease(ref_);
        ref_ = nullptr;
    }

private:
    T ref_ = nullptr;
};

std::string toUtf8(CFStringRef s) {
    if (!s) return {};
    const CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

Bytes bytesOf(CFDataRef d) {
    if (!d) return {};
    const UInt8* p = CFDataGetBytePtr(d);
    return Bytes(p, p + CFDataGetLength(d));
}

// ---- digests for fingerprints (CommonCrypto) -------------------------------------

CryptoResult<Bytes> fingerprintDigest(DigestAlgorithm algorithm, ByteView data) {
    if (data.size() > 0xFFFFFFFFu) return fail(CryptoErrorCode::LimitExceeded, "input too large");
    const auto len = static_cast<CC_LONG>(data.size());
    if (algorithm == DigestAlgorithm::Sha256) {
        Bytes out(CC_SHA256_DIGEST_LENGTH);
        CC_SHA256(data.data(), len, out.data());
        return out;
    }
    if (algorithm == DigestAlgorithm::Sha1) {
        // Legacy display fingerprint only (never used for signatures). Apple
        // marks CC_SHA1 deprecated; the suppression is limited to this call.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        Bytes out(CC_SHA1_DIGEST_LENGTH);
        CC_SHA1(data.data(), len, out.data());
        return out;
#pragma clang diagnostic pop
    }
    return fail(CryptoErrorCode::UnsupportedAlgorithm, "digest not available for fingerprints");
}

// ---- error mapping ---------------------------------------------------------------------

CryptoError mapStatus(OSStatus status, const char* what, bool itemMeansNoKey) {
    switch (status) {
    case errSecUserCanceled:
        return CryptoError{CryptoErrorCode::Cancelled, "cancelled by the user"};
    case errSecAuthFailed:
        return CryptoError{CryptoErrorCode::WrongPassword, "keychain authentication failed (wrong PIN or password)"};
    case errSecItemNotFound:
        return itemMeansNoKey ? CryptoError{CryptoErrorCode::NoPrivateKey, "the keychain holds no private key for this certificate"}
                              : CryptoError{CryptoErrorCode::CertificateNotFound, "certificate not found in the keychain"};
    case errSecInteractionNotAllowed:
        return CryptoError{CryptoErrorCode::Cancelled,
                           "keychain interaction is not allowed (locked keychain or no user interface available)"};
    case errSecParam:
    case errSecUnimplemented:
        return CryptoError{CryptoErrorCode::Internal, std::string(what) + " failed (OSStatus " + std::to_string(status) + ")"};
    default:
        return CryptoError{CryptoErrorCode::Io, std::string(what) + " failed (OSStatus " + std::to_string(status) + ")"};
    }
}

CryptoError mapCfError(CFErrorRef error, const char* what) {
    if (!error) return CryptoError{CryptoErrorCode::Internal, std::string(what) + " failed"};
    CFStringRef domain = CFErrorGetDomain(error);
    const auto code = static_cast<OSStatus>(CFErrorGetCode(error));
    if (domain && CFEqual(domain, kCFErrorDomainOSStatus)) return mapStatus(code, what, true);
    return CryptoError{CryptoErrorCode::Io, std::string(what) + " failed (error code " + std::to_string(code) + ")"};
}

// ---- algorithm mapping -----------------------------------------------------------------

struct SecAlgPair {
    SecKeyAlgorithm message; // Security hashes the message
    SecKeyAlgorithm digest;  // input is a ready digest
};

std::optional<SecAlgPair> secAlgorithms(SignatureAlgorithm a) {
    switch (a) {
    case SignatureAlgorithm::RsaPkcs1Sha256:
        return SecAlgPair{kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA256, kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256};
    case SignatureAlgorithm::RsaPkcs1Sha384:
        return SecAlgPair{kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA384, kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA384};
    case SignatureAlgorithm::RsaPkcs1Sha512:
        return SecAlgPair{kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA512, kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512};
    case SignatureAlgorithm::RsaPssSha256:
        return SecAlgPair{kSecKeyAlgorithmRSASignatureMessagePSSSHA256, kSecKeyAlgorithmRSASignatureDigestPSSSHA256};
    case SignatureAlgorithm::RsaPssSha384:
        return SecAlgPair{kSecKeyAlgorithmRSASignatureMessagePSSSHA384, kSecKeyAlgorithmRSASignatureDigestPSSSHA384};
    case SignatureAlgorithm::RsaPssSha512:
        return SecAlgPair{kSecKeyAlgorithmRSASignatureMessagePSSSHA512, kSecKeyAlgorithmRSASignatureDigestPSSSHA512};
    case SignatureAlgorithm::EcdsaSha256:
        return SecAlgPair{kSecKeyAlgorithmECDSASignatureMessageX962SHA256, kSecKeyAlgorithmECDSASignatureDigestX962SHA256};
    case SignatureAlgorithm::EcdsaSha384:
        return SecAlgPair{kSecKeyAlgorithmECDSASignatureMessageX962SHA384, kSecKeyAlgorithmECDSASignatureDigestX962SHA384};
    case SignatureAlgorithm::EcdsaSha512:
        return SecAlgPair{kSecKeyAlgorithmECDSASignatureMessageX962SHA512, kSecKeyAlgorithmECDSASignatureDigestX962SHA512};
    case SignatureAlgorithm::Gost3410_2012_256:
    case SignatureAlgorithm::Gost3410_2012_512:
        return std::nullopt; // Security.framework has no GOST
    }
    return std::nullopt;
}

constexpr SignatureAlgorithm kAllAlgorithms[] = {
    SignatureAlgorithm::RsaPkcs1Sha256, SignatureAlgorithm::RsaPkcs1Sha384, SignatureAlgorithm::RsaPkcs1Sha512,
    SignatureAlgorithm::RsaPssSha256,   SignatureAlgorithm::RsaPssSha384,   SignatureAlgorithm::RsaPssSha512,
    SignatureAlgorithm::EcdsaSha256,    SignatureAlgorithm::EcdsaSha384,    SignatureAlgorithm::EcdsaSha512,
};

// ---- legacy file keychain (test seam only) -----------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
Cf<CFArrayRef> searchListFor(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {};
    SecKeychainRef keychain = nullptr;
    if (SecKeychainOpen(path.c_str(), &keychain) != errSecSuccess || !keychain) return {};
    Cf<SecKeychainRef> owned(keychain);
    const void* values[] = {keychain};
    return Cf<CFArrayRef>(CFArrayCreate(kCFAllocatorDefault, values, 1, &kCFTypeArrayCallBacks));
}

void restrictTrustToKeychains(SecTrustRef trust, CFArrayRef list) { SecTrustSetKeychains(trust, list); }
#pragma clang diagnostic pop

// ---- signing identity -----------------------------------------------------------------------

class MacSigningIdentity final : public SigningIdentity {
public:
    MacSigningIdentity(CertificateInfo cert, std::vector<Bytes> chain, Cf<SecKeyRef> key)
        : cert_(std::move(cert)), chain_(std::move(chain)), key_(std::move(key)) {
        for (const SignatureAlgorithm a : kAllAlgorithms) {
            if (keyTypeOf(a) != cert_.publicKeyType) continue;
            if (!checkCreationAllowed(a, cert_.publicKeyType, cert_.keyBits)) continue;
            const auto sa = secAlgorithms(a);
            if (sa && SecKeyIsAlgorithmSupported(key_.get(), kSecKeyOperationTypeSign, sa->message))
                supported_.push_back(a);
        }
    }

    std::string_view providerName() const override { return "macos-keychain"; }
    const CertificateInfo& certificate() const override { return cert_; }
    std::vector<Bytes> chainDer() const override { return chain_; }
    PublicKeyType keyType() const override { return cert_.publicKeyType; }
    int keyBits() const override { return cert_.keyBits; }
    std::vector<SignatureAlgorithm> supportedSignatures() const override { return supported_; }

    CryptoResult<Bytes> sign(SignatureAlgorithm algorithm, ByteView message, const CancelToken& cancel) override {
        return perform(algorithm, message, false, cancel);
    }
    CryptoResult<Bytes> signDigest(SignatureAlgorithm algorithm, ByteView digest, const CancelToken& cancel) override {
        return perform(algorithm, digest, true, cancel);
    }

private:
    CryptoResult<Bytes> perform(SignatureAlgorithm algorithm, ByteView input, bool inputIsDigest, const CancelToken& cancel) {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "cancelled");
        if (auto policy = checkCreationAllowed(algorithm, cert_.publicKeyType, cert_.keyBits); !policy)
            return std::unexpected(policy.error());
        const auto sa = secAlgorithms(algorithm);
        if (!sa) return fail(CryptoErrorCode::UnsupportedAlgorithm, "the macOS keychain does not provide this algorithm");
        if (keyTypeOf(algorithm) != cert_.publicKeyType)
            return fail(CryptoErrorCode::KeyMismatch, "key does not fit the signature algorithm");
        if (inputIsDigest && input.size() != digestSize(digestOf(algorithm)))
            return fail(CryptoErrorCode::InvalidArgument, "digest length does not match the algorithm");
        const SecKeyAlgorithm secAlg = inputIsDigest ? sa->digest : sa->message;
        if (!SecKeyIsAlgorithmSupported(key_.get(), kSecKeyOperationTypeSign, secAlg))
            return fail(CryptoErrorCode::UnsupportedAlgorithm, "the key does not support this algorithm");

        static const UInt8 kEmpty = 0;
        Cf<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, input.empty() ? &kEmpty : input.data(),
                                        static_cast<CFIndex>(input.size())));
        if (!data) return fail(CryptoErrorCode::Internal, "out of memory");
        CFErrorRef rawError = nullptr;
        Cf<CFDataRef> signature(SecKeyCreateSignature(key_.get(), secAlg, data.get(), &rawError));
        Cf<CFErrorRef> error(rawError);
        if (!signature) return std::unexpected(mapCfError(error.get(), "signing"));
        return bytesOf(signature.get());
    }

    CertificateInfo cert_;
    std::vector<Bytes> chain_;
    Cf<SecKeyRef> key_;
    std::vector<SignatureAlgorithm> supported_;
};

// ---- store ------------------------------------------------------------------------------------

class MacKeychainStore final : public IdentityStore {
public:
    explicit MacKeychainStore(std::optional<std::filesystem::path> keychain) : keychain_(std::move(keychain)) {}

    std::string_view name() const override { return "macos-keychain"; }
    bool isAvailable() const override { return true; }
    ProviderAssurance assurance() const override { return ProviderAssurance::PlatformKeystore; }

    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken& cancel) override {
        auto found = enumerate(cancel);
        if (!found) return std::unexpected(found.error());
        std::vector<IdentityDescriptor> out;
        out.reserve(found->size());
        for (Found& f : *found) out.push_back(std::move(f.descriptor));
        return out;
    }

    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view id, std::shared_ptr<PinPrompt> /*unused: OS UI*/,
                                                        const CancelToken& cancel) override {
        auto found = enumerate(cancel);
        if (!found) return std::unexpected(found.error());
        for (Found& f : *found) {
            if (f.descriptor.id != id) continue;
            SecKeyRef rawKey = nullptr;
            const OSStatus st = SecIdentityCopyPrivateKey(f.identity.get(), &rawKey);
            Cf<SecKeyRef> key(rawKey);
            if (st != errSecSuccess || !key) return std::unexpected(mapStatus(st, "private key lookup", true));
            std::vector<Bytes> chain = collectChain(f.certificate.get(), f.descriptor.certificate.der);
            return std::unique_ptr<SigningIdentity>(
                std::make_unique<MacSigningIdentity>(f.descriptor.certificate, std::move(chain), std::move(key)));
        }
        return fail(CryptoErrorCode::CertificateNotFound, "no such identity in the keychain");
    }

private:
    struct Found {
        IdentityDescriptor descriptor;
        Cf<SecIdentityRef> identity;
        Cf<SecCertificateRef> certificate;
    };

    CryptoResult<std::vector<Found>> enumerate(const CancelToken& cancel) {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "cancelled");
        Cf<CFMutableDictionaryRef> query(CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks));
        if (!query) return fail(CryptoErrorCode::Internal, "out of memory");
        CFDictionarySetValue(query.get(), kSecClass, kSecClassIdentity);
        CFDictionarySetValue(query.get(), kSecReturnRef, kCFBooleanTrue);
        CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitAll);
        Cf<CFArrayRef> searchList;
        if (keychain_) {
            searchList = searchListFor(*keychain_);
            if (!searchList) return fail(CryptoErrorCode::Io, "cannot open the keychain file");
            CFDictionarySetValue(query.get(), kSecMatchSearchList, searchList.get());
        }

        CFTypeRef rawResult = nullptr;
        const OSStatus status = SecItemCopyMatching(query.get(), &rawResult);
        Cf<CFTypeRef> result(rawResult);
        std::vector<Found> out;
        if (status == errSecItemNotFound) return out;
        if (status != errSecSuccess) return std::unexpected(mapStatus(status, "keychain enumeration", false));
        if (!result || CFGetTypeID(result.get()) != CFArrayGetTypeID()) return out;

        const auto array = static_cast<CFArrayRef>(result.get());
        const CFIndex count = std::min<CFIndex>(CFArrayGetCount(array), 10000);
        const auto now = std::chrono::system_clock::now();
        for (CFIndex i = 0; i < count; ++i) {
            if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "cancelled");
            const auto identity = static_cast<SecIdentityRef>(const_cast<void*>(CFArrayGetValueAtIndex(array, i)));
            if (!identity || CFGetTypeID(identity) != SecIdentityGetTypeID()) continue;

            SecCertificateRef rawCert = nullptr;
            if (SecIdentityCopyCertificate(identity, &rawCert) != errSecSuccess || !rawCert) continue;
            Cf<SecCertificateRef> cert(rawCert);
            Cf<CFDataRef> der(SecCertificateCopyData(cert.get()));
            Bytes derBytes = bytesOf(der.get());
            auto info = parseX509(derBytes, fingerprintDigest);
            if (!info) continue; // certificates Rivet cannot read are not offered
            if (std::ranges::any_of(out, [&](const Found& f) { return f.descriptor.id == info->sha256Fingerprint; })) continue;

            Found f;
            f.descriptor.id = info->sha256Fingerprint;
            f.descriptor.providerName = "macos-keychain";
            f.descriptor.location.kind = KeyLocationKind::Platform;
            {
                Cf<CFStringRef> summary(SecCertificateCopySubjectSummary(cert.get()));
                f.descriptor.location.label = toUtf8(summary.get());
            }
            f.descriptor.certificateExpired = now > info->notAfter;
            {
                SecKeyRef rawKey = nullptr;
                const OSStatus ks = SecIdentityCopyPrivateKey(identity, &rawKey);
                Cf<SecKeyRef> key(rawKey);
                f.descriptor.privateKeyAvailable = ks == errSecSuccess && key;
            }
            f.descriptor.certificate = std::move(*info);
            f.identity = Cf<SecIdentityRef>::retained(identity);
            f.certificate = std::move(cert);
            out.push_back(std::move(f));
        }
        return out;
    }

    // Leaf first; intermediates present on this machine, no network, at most 10.
    std::vector<Bytes> collectChain(SecCertificateRef leaf, const Bytes& leafDer) const {
        std::vector<Bytes> chain{leafDer};
        if (@available(macOS 12.0, *)) {
            Cf<SecPolicyRef> policy(SecPolicyCreateBasicX509());
            SecTrustRef rawTrust = nullptr;
            if (!policy || SecTrustCreateWithCertificates(leaf, policy.get(), &rawTrust) != errSecSuccess || !rawTrust)
                return chain;
            Cf<SecTrustRef> trust(rawTrust);
            SecTrustSetNetworkFetchAllowed(trust.get(), false);
            if (keychain_) {
                Cf<CFArrayRef> list = searchListFor(*keychain_);
                if (list) restrictTrustToKeychains(trust.get(), list.get());
            }
            CFErrorRef rawError = nullptr;
            (void)SecTrustEvaluateWithError(trust.get(), &rawError); // result irrelevant: only the built chain is used
            Cf<CFErrorRef> error(rawError);
            Cf<CFArrayRef> certs(SecTrustCopyCertificateChain(trust.get()));
            if (!certs) return chain;
            const CFIndex n = std::min<CFIndex>(CFArrayGetCount(certs.get()), 10);
            for (CFIndex i = 1; i < n; ++i) {
                const auto c = static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(certs.get(), i)));
                Cf<CFDataRef> data(SecCertificateCopyData(c));
                Bytes der = bytesOf(data.get());
                if (!der.empty()) chain.push_back(std::move(der));
            }
        }
        return chain;
    }

    std::optional<std::filesystem::path> keychain_;
};

} // namespace

namespace platform_detail {
std::shared_ptr<IdentityStore> makeMacKeychainStore() { return std::make_shared<MacKeychainStore>(std::nullopt); }
} // namespace platform_detail

std::shared_ptr<IdentityStore> makeMacKeychainStoreForTesting(const std::filesystem::path& keychainPath) {
    return std::make_shared<MacKeychainStore>(keychainPath);
}

} // namespace rivet::crypto
