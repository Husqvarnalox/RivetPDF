// SPDX-License-Identifier: MPL-2.0
// CryptoPro CSP backend: identity store, signing identity and CryptoProvider
// (hash / verify) over the CAPI10 / CAPI20 entry points. See ADR-0040.
//
// Threading: every CSP call is made while holding Csp::mutex (the CSP handles
// are not documented as freely shareable); PinPrompt callbacks are always
// invoked with no lock held.

#include "crypto/Der.hpp"
#include "crypto/X509Parse.hpp"
#include "crypto/providers/cryptopro/CryptoProApi.hpp"
#include "crypto/providers/cryptopro/CryptoProBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

namespace rivet::crypto::cryptopro_detail {

namespace {

constexpr int kMaxPinAttempts = 3;

// GOST signatures: the CSP's native (CAPI) value is little-endian; the value
// carried in CMS / X.509 is its byte reversal (ADR-0040). One definition so the
// convention can be changed in a single place if a vector ever disagrees.
Bytes reversed(ByteView in) { return Bytes(in.rbegin(), in.rend()); }

std::string narrow(const WCHAR* w) {
    std::string out;
    if (!w) return out;
    for (; *w; ++w) {
        auto cp = static_cast<std::uint32_t>(*w);
        if (sizeof(WCHAR) == 2 && cp >= 0xD800 && cp < 0xDC00 && w[1] >= 0xDC00 && w[1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<std::uint32_t>(w[1]) - 0xDC00);
            ++w;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string hex(DWORD v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", static_cast<unsigned>(v));
    return buf;
}

enum class ErrorClass { NeedsPin, WrongPin, PinBlocked, Removed, Cancelled, Other };

ErrorClass classify(DWORD e) {
    switch (static_cast<std::uint32_t>(e)) {
    case 0x8010006Bu: // SCARD_W_WRONG_CHV
    case 0x80090033u: // NTE_INCORRECT_PASSWORD
        return ErrorClass::WrongPin;
    case 0x8010006Cu: // SCARD_W_CHV_BLOCKED
        return ErrorClass::PinBlocked;
    case 0x80090022u: // NTE_SILENT_CONTEXT: a silent context needs the PIN supplied
    case 0x8009002Eu: // NTE_UI_REQUIRED
    case 0x80100066u: // SCARD_W_SECURITY_VIOLATION
    case 0x80090010u: // NTE_PERM
    case 5u:          // ERROR_ACCESS_DENIED
        return ErrorClass::NeedsPin;
    case 0x80100069u: // SCARD_W_REMOVED_CARD
    case 0x8010000Cu: // SCARD_E_NO_SMARTCARD
    case 0x80090035u: // NTE_DEVICE_NOT_FOUND
    case 0x8010002Eu: // SCARD_E_NO_READERS_AVAILABLE
        return ErrorClass::Removed;
    case 0x80090036u: // NTE_USER_CANCELLED
    case 1223u:       // ERROR_CANCELLED
        return ErrorClass::Cancelled;
    default:
        return ErrorClass::Other;
    }
}

CryptoError makeError(DWORD e, const char* op) {
    std::string msg = std::string(op) + " failed (CSP error " + hex(e) + ")";
    switch (classify(e)) {
    case ErrorClass::WrongPin: return {CryptoErrorCode::WrongPassword, msg};
    case ErrorClass::PinBlocked: return {CryptoErrorCode::PinLocked, msg};
    case ErrorClass::Removed: return {CryptoErrorCode::DeviceRemoved, msg};
    case ErrorClass::Cancelled: return {CryptoErrorCode::Cancelled, msg};
    case ErrorClass::NeedsPin: return {CryptoErrorCode::WrongPassword, msg + ": key is PIN protected"};
    default: break;
    }
    switch (static_cast<std::uint32_t>(e)) {
    case 0x80090016u: // NTE_BAD_KEYSET: container not found
    case 0x80090011u: // NTE_NOT_FOUND
        return {CryptoErrorCode::NoPrivateKey, msg + ": key container not found"};
    case 0x80090008u: // NTE_BAD_ALGID
    case 0x80090014u: // NTE_BAD_PROV_TYPE
    case 0x80090029u: // NTE_NOT_SUPPORTED
        return {CryptoErrorCode::UnsupportedAlgorithm, msg};
    case 0x8009001Du: // NTE_PROVIDER_DLL_FAIL
    case 0x8009001Eu: // NTE_PROV_DLL_NOT_FOUND
    case 0x80090013u: // NTE_BAD_PROVIDER
        return {CryptoErrorCode::BackendUnavailable, msg};
    default:
        return {CryptoErrorCode::Internal, msg};
    }
}

struct AlgInfo {
    ALG_ID alg;
    DWORD provType;
};

std::optional<AlgInfo> algOf(DigestAlgorithm d) {
    switch (d) {
    case DigestAlgorithm::Gost3411_2012_256: return AlgInfo{CALG_GR3411_2012_256, PROV_GOST_2012_256};
    case DigestAlgorithm::Gost3411_2012_512: return AlgInfo{CALG_GR3411_2012_512, PROV_GOST_2012_512};
    case DigestAlgorithm::Sha1: return AlgInfo{CALG_SHA1, PROV_GOST_2012_256};
    case DigestAlgorithm::Sha256: return AlgInfo{CALG_SHA_256, PROV_GOST_2012_256};
    default: return std::nullopt;
    }
}

// Shared CSP state: the loaded API and cached verify-only contexts.
struct Csp {
    std::shared_ptr<const CspApi> api;
    CryptoProOptions options;
    std::mutex mutex;
    std::map<DWORD, HCRYPTPROV> verifyContexts; // by provider type; guarded by mutex

    ~Csp() {
        for (auto& [type, prov] : verifyContexts) api->releaseContext(prov, 0);
    }

    // Caller holds mutex.
    CryptoResult<HCRYPTPROV> verifyContext(DWORD provType) {
        if (auto it = verifyContexts.find(provType); it != verifyContexts.end()) return it->second;
        HCRYPTPROV prov = 0;
        if (!api->acquireContext(&prov, nullptr, nullptr, provType, CRYPT_VERIFYCONTEXT | CRYPT_SILENT))
            return std::unexpected(makeError(api->getLastError(), "CryptAcquireContext"));
        verifyContexts[provType] = prov;
        return prov;
    }
};

struct HashGuard {
    const CspApi* api;
    HCRYPTHASH h = 0;
    ~HashGuard() {
        if (h) api->destroyHash(h);
    }
};
struct KeyGuard {
    const CspApi* api;
    HCRYPTKEY k = 0;
    ~KeyGuard() {
        if (k) api->destroyKey(k);
    }
};
struct ProvGuard {
    const CspApi* api;
    HCRYPTPROV p = 0;
    ~ProvGuard() {
        if (p) api->releaseContext(p, 0);
    }
};
struct StoreGuard {
    const CspApi* api;
    HCERTSTORE s = nullptr;
    ~StoreGuard() {
        if (s) api->closeStore(s, 0);
    }
};

// ---- hashing ---------------------------------------------------------------

class CspHasher final : public Hasher {
public:
    CspHasher(std::shared_ptr<Csp> csp, DigestAlgorithm alg) : csp_(std::move(csp)), alg_(alg) {
        auto info = *algOf(alg);
        std::scoped_lock lock(csp_->mutex);
        auto ctx = csp_->verifyContext(info.provType);
        if (!ctx) {
            ok_ = false;
            return;
        }
        ok_ = csp_->api->createHash(*ctx, info.alg, 0, 0, &hash_) != 0;
    }
    ~CspHasher() override {
        if (hash_) csp_->api->destroyHash(hash_);
    }
    bool ok() const { return ok_; }

    void update(ByteView data) override {
        if (!ok_ || data.empty()) return;
        std::scoped_lock lock(csp_->mutex);
        // CSP takes a 32-bit length: feed large inputs in bounded pieces.
        constexpr std::size_t kChunk = std::size_t{1} << 28;
        for (std::size_t off = 0; off < data.size(); off += kChunk) {
            auto n = std::min(kChunk, data.size() - off);
            if (!csp_->api->hashData(hash_, data.data() + off, static_cast<DWORD>(n), 0)) {
                ok_ = false;
                return;
            }
        }
    }

    // Returns an empty vector when the CSP failed (Hasher has no error channel).
    Bytes finish() override {
        if (!ok_) return {};
        std::scoped_lock lock(csp_->mutex);
        Bytes out(digestSize(alg_));
        DWORD len = static_cast<DWORD>(out.size());
        if (!csp_->api->getHashParam(hash_, HP_HASHVAL, out.data(), &len, 0) || len != out.size()) {
            ok_ = false;
            return {};
        }
        return out;
    }

private:
    std::shared_ptr<Csp> csp_;
    DigestAlgorithm alg_;
    HCRYPTHASH hash_ = 0;
    bool ok_ = false;
};

CryptoResult<Bytes> cspDigest(const std::shared_ptr<Csp>& csp, DigestAlgorithm alg, ByteView data) {
    if (!algOf(alg)) return fail(CryptoErrorCode::UnsupportedAlgorithm, "digest not provided by the CryptoPro CSP provider");
    CspHasher h(csp, alg);
    if (!h.ok()) return fail(CryptoErrorCode::Internal, "CSP could not start the hash");
    h.update(data);
    auto out = h.finish();
    if (out.empty()) return fail(CryptoErrorCode::Internal, "CSP hash failed");
    return out;
}

// ---- certificate helpers ----------------------------------------------------

CryptoResult<CertificateInfo> parseWithCsp(const std::shared_ptr<Csp>& csp, ByteView der) {
    DigestFn fn = [csp](DigestAlgorithm a, ByteView d) { return cspDigest(csp, a, d); };
    return parseX509(der, fn);
}

// Splits a certificate into TBSCertificate, signatureAlgorithm OID and signature bytes.
struct CertParts {
    Bytes tbs;
    std::string sigOid;
    Bytes signature;
};

std::optional<CertParts> splitCertificate(ByteView der) {
    auto root = der::readSingle(der);
    if (!root) return std::nullopt;
    auto kids = der::children(*root);
    if (!kids || kids->size() != 3) return std::nullopt;
    auto algKids = der::children((*kids)[1]);
    if (!algKids || algKids->empty() || (*algKids)[0].tag != der::kOid) return std::nullopt;
    auto oid = der::decodeOid((*algKids)[0].content);
    if (!oid) return std::nullopt;
    const auto& sig = (*kids)[2];
    if (sig.tag != der::kBitString || sig.content.size() < 2 || sig.content[0] != 0) return std::nullopt;
    CertParts p;
    p.tbs.assign((*kids)[0].full.begin(), (*kids)[0].full.end());
    p.sigOid = *oid;
    p.signature.assign(sig.content.begin() + 1, sig.content.end());
    return p;
}

bool isGostKey(PublicKeyType t) { return t == PublicKeyType::Gost3410_2012_256 || t == PublicKeyType::Gost3410_2012_512; }

// ---- verification -----------------------------------------------------------

CryptoOk verifyGost(const std::shared_ptr<Csp>& csp, const CertificateInfo& signer, DigestAlgorithm digest,
                    ByteView message, ByteView signature, bool messageIsDigest = false) {
    if (!isGostKey(signer.publicKeyType)) return fail(CryptoErrorCode::KeyMismatch, "signer key is not a GOST R 34.10-2012 key");
    const bool is512 = signer.publicKeyType == PublicKeyType::Gost3410_2012_512;
    if (digest != (is512 ? DigestAlgorithm::Gost3411_2012_512 : DigestAlgorithm::Gost3411_2012_256))
        return fail(CryptoErrorCode::KeyMismatch, "digest does not match the GOST key size");
    const std::size_t sigLen = is512 ? 128 : 64;
    if (signature.size() != sigLen) return fail(CryptoErrorCode::CorruptData, "GOST signature has the wrong length");
    const auto provType = is512 ? PROV_GOST_2012_512 : PROV_GOST_2012_256;
    const auto native = reversed(signature);
    std::scoped_lock lock(csp->mutex);
    const auto& api = *csp->api;
    auto prov = csp->verifyContext(provType);
    if (!prov) return std::unexpected(prov.error());
    PCCERT_CONTEXT ctx = api.createCertificateContext(X509_ASN_ENCODING, signer.der.data(), static_cast<DWORD>(signer.der.size()));
    if (!ctx) return fail(CryptoErrorCode::CorruptData, "CSP rejected the signer certificate");
    KeyGuard key{&api};
    const BOOL imported = api.importPublicKeyInfo(*prov, X509_ASN_ENCODING, &ctx->pCertInfo->SubjectPublicKeyInfo, &key.k);
    const DWORD importErr = imported ? 0 : api.getLastError();
    api.freeCertificateContext(ctx);
    if (!imported) return std::unexpected(makeError(importErr, "CryptImportPublicKeyInfo"));
    HashGuard hash{&api};
    if (!api.createHash(*prov, is512 ? CALG_GR3411_2012_512 : CALG_GR3411_2012_256, 0, 0, &hash.h))
        return std::unexpected(makeError(api.getLastError(), "CryptCreateHash"));
    if (messageIsDigest) {
        if (message.size() != digestSize(digest)) return fail(CryptoErrorCode::InvalidArgument, "digest length does not match the key");
        if (!api.setHashParam(hash.h, HP_HASHVAL, message.data(), 0))
            return std::unexpected(makeError(api.getLastError(), "CryptSetHashParam"));
    }
    constexpr std::size_t kChunk = std::size_t{1} << 28;
    for (std::size_t off = 0; !messageIsDigest && off < message.size(); off += kChunk) {
        auto n = std::min(kChunk, message.size() - off);
        if (!api.hashData(hash.h, message.data() + off, static_cast<DWORD>(n), 0))
            return std::unexpected(makeError(api.getLastError(), "CryptHashData"));
    }
    if (!api.verifySignature(hash.h, native.data(), static_cast<DWORD>(native.size()), key.k, nullptr, 0)) {
        const DWORD e = api.getLastError();
        if (e == static_cast<DWORD>(NTE_BAD_SIGNATURE)) return fail(CryptoErrorCode::CorruptData, "GOST signature is invalid");
        return std::unexpected(makeError(e, "CryptVerifySignature"));
    }
    return {};
}

// ---- provider ---------------------------------------------------------------

class CspProvider final : public CryptoProvider, public GostDigestVerifier {
public:
    explicit CspProvider(std::shared_ptr<Csp> csp) : csp_(std::move(csp)) {
        for (auto d : {DigestAlgorithm::Sha1, DigestAlgorithm::Sha256, DigestAlgorithm::Gost3411_2012_256,
                       DigestAlgorithm::Gost3411_2012_512}) {
            CspHasher h(csp_, d);
            if (h.ok()) supported_.push_back(d);
        }
    }

    std::string_view name() const override { return "cryptopro"; }
    bool isAvailable() const override { return true; }
    bool supports(DigestAlgorithm d) const override {
        return std::find(supported_.begin(), supported_.end(), d) != supported_.end();
    }
    bool supports(SignatureAlgorithm a) const override {
        // Verification only: creation goes through the SigningIdentity.
        return (a == SignatureAlgorithm::Gost3410_2012_256 && supports(DigestAlgorithm::Gost3411_2012_256)) ||
               (a == SignatureAlgorithm::Gost3410_2012_512 && supports(DigestAlgorithm::Gost3411_2012_512));
    }

    CryptoResult<std::unique_ptr<Hasher>> createHasher(DigestAlgorithm d) override {
        if (!supports(d)) return fail(CryptoErrorCode::UnsupportedAlgorithm, "digest not provided by the CryptoPro CSP provider");
        auto h = std::make_unique<CspHasher>(csp_, d);
        if (!h->ok()) return fail(CryptoErrorCode::Internal, "CSP could not start the hash");
        return std::unique_ptr<Hasher>(std::move(h));
    }

    CryptoResult<Bytes> randomBytes(std::size_t count) override {
        std::scoped_lock lock(csp_->mutex);
        auto prov = csp_->verifyContext(PROV_GOST_2012_256);
        if (!prov) return std::unexpected(prov.error());
        Bytes out(count);
        if (count > 0 && !csp_->api->genRandom(*prov, static_cast<DWORD>(count), out.data()))
            return std::unexpected(makeError(csp_->api->getLastError(), "CryptGenRandom"));
        return out;
    }

    CryptoResult<CertificateInfo> parseCertificate(ByteView der) override { return parseWithCsp(csp_, der); }

    bool isIssuedBy(const CertificateInfo& cert, const CertificateInfo& issuer) override {
        if (cert.issuerDer != issuer.subjectDer) return false;
        auto parts = splitCertificate(cert.der);
        if (!parts) return false;
        DigestAlgorithm d;
        if (parts->sigOid == oid::kGost3410_256) d = DigestAlgorithm::Gost3411_2012_256;
        else if (parts->sigOid == oid::kGost3410_512) d = DigestAlgorithm::Gost3411_2012_512;
        else return false; // non-GOST issuers are the OpenSSL provider's business
        return verifyGost(csp_, issuer, d, parts->tbs, parts->signature).has_value();
    }

    CryptoOk verifySignature(const CertificateInfo& signer, const SignatureScheme& scheme, ByteView message,
                             ByteView signature) override {
        if (scheme.padding != SignaturePadding::Gost) return fail(CryptoErrorCode::UnsupportedAlgorithm, "not a GOST signature scheme");
        return verifyGost(csp_, signer, scheme.digest, message, signature);
    }

    CryptoOk verifyDigest(const CertificateInfo& signer, ByteView digest, ByteView signature) override {
        const auto d = signer.publicKeyType == PublicKeyType::Gost3410_2012_512 ? DigestAlgorithm::Gost3411_2012_512
                                                                                 : DigestAlgorithm::Gost3411_2012_256;
        return verifyGost(csp_, signer, d, digest, signature, true);
    }

    CryptoResult<Pkcs12Contents> importPkcs12(ByteView, const SecretBuffer&) override {
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "PKCS#12 import is not provided by the CryptoPro CSP provider");
    }
    CryptoResult<std::unique_ptr<SigningIdentity>> createSelfSignedIdentity(const SelfSignedRequest&) override {
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "the CryptoPro CSP provider does not create identities");
    }
    CryptoResult<Bytes> exportPkcs12(const SigningIdentity&, const SecretBuffer&) override {
        return fail(CryptoErrorCode::NotExportable, "CryptoPro private keys are never exported");
    }

private:
    std::shared_ptr<Csp> csp_;
    std::vector<DigestAlgorithm> supported_;
};

// ---- identities -------------------------------------------------------------

struct KeyRecord {
    IdentityDescriptor descriptor;
    std::string container;
    std::string providerName;
    DWORD provType = 0;
    DWORD keySpec = 0;
};

std::string idFor(const CertificateInfo& c) {
    return "cryptopro:" + (c.sha256Fingerprint.empty() ? c.sha1Fingerprint : c.sha256Fingerprint);
}

class CspIdentity final : public SigningIdentity {
public:
    CspIdentity(std::shared_ptr<Csp> csp, KeyRecord rec, std::vector<Bytes> chain, std::shared_ptr<PinPrompt> prompt)
        : csp_(std::move(csp)), rec_(std::move(rec)), chain_(std::move(chain)), prompt_(std::move(prompt)) {}

    std::string_view providerName() const override { return "cryptopro"; }
    const CertificateInfo& certificate() const override { return rec_.descriptor.certificate; }
    std::vector<Bytes> chainDer() const override { return chain_; }
    PublicKeyType keyType() const override { return certificate().publicKeyType; }
    int keyBits() const override { return certificate().keyBits; }
    std::vector<SignatureAlgorithm> supportedSignatures() const override {
        if (keyType() == PublicKeyType::Gost3410_2012_256) return {SignatureAlgorithm::Gost3410_2012_256};
        if (keyType() == PublicKeyType::Gost3410_2012_512) return {SignatureAlgorithm::Gost3410_2012_512};
        return {};
    }

    CryptoResult<Bytes> sign(SignatureAlgorithm a, ByteView message, const CancelToken& cancel) override {
        return signImpl(a, message, false, cancel);
    }
    CryptoResult<Bytes> signDigest(SignatureAlgorithm a, ByteView digest, const CancelToken& cancel) override {
        return signImpl(a, digest, true, cancel);
    }

private:
    CryptoResult<Bytes> signImpl(SignatureAlgorithm a, ByteView data, bool isDigest, const CancelToken& cancel) {
        const auto kt = keyType();
        if (!isGostKey(kt) || keyTypeOf(a) != kt)
            return fail(CryptoErrorCode::KeyMismatch, "signature algorithm does not match the CryptoPro key");
        if (auto ok = checkCreationAllowed(a, kt, keyBits()); !ok) return std::unexpected(ok.error());
        const auto digestAlg = digestOf(a);
        if (isDigest && data.size() != digestSize(digestAlg))
            return fail(CryptoErrorCode::InvalidArgument, "digest length does not match the algorithm");

        std::optional<SecretBuffer> pin;
        PinPromptReason reason = PinPromptReason::Required;
        for (int attempt = 1;; ++attempt) {
            if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "signing cancelled");
            DWORD err = 0;
            auto result = trySign(digestAlg, data, isDigest, pin ? &*pin : nullptr, err);
            if (result) return reversed(*result);
            switch (classify(err)) {
            case ErrorClass::NeedsPin:
            case ErrorClass::WrongPin: {
                if (attempt > kMaxPinAttempts) return fail(CryptoErrorCode::WrongPassword, "PIN rejected");
                if (!prompt_) return fail(CryptoErrorCode::Cancelled, "key requires a PIN and no prompt is available");
                if (classify(err) == ErrorClass::WrongPin) {
                    reason = attempt >= kMaxPinAttempts ? PinPromptReason::LastAttempt : PinPromptReason::Incorrect;
                    if (attempt >= kMaxPinAttempts) return std::unexpected(makeError(err, "CryptSignHash"));
                }
                PinRequest req;
                req.providerName = "cryptopro";
                req.label = rec_.descriptor.location.label;
                req.reason = reason;
                req.attempt = attempt;
                pin = prompt_->requestPin(req, cancel); // no lock held
                if (!pin || cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "PIN entry cancelled");
                break;
            }
            default:
                return std::unexpected(makeError(err, "CryptSignHash"));
            }
        }
    }

    // One attempt on a fresh silent context. `err` receives the CSP error code.
    std::optional<Bytes> trySign(DigestAlgorithm digestAlg, ByteView data, bool isDigest, const SecretBuffer* pin, DWORD& err) {
        std::scoped_lock lock(csp_->mutex);
        const auto& api = *csp_->api;
        ProvGuard prov{&api};
        if (!api.acquireContext(&prov.p, rec_.container.c_str(), rec_.providerName.c_str(), rec_.provType, CRYPT_SILENT)) {
            err = api.getLastError();
            return std::nullopt;
        }
        if (pin) {
            const DWORD param = rec_.keySpec == AT_SIGNATURE ? PP_SIGNATURE_PIN : PP_KEYEXCHANGE_PIN;
            if (!api.setProvParam(prov.p, param, reinterpret_cast<const BYTE*>(pin->chars()), 0)) {
                err = api.getLastError();
                return std::nullopt;
            }
        }
        HashGuard hash{&api};
        if (!api.createHash(prov.p, algOf(digestAlg)->alg, 0, 0, &hash.h)) {
            err = api.getLastError();
            return std::nullopt;
        }
        const BOOL fed = isDigest ? api.setHashParam(hash.h, HP_HASHVAL, data.data(), 0) : feed(api, hash.h, data);
        if (!fed) {
            err = api.getLastError();
            return std::nullopt;
        }
        DWORD len = 0;
        if (!api.signHash(hash.h, rec_.keySpec, nullptr, 0, nullptr, &len)) {
            err = api.getLastError();
            return std::nullopt;
        }
        Bytes sig(len);
        if (!api.signHash(hash.h, rec_.keySpec, nullptr, 0, sig.data(), &len)) {
            err = api.getLastError();
            return std::nullopt;
        }
        sig.resize(len);
        return sig;
    }

    static BOOL feed(const CspApi& api, HCRYPTHASH h, ByteView data) {
        constexpr std::size_t kChunk = std::size_t{1} << 28;
        for (std::size_t off = 0; off < data.size(); off += kChunk) {
            auto n = std::min(kChunk, data.size() - off);
            if (!api.hashData(h, data.data() + off, static_cast<DWORD>(n), 0)) return FALSE;
        }
        return TRUE;
    }

    std::shared_ptr<Csp> csp_;
    KeyRecord rec_;
    std::vector<Bytes> chain_;
    std::shared_ptr<PinPrompt> prompt_;
};

// ---- store ------------------------------------------------------------------

class CspStore final : public IdentityStore {
public:
    CspStore(std::shared_ptr<Csp> csp, std::shared_ptr<CryptoProvider> provider)
        : csp_(std::move(csp)), provider_(std::move(provider)) {}

    std::string_view name() const override { return "cryptopro"; }
    bool isAvailable() const override { return true; }
    ProviderAssurance assurance() const override { return ProviderAssurance::CertifiedCspDeclared; }

    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken& cancel) override {
        auto recs = enumerate(cancel);
        if (!recs) return std::unexpected(recs.error());
        std::vector<IdentityDescriptor> out;
        for (auto& r : *recs) out.push_back(std::move(r.descriptor));
        return out;
    }

    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view id, std::shared_ptr<PinPrompt> prompt,
                                                        const CancelToken& cancel) override {
        auto recs = enumerate(cancel);
        if (!recs) return std::unexpected(recs.error());
        for (auto& r : *recs) {
            if (r.descriptor.id != id) continue;
            if (!r.descriptor.privateKeyAvailable) return fail(CryptoErrorCode::NoPrivateKey, "private key container not available");
            auto chain = buildChainDer(r.descriptor.certificate);
            return std::unique_ptr<SigningIdentity>(
                std::make_unique<CspIdentity>(csp_, std::move(r), std::move(chain), std::move(prompt)));
        }
        return fail(CryptoErrorCode::CertificateNotFound, "no such CryptoPro identity");
    }

private:
    std::vector<Bytes> readStoreDer(const char* storeName) {
        std::vector<Bytes> out;
        std::scoped_lock lock(csp_->mutex);
        const auto& api = *csp_->api;
        StoreGuard store{&api, api.openSystemStore(0, storeName)};
        if (!store.s) return out;
        PCCERT_CONTEXT ctx = nullptr;
        while ((ctx = api.enumCertificates(store.s, ctx)) != nullptr)
            out.emplace_back(ctx->pbCertEncoded, ctx->pbCertEncoded + ctx->cbCertEncoded);
        return out;
    }

    std::vector<Bytes> buildChainDer(const CertificateInfo& leaf) {
        std::vector<CertificateInfo> pool;
        auto add = [&](const std::vector<Bytes>& ders) {
            for (const auto& d : ders)
                if (auto c = parseWithCsp(csp_, d)) pool.push_back(std::move(*c));
        };
        add(readStoreDer("CA"));
        add(readStoreDer("ROOT"));
        add(readStoreDer(csp_->options.storeName.c_str()));
        add(csp_->options.extraChainCertificates);
        auto result = buildChain(leaf, pool, TrustAnchorSet{}, *provider_);
        std::vector<Bytes> out;
        for (const auto& c : result.chain) out.push_back(c.der);
        if (out.empty()) out.push_back(leaf.der);
        return out;
    }

    CryptoResult<std::vector<KeyRecord>> enumerate(const CancelToken& cancel) {
        std::vector<KeyRecord> out;
        std::scoped_lock lock(csp_->mutex);
        const auto& api = *csp_->api;
        StoreGuard store{&api, api.openSystemStore(0, csp_->options.storeName.c_str())};
        if (!store.s) return std::unexpected(makeError(api.getLastError(), "CertOpenSystemStore"));
        const auto now = std::chrono::system_clock::now();
        PCCERT_CONTEXT ctx = nullptr;
        while ((ctx = api.enumCertificates(store.s, ctx)) != nullptr) {
            if (cancel.cancelled()) {
                api.freeCertificateContext(ctx);
                return fail(CryptoErrorCode::Cancelled, "enumeration cancelled");
            }
            DWORD size = 0;
            if (!api.getCertificateProperty(ctx, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) || size == 0) continue; // no key link
            std::vector<std::uint8_t> raw(size);
            if (!api.getCertificateProperty(ctx, CERT_KEY_PROV_INFO_PROP_ID, raw.data(), &size)) continue;
            const auto* info = reinterpret_cast<const CRYPT_KEY_PROV_INFO*>(raw.data());
            if (info->dwProvType != PROV_GOST_2012_256 && info->dwProvType != PROV_GOST_2012_512) continue;

            Bytes der(ctx->pbCertEncoded, ctx->pbCertEncoded + ctx->cbCertEncoded);
            // Parsing hashes through the CSP, which takes the same (non-recursive) mutex: use a lock-free helper.
            auto parsed = parseUnlocked(der);
            if (!parsed) continue;

            KeyRecord rec;
            rec.container = narrow(info->pwszContainerName);
            rec.providerName = narrow(info->pwszProvName);
            rec.provType = info->dwProvType;
            rec.keySpec = info->dwKeySpec ? info->dwKeySpec : AT_KEYEXCHANGE;
            auto& d = rec.descriptor;
            d.certificate = std::move(*parsed);
            d.id = idFor(d.certificate);
            d.providerName = "cryptopro";
            d.location.kind = KeyLocationKind::Container;
            d.location.label = rec.container;
            d.location.device = rec.providerName;
            d.certificateExpired = now > d.certificate.notAfter;
            d.privateKeyAvailable = keyReachable(rec);
            out.push_back(std::move(rec));
        }
        return out;
    }

    // Caller holds csp_->mutex.
    CryptoResult<CertificateInfo> parseUnlocked(ByteView der) {
        const auto& api = *csp_->api;
        DigestFn fn = [this, &api](DigestAlgorithm a, ByteView data) -> CryptoResult<Bytes> {
            auto info = algOf(a);
            if (!info) return fail(CryptoErrorCode::UnsupportedAlgorithm, "digest not available");
            auto prov = csp_->verifyContext(info->provType);
            if (!prov) return std::unexpected(prov.error());
            HashGuard h{&api};
            if (!api.createHash(*prov, info->alg, 0, 0, &h.h)) return std::unexpected(makeError(api.getLastError(), "CryptCreateHash"));
            if (!data.empty() && !api.hashData(h.h, data.data(), static_cast<DWORD>(data.size()), 0))
                return std::unexpected(makeError(api.getLastError(), "CryptHashData"));
            Bytes out(digestSize(a));
            DWORD len = static_cast<DWORD>(out.size());
            if (!api.getHashParam(h.h, HP_HASHVAL, out.data(), &len, 0)) return std::unexpected(makeError(api.getLastError(), "CryptGetHashParam"));
            return out;
        };
        return parseX509(der, fn);
    }

    // True when the container opens silently and holds the key. Caller holds the mutex.
    bool keyReachable(const KeyRecord& rec) {
        const auto& api = *csp_->api;
        ProvGuard prov{&api};
        if (!api.acquireContext(&prov.p, rec.container.c_str(), rec.providerName.c_str(), rec.provType, CRYPT_SILENT)) return false;
        KeyGuard key{&api};
        return api.getUserKey(prov.p, rec.keySpec, &key.k) != 0;
    }

    std::shared_ptr<Csp> csp_;
    std::shared_ptr<CryptoProvider> provider_;
};

std::shared_ptr<Csp> loadCsp(const CryptoProOptions& options, std::string& reason) {
    auto api = CspApi::load(options.libraryDirectory, reason);
    if (!api) return nullptr;
    auto csp = std::make_shared<Csp>();
    csp->api = std::move(api);
    csp->options = options;
    // The CSP must be usable (installed provider types); report instead of failing later.
    HCRYPTPROV prov = 0;
    if (!csp->api->acquireContext(&prov, nullptr, nullptr, PROV_GOST_2012_256, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        reason = "CryptoPro CSP is installed but the GOST R 34.10-2012 provider cannot be opened (" +
                 hex(csp->api->getLastError()) + "); check the CSP installation and licence";
        return nullptr;
    }
    csp->api->releaseContext(prov, 0);
    return csp;
}

} // namespace

std::shared_ptr<IdentityStore> makeBackendStore(const CryptoProOptions& options, std::string& reason) {
    auto csp = loadCsp(options, reason);
    if (!csp) return nullptr;
    return std::make_shared<CspStore>(csp, std::make_shared<CspProvider>(csp));
}

std::shared_ptr<CryptoProvider> makeBackendProvider(const CryptoProOptions& options, std::string& reason) {
    auto csp = loadCsp(options, reason);
    if (!csp) return nullptr;
    return std::make_shared<CspProvider>(csp);
}

} // namespace rivet::crypto::cryptopro_detail
