// SPDX-License-Identifier: MPL-2.0
//
// ===========================================================================
// UNTESTED: written without access to a Windows machine; compile-checked only
// by review. It is NOT part of any CI job. Treat every line as unverified until
// it has been built and exercised on Windows (software key, smart card, token).
// ===========================================================================
//
// Windows certificate store + CNG identity store.
//
//  * Certificates: CertOpenStore(CERT_STORE_PROV_SYSTEM_W, "MY", current user),
//    CertEnumCertificatesInStore. Parsed with the backend-free parseX509. The
//    identity id is the SHA-256 fingerprint (lower-case hex); fingerprints use
//    BCrypt hashing.
//  * Keys: CryptAcquireCertificatePrivateKey with CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG
//    (no CRYPT_ACQUIRE_SILENT_FLAG: the OS may show its own UI, e.g. the
//    smart-card PIN dialog).
//  * Signing: NCryptSignHash with BCRYPT_PKCS1_PADDING_INFO (RSA PKCS#1),
//    BCRYPT_PSS_PADDING_INFO (salt length = digest length) or raw ECDSA (the
//    CNG r||s output is converted to DER). Keys are never exported; platform
//    identities always report isExportable() == false. (A production hardening
//    step is to also read NCRYPT_EXPORT_POLICY_PROPERTY and surface it in the UI.)
//  * PIN: when a PinPrompt is supplied, the PIN is requested from it and given to
//    the key with NCryptSetProperty(NCRYPT_PIN_PROPERTY) (null-terminated UTF-16)
//    before signing, so the application's own prompt is used. Without a prompt the
//    OS shows its dialog. The wide copy of the PIN is zeroed immediately after.
//  * Chain: CertGetCertificateChain with CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL (no
//    network), leaf first, bounded to 10 elements.

#if defined(RIVET_CRYPTO_PLATFORM_WINDOWS)

#include "crypto/providers/platform/PlatformStoreImpl.hpp"

#include "crypto/Der.hpp"
#include "crypto/X509Parse.hpp"

// clang-format off
#include <windows.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <ncrypt.h>
// clang-format on

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace rivet::crypto::platform_detail {

namespace {

constexpr std::size_t kMaxChain = 10;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// ---- digests (BCrypt) ---------------------------------------------------------------------

LPCWSTR bcryptName(DigestAlgorithm a) {
    switch (a) {
    case DigestAlgorithm::Sha1: return BCRYPT_SHA1_ALGORITHM;
    case DigestAlgorithm::Sha256: return BCRYPT_SHA256_ALGORITHM;
    case DigestAlgorithm::Sha384: return BCRYPT_SHA384_ALGORITHM;
    case DigestAlgorithm::Sha512: return BCRYPT_SHA512_ALGORITHM;
    default: return nullptr;
    }
}

CryptoResult<Bytes> digestFn(DigestAlgorithm algorithm, ByteView data) {
    LPCWSTR name = bcryptName(algorithm);
    if (!name) return fail(CryptoErrorCode::UnsupportedAlgorithm, "digest not available");
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, name, nullptr, 0) < 0)
        return fail(CryptoErrorCode::Internal, "BCryptOpenAlgorithmProvider failed");
    Bytes out(digestSize(algorithm));
    const NTSTATUS st = BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), out.data(),
                                   static_cast<ULONG>(out.size()));
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) return fail(CryptoErrorCode::Internal, "BCryptHash failed");
    return out;
}

// ---- error mapping --------------------------------------------------------------------------

CryptoError mapSecurityStatus(SECURITY_STATUS s, const char* what) {
    const auto code = static_cast<unsigned long>(s);
    if (code == static_cast<unsigned long>(NTE_USER_CANCELLED) || code == static_cast<unsigned long>(ERROR_CANCELLED))
        return {CryptoErrorCode::Cancelled, "cancelled by the user"};
    if (code == static_cast<unsigned long>(SCARD_W_WRONG_CHV) || code == static_cast<unsigned long>(SCARD_W_CHV_BLOCKED))
        return {CryptoErrorCode::WrongPassword, "wrong or blocked PIN"};
    if (code == static_cast<unsigned long>(NTE_BAD_KEYSET) || code == static_cast<unsigned long>(NTE_NO_KEY) ||
        code == static_cast<unsigned long>(SCARD_E_NO_SMARTCARD) || code == static_cast<unsigned long>(SCARD_E_NO_READERS_AVAILABLE))
        return {CryptoErrorCode::NoPrivateKey, "private key is not available (card or token missing?)"};
    if (code == static_cast<unsigned long>(NTE_NOT_SUPPORTED) || code == static_cast<unsigned long>(NTE_BAD_ALGID))
        return {CryptoErrorCode::UnsupportedAlgorithm, "the key does not support this algorithm"};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08lX", code);
    return {CryptoErrorCode::Io, std::string(what) + " failed (0x" + buf + ")"};
}

// ---- RAII ----------------------------------------------------------------------------------------

struct CertCtx {
    PCCERT_CONTEXT p = nullptr;
    CertCtx() = default;
    explicit CertCtx(PCCERT_CONTEXT c) : p(c) {}
    CertCtx(CertCtx&& o) noexcept : p(o.p) { o.p = nullptr; }
    CertCtx& operator=(CertCtx&& o) noexcept {
        if (this != &o) {
            reset();
            p = o.p;
            o.p = nullptr;
        }
        return *this;
    }
    CertCtx(const CertCtx&) = delete;
    CertCtx& operator=(const CertCtx&) = delete;
    ~CertCtx() { reset(); }
    void reset() {
        if (p) CertFreeCertificateContext(p);
        p = nullptr;
    }
};

struct NKey {
    NCRYPT_KEY_HANDLE h = 0;
    bool freeIt = false;
    ~NKey() {
        if (h && freeIt) NCryptFreeObject(h);
    }
};

ByteView certBytes(PCCERT_CONTEXT c) { return ByteView(c->pbCertEncoded, c->cbCertEncoded); }

Bytes derFromRs(const Bytes& rs) {
    const std::size_t half = rs.size() / 2;
    return der::sequence({der::integer(ByteView(rs.data(), half)), der::integer(ByteView(rs.data() + half, half))});
}

// ---- identity ------------------------------------------------------------------------------------

class CngSigningIdentity final : public SigningIdentity {
public:
    CngSigningIdentity(CertCtx cert, CertificateInfo info, std::vector<Bytes> chain, std::shared_ptr<PinPrompt> prompt,
                       std::string label)
        : cert_(std::move(cert)), info_(std::move(info)), chain_(std::move(chain)), prompt_(std::move(prompt)),
          label_(std::move(label)) {}

    std::string_view providerName() const override { return "windows-cng"; }
    const CertificateInfo& certificate() const override { return info_; }
    std::vector<Bytes> chainDer() const override { return chain_; }
    PublicKeyType keyType() const override { return info_.publicKeyType; }
    int keyBits() const override { return info_.keyBits; }

    std::vector<SignatureAlgorithm> supportedSignatures() const override {
        if (info_.publicKeyType == PublicKeyType::Rsa)
            return {SignatureAlgorithm::RsaPkcs1Sha256, SignatureAlgorithm::RsaPkcs1Sha384, SignatureAlgorithm::RsaPkcs1Sha512,
                    SignatureAlgorithm::RsaPssSha256,   SignatureAlgorithm::RsaPssSha384,   SignatureAlgorithm::RsaPssSha512};
        if (info_.publicKeyType == PublicKeyType::Ec)
            return {SignatureAlgorithm::EcdsaSha256, SignatureAlgorithm::EcdsaSha384, SignatureAlgorithm::EcdsaSha512};
        return {};
    }

    CryptoResult<Bytes> sign(SignatureAlgorithm algorithm, ByteView message, const CancelToken& cancel) override {
        auto d = digestFn(digestOf(algorithm), message);
        if (!d) return std::unexpected(d.error());
        return perform(algorithm, *d, cancel);
    }
    CryptoResult<Bytes> signDigest(SignatureAlgorithm algorithm, ByteView digest, const CancelToken& cancel) override {
        if (digest.size() != digestSize(digestOf(algorithm)))
            return fail(CryptoErrorCode::InvalidArgument, "digest length does not match the algorithm");
        return perform(algorithm, digest, cancel);
    }

private:
    CryptoResult<Bytes> perform(SignatureAlgorithm algorithm, ByteView digest, const CancelToken& cancel) {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "cancelled");
        if (auto policy = checkCreationAllowed(algorithm, info_.publicKeyType, info_.keyBits); !policy)
            return std::unexpected(policy.error());
        if (isGost(algorithm) || keyTypeOf(algorithm) != info_.publicKeyType)
            return fail(CryptoErrorCode::KeyMismatch, "key does not fit the signature algorithm");

        std::lock_guard<std::mutex> lock(mutex_); // serialise PIN + sign
        NKey key;
        DWORD spec = 0;
        BOOL mustFree = FALSE;
        HCRYPTPROV_OR_NCRYPT_KEY_HANDLE handle = 0;
        if (!CryptAcquireCertificatePrivateKey(cert_.p, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG, nullptr, &handle, &spec, &mustFree))
            return std::unexpected(
                mapSecurityStatus(static_cast<SECURITY_STATUS>(GetLastError()), "CryptAcquireCertificatePrivateKey"));
        key.h = handle;
        key.freeIt = mustFree != FALSE;

        if (prompt_) {
            PinRequest req;
            req.providerName = "windows-cng";
            req.label = label_;
            auto pin = prompt_->requestPin(req, cancel);
            if (!pin) return fail(CryptoErrorCode::Cancelled, "PIN entry cancelled");
            std::wstring wide;
            if (!pin->empty()) {
                const int n = MultiByteToWideChar(CP_UTF8, 0, pin->chars(), static_cast<int>(pin->size()), nullptr, 0);
                wide.assign(static_cast<std::size_t>(n), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, pin->chars(), static_cast<int>(pin->size()), wide.data(), n);
            }
            const SECURITY_STATUS ps =
                NCryptSetProperty(key.h, NCRYPT_PIN_PROPERTY, reinterpret_cast<PBYTE>(wide.data()),
                                  static_cast<DWORD>((wide.size() + 1) * sizeof(wchar_t)), 0);
            SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
            if (ps != ERROR_SUCCESS) return std::unexpected(mapSecurityStatus(ps, "NCryptSetProperty(PIN)"));
        }

        const bool pss = algorithm == SignatureAlgorithm::RsaPssSha256 || algorithm == SignatureAlgorithm::RsaPssSha384 ||
                         algorithm == SignatureAlgorithm::RsaPssSha512;
        BCRYPT_PKCS1_PADDING_INFO pkcs1{bcryptName(digestOf(algorithm))};
        BCRYPT_PSS_PADDING_INFO pssInfo{bcryptName(digestOf(algorithm)), static_cast<ULONG>(digest.size())};
        void* padding = nullptr;
        DWORD flags = 0;
        if (info_.publicKeyType == PublicKeyType::Rsa) {
            padding = pss ? static_cast<void*>(&pssInfo) : static_cast<void*>(&pkcs1);
            flags = pss ? BCRYPT_PAD_PSS : BCRYPT_PAD_PKCS1;
        }
        DWORD size = 0;
        SECURITY_STATUS s = NCryptSignHash(key.h, padding, const_cast<PBYTE>(digest.data()), static_cast<DWORD>(digest.size()),
                                           nullptr, 0, &size, flags);
        if (s != ERROR_SUCCESS) return std::unexpected(mapSecurityStatus(s, "NCryptSignHash(size)"));
        Bytes sig(size);
        s = NCryptSignHash(key.h, padding, const_cast<PBYTE>(digest.data()), static_cast<DWORD>(digest.size()), sig.data(), size,
                           &size, flags);
        if (s != ERROR_SUCCESS) return std::unexpected(mapSecurityStatus(s, "NCryptSignHash"));
        sig.resize(size);
        return info_.publicKeyType == PublicKeyType::Ec ? derFromRs(sig) : sig;
    }

    CertCtx cert_;
    CertificateInfo info_;
    std::vector<Bytes> chain_;
    std::shared_ptr<PinPrompt> prompt_;
    std::string label_;
    std::mutex mutex_;
};

// ---- store ---------------------------------------------------------------------------------------

HCERTSTORE openMyStore() {
    return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_CURRENT_USER | CERT_STORE_READONLY_FLAG, L"MY");
}

std::string friendlyLabel(PCCERT_CONTEXT c) {
    wchar_t buf[256] = {};
    const DWORD n = CertGetNameStringW(c, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, buf, 256);
    return n > 1 ? narrow(std::wstring(buf, n - 1)) : std::string();
}

std::vector<Bytes> buildChain(PCCERT_CONTEXT leaf, const Bytes& leafDer) {
    std::vector<Bytes> chain{leafDer};
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof(para);
    PCCERT_CHAIN_CONTEXT ctx = nullptr;
    if (!CertGetCertificateChain(nullptr, leaf, nullptr, nullptr, &para, CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL, nullptr, &ctx) || !ctx)
        return chain;
    if (ctx->cChain > 0 && ctx->rgpChain[0]) {
        PCERT_SIMPLE_CHAIN simple = ctx->rgpChain[0];
        for (DWORD i = 1; i < simple->cElement && chain.size() < kMaxChain; ++i) {
            PCCERT_CONTEXT e = simple->rgpElement[i]->pCertContext;
            chain.emplace_back(e->pbCertEncoded, e->pbCertEncoded + e->cbCertEncoded);
        }
    }
    CertFreeCertificateChain(ctx);
    return chain;
}

class CngStore final : public IdentityStore {
public:
    std::string_view name() const override { return "windows-cng"; }
    bool isAvailable() const override { return true; }
    ProviderAssurance assurance() const override { return ProviderAssurance::PlatformKeystore; }

    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken& cancel) override {
        HCERTSTORE store = openMyStore();
        if (!store) return fail(CryptoErrorCode::Io, "cannot open the current-user MY certificate store");
        std::vector<IdentityDescriptor> out;
        PCCERT_CONTEXT c = nullptr;
        while ((c = CertEnumCertificatesInStore(store, c)) != nullptr) {
            if (cancel.cancelled()) {
                CertFreeCertificateContext(c);
                CertCloseStore(store, 0);
                return fail(CryptoErrorCode::Cancelled, "cancelled");
            }
            auto info = parseX509(certBytes(c), digestFn);
            if (!info) continue; // skip what cannot be read
            DWORD cb = 0;
            const bool hasKey = CertGetCertificateContextProperty(c, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &cb) != FALSE;
            IdentityDescriptor d;
            d.id = info->sha256Fingerprint;
            d.providerName = "windows-cng";
            d.location.kind = KeyLocationKind::Platform;
            d.location.label = friendlyLabel(c);
            d.privateKeyAvailable = hasKey;
            d.certificateExpired = info->notAfter < std::chrono::system_clock::now();
            d.certificate = std::move(*info);
            out.push_back(std::move(d));
        }
        CertCloseStore(store, 0);
        return out;
    }

    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view id, std::shared_ptr<PinPrompt> prompt,
                                                        const CancelToken& cancel) override {
        HCERTSTORE store = openMyStore();
        if (!store) return fail(CryptoErrorCode::Io, "cannot open the current-user MY certificate store");
        PCCERT_CONTEXT c = nullptr;
        while ((c = CertEnumCertificatesInStore(store, c)) != nullptr) {
            if (cancel.cancelled()) {
                CertFreeCertificateContext(c);
                CertCloseStore(store, 0);
                return fail(CryptoErrorCode::Cancelled, "cancelled");
            }
            auto info = parseX509(certBytes(c), digestFn);
            if (!info || info->sha256Fingerprint != id) continue;
            CertCtx owned(CertDuplicateCertificateContext(c)); // outlives the store handle
            CertFreeCertificateContext(c);
            DWORD cb = 0;
            if (!CertGetCertificateContextProperty(owned.p, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &cb)) {
                CertCloseStore(store, 0);
                return fail(CryptoErrorCode::NoPrivateKey, "the certificate has no associated private key");
            }
            std::vector<Bytes> chain = buildChain(owned.p, info->der);
            std::string label = friendlyLabel(owned.p);
            CertificateInfo copy = std::move(*info);
            CertCloseStore(store, 0);
            return std::unique_ptr<SigningIdentity>(std::make_unique<CngSigningIdentity>(
                std::move(owned), std::move(copy), std::move(chain), std::move(prompt), std::move(label)));
        }
        CertCloseStore(store, 0);
        return fail(CryptoErrorCode::CertificateNotFound, "certificate not found in the Windows store");
    }
};

} // namespace

std::shared_ptr<IdentityStore> makeCngStore() { return std::make_shared<CngStore>(); }

} // namespace rivet::crypto::platform_detail

#endif // RIVET_CRYPTO_PLATFORM_WINDOWS
