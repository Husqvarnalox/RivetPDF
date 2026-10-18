// SPDX-License-Identifier: MPL-2.0
#include "crypto/providers/pkcs11/Pkcs11IdentityStore.hpp"

#include "crypto/Algorithms.hpp"
#include "crypto/Der.hpp"
#include "crypto/X509Parse.hpp"

#if RIVET_CRYPTO_PKCS11
#include "crypto/providers/pkcs11/Pkcs11Module.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#endif

namespace rivet::crypto {

#if RIVET_CRYPTO_PKCS11

namespace {

using namespace p11;

constexpr std::size_t kMaxSlots = 128;
constexpr std::size_t kMaxCertificatesPerToken = 128;
constexpr std::size_t kMaxKeysPerToken = 256;
constexpr std::size_t kMaxCertificateSize = 64 * 1024;
constexpr std::size_t kMaxChain = 10;
constexpr int kMaxPinAttempts = 3;
// Messages above this size are hashed in software when possible instead of
// being handed to a token in one C_Sign call (small tokens reject big inputs).
constexpr std::size_t kMaxTokenMessage = 64 * 1024;

struct Config {
    Pkcs11Options options;
    std::shared_ptr<CryptoProvider> hasher;
    DigestFn digestFn; // empty when no software provider is available

    std::string display() const { return options.displayName.empty() ? "PKCS#11" : options.displayName; }
    bool canHash(DigestAlgorithm d) const { return hasher && hasher->isAvailable() && hasher->supports(d); }
};

std::string hexOf(ByteView data) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (const std::uint8_t b : data) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

std::optional<Bytes> fromHex(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    out.reserve(text.size() / 2);
    for (std::size_t i = 0; i < text.size(); i += 2) {
        const int hi = nibble(text[i]);
        const int lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
    }
    return out;
}

// Serial numbers enter identity ids: keep them free of the '/' separator.
std::string sanitizeSerial(std::string_view serial) {
    std::string out;
    for (const char c : serial) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' ||
                        c == '-' || c == '+';
        out.push_back(ok ? c : '_');
    }
    return out.empty() ? std::string("none") : out;
}

CryptoResult<CertificateInfo> parseCertificate(const Config& cfg, ByteView der) {
    return parseX509(der, cfg.digestFn);
}

// ---- token discovery ----------------------------------------------------

struct TokenRef {
    CK_SLOT_ID slot = 0;
    CK_TOKEN_INFO info{};
    std::string label;
    std::string serial; // trimmed
    std::string slotDescription;
};

CryptoResult<std::vector<TokenRef>> presentTokens(const Pkcs11Module& module, const CancelToken& cancel) {
    CK_ULONG count = 0;
    CK_RV rv = module.call(&CK_FUNCTION_LIST::C_GetSlotList, CK_BBOOL{CK_TRUE}, static_cast<CK_SLOT_ID*>(nullptr), &count);
    if (rv != CKR_OK) return std::unexpected(mapRv(rv, "cannot enumerate slots"));
    std::vector<CK_SLOT_ID> slots;
    bool listed = false;
    for (int attempt = 0; attempt < 3 && !listed; ++attempt) {
        if (count == 0) return std::vector<TokenRef>{};
        if (count > kMaxSlots) count = kMaxSlots;
        slots.assign(count, 0);
        CK_ULONG got = count;
        rv = module.call(&CK_FUNCTION_LIST::C_GetSlotList, CK_BBOOL{CK_TRUE}, slots.data(), &got);
        if (rv == CKR_BUFFER_TOO_SMALL) { // the set of tokens changed between the two calls
            rv = module.call(&CK_FUNCTION_LIST::C_GetSlotList, CK_BBOOL{CK_TRUE}, static_cast<CK_SLOT_ID*>(nullptr), &count);
            if (rv != CKR_OK) return std::unexpected(mapRv(rv, "cannot enumerate slots"));
            continue;
        }
        if (rv != CKR_OK) return std::unexpected(mapRv(rv, "cannot enumerate slots"));
        slots.resize(std::min<CK_ULONG>(got, count));
        listed = true;
    }
    if (!listed) return fail(CryptoErrorCode::Io, "the slot list kept changing during enumeration");
    std::vector<TokenRef> tokens;
    for (const CK_SLOT_ID slot : slots) {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        TokenRef ref;
        ref.slot = slot;
        if (module.call(&CK_FUNCTION_LIST::C_GetTokenInfo, slot, &ref.info) != CKR_OK) continue; // removed meanwhile
        ref.label = trimField(ref.info.label, sizeof ref.info.label);
        ref.serial = trimField(ref.info.serialNumber, sizeof ref.info.serialNumber);
        CK_SLOT_INFO slotInfo{};
        if (module.call(&CK_FUNCTION_LIST::C_GetSlotInfo, slot, &slotInfo) == CKR_OK)
            ref.slotDescription = trimField(slotInfo.slotDescription, sizeof slotInfo.slotDescription);
        tokens.push_back(std::move(ref));
    }
    return tokens;
}

struct TokenCertificate {
    Bytes id; // CKA_ID
    std::string label;
    CertificateInfo info;
};

CryptoResult<std::vector<TokenCertificate>> readCertificates(const Config& cfg, const Pkcs11Session& session,
                                                             const CancelToken& cancel) {
    CK_OBJECT_CLASS cls = CKO_CERTIFICATE;
    std::vector<CK_OBJECT_HANDLE> handles;
    const CK_RV rv = session.find({CK_ATTRIBUTE{CKA_CLASS, &cls, sizeof cls}}, kMaxCertificatesPerToken, handles);
    if (rv != CKR_OK) return std::unexpected(mapRv(rv, "cannot enumerate certificates"));
    std::vector<TokenCertificate> out;
    for (const CK_OBJECT_HANDLE handle : handles) {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        CK_ULONG type = CKC_X_509;
        if (session.getUlong(handle, CKA_CERTIFICATE_TYPE, type) == CKR_OK && type != CKC_X_509) continue;
        TokenCertificate cert;
        Bytes der;
        if (session.getBytes(handle, CKA_VALUE, der, kMaxCertificateSize) != CKR_OK || der.empty()) continue;
        auto parsed = parseCertificate(cfg, der);
        if (!parsed) continue; // unreadable certificate objects are skipped, not fatal
        cert.info = std::move(*parsed);
        if (cert.info.der.empty()) cert.info.der = std::move(der);
        Bytes label;
        if (session.getBytes(handle, CKA_LABEL, label, 256) == CKR_OK) cert.label = trimField(label.data(), label.size());
        (void)session.getBytes(handle, CKA_ID, cert.id, 256);
        out.push_back(std::move(cert));
    }
    return out;
}

// CKA_ID values of the key objects of `cls` visible in this session.
std::vector<Bytes> keyIds(const Pkcs11Session& session, CK_OBJECT_CLASS cls) {
    std::vector<CK_OBJECT_HANDLE> handles;
    std::vector<Bytes> ids;
    if (session.find({CK_ATTRIBUTE{CKA_CLASS, &cls, sizeof cls}}, kMaxKeysPerToken, handles) != CKR_OK) return ids;
    for (const CK_OBJECT_HANDLE handle : handles) {
        Bytes id;
        if (session.getBytes(handle, CKA_ID, id, 256) == CKR_OK && !id.empty()) ids.push_back(std::move(id));
    }
    return ids;
}

bool contains(const std::vector<Bytes>& ids, const Bytes& id) {
    return !id.empty() && std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Finds the private key paired with `id`: CKR_OK, CKR_KEY_HANDLE_INVALID when
// absent (or hidden because the session is not logged in), or a module error.
CK_RV findPrivateKey(const Pkcs11Session& session, const Bytes& id, PublicKeyType type, CK_OBJECT_HANDLE& out) {
    if (id.empty()) return CKR_KEY_HANDLE_INVALID;
    CK_OBJECT_CLASS cls = CKO_PRIVATE_KEY;
    std::vector<CK_OBJECT_HANDLE> handles;
    const CK_RV rv = session.find({CK_ATTRIBUTE{CKA_CLASS, &cls, sizeof cls},
                                   CK_ATTRIBUTE{CKA_ID, const_cast<std::uint8_t*>(id.data()), static_cast<CK_ULONG>(id.size())}},
                                  8, handles);
    if (rv != CKR_OK) return rv;
    if (handles.empty()) return CKR_KEY_HANDLE_INVALID;
    int bestScore = -1;
    for (const CK_OBJECT_HANDLE handle : handles) {
        int score = 0;
        bool canSign = true;
        if (session.getBool(handle, CKA_SIGN, canSign) == CKR_OK && !canSign) score -= 2;
        CK_ULONG keyType = 0;
        if (session.getUlong(handle, CKA_KEY_TYPE, keyType) == CKR_OK) {
            if (type == PublicKeyType::Rsa && keyType == CKK_RSA) score += 1;
            if (type == PublicKeyType::Ec && keyType == CKK_EC) score += 1;
        }
        if (score > bestScore) {
            bestScore = score;
            out = handle;
        }
    }
    return CKR_OK;
}

std::optional<std::set<CK_MECHANISM_TYPE>> readMechanisms(const Pkcs11Module& module, CK_SLOT_ID slot) {
    CK_ULONG count = 0;
    if (module.call(&CK_FUNCTION_LIST::C_GetMechanismList, slot, static_cast<CK_MECHANISM_TYPE*>(nullptr), &count) !=
            CKR_OK ||
        count == 0 || count > 4096)
        return std::nullopt;
    std::vector<CK_MECHANISM_TYPE> list(count);
    CK_ULONG got = count;
    if (module.call(&CK_FUNCTION_LIST::C_GetMechanismList, slot, list.data(), &got) != CKR_OK || got == 0 || got > count)
        return std::nullopt;
    return std::set<CK_MECHANISM_TYPE>(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(got));
}

// ---- identity id -----------------------------------------------------------

template <typename T>
bool parseNumber(const std::string& text, T& out) {
    unsigned long long value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size() || value > static_cast<unsigned long long>(~T{0})) return false;
    out = static_cast<T>(value);
    return true;
}

struct ParsedId {
    CK_SLOT_ID slotHint = 0;
    std::string serial; // sanitized
    bool noKeyId = false;
    Bytes keyId;
    std::size_t duplicate = 0; // 0: first certificate with this id
    std::size_t ordinal = 0;   // for noKeyId ids
};

std::optional<ParsedId> parseId(std::string_view id) {
    const auto first = id.find('/');
    if (first == std::string_view::npos) return std::nullopt;
    const auto second = id.find('/', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    ParsedId parsed;
    const std::string slotText(id.substr(0, first));
    if (slotText.empty() || slotText.size() > 18 || slotText.find_first_not_of("0123456789") != std::string::npos)
        return std::nullopt;
    if (!parseNumber(slotText, parsed.slotHint)) return std::nullopt;
    parsed.serial = std::string(id.substr(first + 1, second - first - 1));
    std::string_view tail = id.substr(second + 1);
    if (tail.empty()) return std::nullopt;
    if (tail.front() == '-') {
        parsed.noKeyId = true;
        return parsed;
    }
    if (const auto colon = tail.find(':'); colon != std::string_view::npos) {
        const std::string dup(tail.substr(colon + 1));
        if (dup.empty() || dup.size() > 6 || dup.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        if (!parseNumber(dup, parsed.duplicate)) return std::nullopt;
        tail = tail.substr(0, colon);
    }
    auto bytes = fromHex(tail);
    if (!bytes || bytes->empty()) return std::nullopt;
    parsed.keyId = std::move(*bytes);
    return parsed;
}

// ---- signing plan --------------------------------------------------------------

struct Plan {
    CK_MECHANISM_TYPE mechanism = 0;
    DigestAlgorithm digest = DigestAlgorithm::Sha256;
    bool hashLocally = false;        // hash the message in software before the token call
    std::optional<CK_MECHANISM_TYPE> tokenDigest; // hash on the token (GOST, configured mechanism)
    bool prependDigestInfo = false;  // RSA PKCS#1 over a bare digest
    bool pss = false;
    bool derEcdsa = false;           // convert raw r||s to ASN.1 ECDSA-Sig-Value
};

CK_MECHANISM_TYPE tokenDigestMechanism(DigestAlgorithm d) {
    switch (d) {
    case DigestAlgorithm::Sha256: return CKM_SHA256;
    case DigestAlgorithm::Sha384: return CKM_SHA384;
    default: return CKM_SHA512;
    }
}

CK_RSA_PKCS_MGF_TYPE mgfFor(DigestAlgorithm d) {
    switch (d) {
    case DigestAlgorithm::Sha256: return CKG_MGF1_SHA256;
    case DigestAlgorithm::Sha384: return CKG_MGF1_SHA384;
    default: return CKG_MGF1_SHA512;
    }
}

// DigestInfo prefixes of RFC 8017 section 9.2 notes (AlgorithmIdentifier with
// NULL parameters followed by the OCTET STRING header).
std::optional<Bytes> digestInfoPrefix(DigestAlgorithm d) {
    switch (d) {
    case DigestAlgorithm::Sha256:
        return Bytes{0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
    case DigestAlgorithm::Sha384:
        return Bytes{0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
    case DigestAlgorithm::Sha512:
        return Bytes{0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};
    default: return std::nullopt;
    }
}

// Raw r||s -> DER ECDSA-Sig-Value (SEQUENCE { INTEGER r, INTEGER s }), the
// encoding the Rivet CMS model and the OpenSSL provider use for ECDSA.
CryptoResult<Bytes> ecdsaRawToDer(ByteView raw) {
    if (raw.empty() || raw.size() % 2 != 0) return fail(CryptoErrorCode::Io, "token returned a malformed ECDSA signature");
    const std::size_t half = raw.size() / 2;
    return der::sequence({der::integer(raw.first(half)), der::integer(raw.subspan(half))});
}

class Pkcs11Identity final : public SigningIdentity {
public:
    struct TokenKey {
        std::string serialSanitized;
        CK_SLOT_ID slotHint = 0;
        std::string label;
    };

    Pkcs11Identity(std::shared_ptr<const Config> cfg, std::shared_ptr<Pkcs11Module> module, TokenKey token,
                   CertificateInfo certificate, Bytes keyId, std::vector<Bytes> chain, std::shared_ptr<PinPrompt> prompt,
                   std::optional<std::set<CK_MECHANISM_TYPE>> mechanisms)
        : cfg_(std::move(cfg)), module_(std::move(module)), token_(std::move(token)), cert_(std::move(certificate)),
          keyId_(std::move(keyId)), chain_(std::move(chain)), prompt_(std::move(prompt)), mechanisms_(std::move(mechanisms)) {}

    std::string_view providerName() const override { return "pkcs11"; }
    const CertificateInfo& certificate() const override { return cert_; }
    std::vector<Bytes> chainDer() const override { return chain_; }
    PublicKeyType keyType() const override { return cert_.publicKeyType; }
    int keyBits() const override { return cert_.keyBits; }

    std::vector<SignatureAlgorithm> supportedSignatures() const override {
        using S = SignatureAlgorithm;
        std::vector<S> candidates;
        switch (cert_.publicKeyType) {
        case PublicKeyType::Rsa:
            candidates = {S::RsaPkcs1Sha256, S::RsaPkcs1Sha384, S::RsaPkcs1Sha512,
                          S::RsaPssSha256,   S::RsaPssSha384,   S::RsaPssSha512};
            break;
        case PublicKeyType::Ec: candidates = {S::EcdsaSha256, S::EcdsaSha384, S::EcdsaSha512}; break;
        case PublicKeyType::Gost3410_2012_256: candidates = {S::Gost3410_2012_256}; break;
        case PublicKeyType::Gost3410_2012_512: candidates = {S::Gost3410_2012_512}; break;
        default: break;
        }
        std::vector<S> out;
        for (const S alg : candidates)
            if (checkCreationAllowed(alg, cert_.publicKeyType, cert_.keyBits) && selectPlan(alg, false, false)) out.push_back(alg);
        return out;
    }

    CryptoResult<Bytes> sign(SignatureAlgorithm algorithm, ByteView message, const CancelToken& cancel) override {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        if (auto ok = checkCreationAllowed(algorithm, cert_.publicKeyType, cert_.keyBits); !ok) return std::unexpected(ok.error());
        auto plan = selectPlan(algorithm, false, message.size() > kMaxTokenMessage);
        if (!plan) return std::unexpected(plan.error());
        Bytes input(message.begin(), message.end());
        if (plan->hashLocally) {
            auto digest = hashLocally(plan->digest, message);
            if (!digest) return std::unexpected(digest.error());
            input = std::move(*digest);
            if (plan->prependDigestInfo) { // CKM_RSA_PKCS signs a DigestInfo, not a bare digest
                auto prefix = digestInfoPrefix(plan->digest);
                if (!prefix) return fail(CryptoErrorCode::UnsupportedAlgorithm, "no DigestInfo for this digest");
                prefix->insert(prefix->end(), input.begin(), input.end());
                input = std::move(*prefix);
            }
        }
        return perform(algorithm, *plan, std::move(input), message, cancel);
    }

    CryptoResult<Bytes> signDigest(SignatureAlgorithm algorithm, ByteView digest, const CancelToken& cancel) override {
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
        if (auto ok = checkCreationAllowed(algorithm, cert_.publicKeyType, cert_.keyBits); !ok) return std::unexpected(ok.error());
        auto plan = selectPlan(algorithm, true, false);
        if (!plan) return std::unexpected(plan.error());
        if (digest.size() != digestSize(plan->digest))
            return fail(CryptoErrorCode::InvalidArgument, "digest length does not match the signature algorithm");
        Bytes input(digest.begin(), digest.end());
        if (plan->prependDigestInfo) {
            auto prefix = digestInfoPrefix(plan->digest);
            if (!prefix) return fail(CryptoErrorCode::UnsupportedAlgorithm, "no DigestInfo for this digest");
            prefix->insert(prefix->end(), digest.begin(), digest.end());
            input = std::move(*prefix);
        }
        return perform(algorithm, *plan, std::move(input), {}, cancel);
    }

private:
    bool mechanismListed(CK_MECHANISM_TYPE m) const { return !mechanisms_ || mechanisms_->contains(m); }
    // True only when the token reported its mechanism list and it contains `m`.
    bool mechanismKnownListed(CK_MECHANISM_TYPE m) const { return mechanisms_ && mechanisms_->contains(m); }

    CryptoResult<Plan> selectPlan(SignatureAlgorithm alg, bool digestInput, bool preferLocalHash) const {
        using S = SignatureAlgorithm;
        Plan plan;
        plan.digest = digestOf(alg);
        const DigestAlgorithm d = plan.digest;
        const bool canHash = cfg_->canHash(d);
        const auto unsupported = [] { return fail(CryptoErrorCode::UnsupportedAlgorithm, "token does not offer a mechanism for this algorithm"); };

        CK_MECHANISM_TYPE combined = 0;
        switch (alg) {
        case S::RsaPkcs1Sha256: combined = CKM_SHA256_RSA_PKCS; break;
        case S::RsaPkcs1Sha384: combined = CKM_SHA384_RSA_PKCS; break;
        case S::RsaPkcs1Sha512: combined = CKM_SHA512_RSA_PKCS; break;
        case S::RsaPssSha256: combined = CKM_SHA256_RSA_PKCS_PSS; break;
        case S::RsaPssSha384: combined = CKM_SHA384_RSA_PKCS_PSS; break;
        case S::RsaPssSha512: combined = CKM_SHA512_RSA_PKCS_PSS; break;
        case S::EcdsaSha256: combined = CKM_ECDSA_SHA256; break;
        case S::EcdsaSha384: combined = CKM_ECDSA_SHA384; break;
        case S::EcdsaSha512: combined = CKM_ECDSA_SHA512; break;
        default: break;
        }

        switch (alg) {
        case S::RsaPkcs1Sha256:
        case S::RsaPkcs1Sha384:
        case S::RsaPkcs1Sha512:
            if (digestInput) {
                if (!mechanismListed(CKM_RSA_PKCS)) return unsupported();
                plan.mechanism = CKM_RSA_PKCS;
                plan.prependDigestInfo = true;
                return plan;
            }
            if (mechanismListed(combined) && !(preferLocalHash && canHash && mechanismListed(CKM_RSA_PKCS))) {
                plan.mechanism = combined;
                return plan;
            }
            if (mechanismListed(CKM_RSA_PKCS) && canHash) {
                plan.mechanism = CKM_RSA_PKCS;
                plan.hashLocally = true;
                plan.prependDigestInfo = true;
                return plan;
            }
            return unsupported();
        case S::RsaPssSha256:
        case S::RsaPssSha384:
        case S::RsaPssSha512:
            plan.pss = true;
            if (digestInput) {
                if (!mechanismListed(CKM_RSA_PKCS_PSS)) return unsupported();
                plan.mechanism = CKM_RSA_PKCS_PSS;
                return plan;
            }
            if (mechanismListed(combined) && !(preferLocalHash && canHash && mechanismListed(CKM_RSA_PKCS_PSS))) {
                plan.mechanism = combined;
                return plan;
            }
            if (mechanismListed(CKM_RSA_PKCS_PSS) && canHash) {
                plan.mechanism = CKM_RSA_PKCS_PSS;
                plan.hashLocally = true;
                return plan;
            }
            return unsupported();
        case S::EcdsaSha256:
        case S::EcdsaSha384:
        case S::EcdsaSha512:
            plan.derEcdsa = true;
            if (digestInput) {
                if (!mechanismListed(CKM_ECDSA)) return unsupported();
                plan.mechanism = CKM_ECDSA;
                return plan;
            }
            // The combined mechanism is used only when the token lists it; otherwise the
            // digest is computed in software and CKM_ECDSA signs it.
            if (mechanismKnownListed(combined) && !(preferLocalHash && canHash && mechanismListed(CKM_ECDSA))) {
                plan.mechanism = combined;
                return plan;
            }
            if (mechanismListed(CKM_ECDSA) && canHash) {
                plan.mechanism = CKM_ECDSA;
                plan.hashLocally = true;
                return plan;
            }
            return unsupported();
        case S::Gost3410_2012_256:
        case S::Gost3410_2012_512: {
            const bool is256 = alg == S::Gost3410_2012_256;
            const auto& gost = cfg_->options.gost;
            const auto raw = is256 ? gost.sign256 : gost.sign512;
            const auto both = is256 ? gost.signWithDigest256 : gost.signWithDigest512;
            const auto onToken = is256 ? gost.digest256 : gost.digest512;
            if (digestInput) {
                if (!raw) return fail(CryptoErrorCode::UnsupportedAlgorithm, "no raw GOST signing mechanism is configured");
                plan.mechanism = *raw;
                return plan;
            }
            if (raw && canHash) {
                plan.mechanism = *raw;
                plan.hashLocally = true;
                return plan;
            }
            if (both) {
                plan.mechanism = *both;
                return plan;
            }
            if (raw && onToken) {
                plan.mechanism = *raw;
                plan.tokenDigest = *onToken;
                return plan;
            }
            return fail(CryptoErrorCode::UnsupportedAlgorithm, "no GOST mechanism is configured for this algorithm");
        }
        }
        return unsupported();
    }

    CryptoResult<Bytes> hashLocally(DigestAlgorithm d, ByteView data) const {
        if (!cfg_->hasher) return fail(CryptoErrorCode::UnsupportedAlgorithm, "no software digest provider");
        auto hasher = cfg_->hasher->createHasher(d);
        if (!hasher) return std::unexpected(hasher.error());
        (*hasher)->update(data);
        return (*hasher)->finish();
    }

    struct Attempt {
        CryptoResult<Bytes> result = fail(CryptoErrorCode::Internal, "no result");
        bool retry = false;
    };

    // At most two attempts: a lost session (token reset, session closed, user logged
    // out behind our back) is recovered once by opening a new session and logging in again.
    CryptoResult<Bytes> perform(SignatureAlgorithm algorithm, const Plan& plan, Bytes input, ByteView originalMessage,
                                const CancelToken& cancel) const {
        (void)algorithm;
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
            Attempt outcome = attemptSign(plan, input, originalMessage, attempt > 0, cancel);
            if (outcome.result) return std::move(outcome.result);
            if (!outcome.retry || attempt > 0) return std::move(outcome.result);
        }
        return fail(CryptoErrorCode::SessionExpired, "PKCS#11 session could not be re-established");
    }

    CryptoResult<CK_SLOT_ID> locateSlot(CK_TOKEN_INFO& tokenInfo, const CancelToken& cancel) const {
        auto tokens = presentTokens(*module_, cancel);
        if (!tokens) return std::unexpected(tokens.error());
        const TokenRef* chosen = nullptr;
        for (const TokenRef& t : *tokens) {
            if (sanitizeSerial(t.serial) != token_.serialSanitized) continue;
            if (!chosen || t.slot == token_.slotHint) chosen = &t;
        }
        if (!chosen) return fail(CryptoErrorCode::DeviceRemoved, "the token is not present");
        tokenInfo = chosen->info;
        return chosen->slot;
    }

    Attempt attemptSign(const Plan& plan, const Bytes& inputIn, ByteView originalMessage, bool forceLogin,
                        const CancelToken& cancel) const {
        Attempt attempt;
        const auto failWith = [&](CryptoError error) {
            attempt.retry = error.code == CryptoErrorCode::SessionExpired;
            attempt.result = std::unexpected(std::move(error));
            return attempt;
        };
        CK_TOKEN_INFO info{};
        auto slot = locateSlot(info, cancel);
        if (!slot) return failWith(slot.error());
        if (cancel.cancelled()) return failWith(CryptoError{CryptoErrorCode::Cancelled, "operation cancelled"});

        Pkcs11Session session;
        if (const CK_RV rv = Pkcs11Session::open(module_, *slot, session); rv != CKR_OK)
            return failWith(mapRv(rv, "cannot open a session"));

        CK_SESSION_INFO sessionInfo{};
        bool loggedIn = false;
        if (module_->call(&CK_FUNCTION_LIST::C_GetSessionInfo, session.handle(), &sessionInfo) == CKR_OK)
            loggedIn = sessionInfo.state == CKS_RO_USER_FUNCTIONS || sessionInfo.state == CKS_RW_USER_FUNCTIONS;
        if (!loggedIn && ((info.flags & CKF_LOGIN_REQUIRED) != 0 || forceLogin)) {
            if (auto ok = login(session, *slot, cancel); !ok) return failWith(ok.error());
        }
        if (cancel.cancelled()) return failWith(CryptoError{CryptoErrorCode::Cancelled, "operation cancelled"});

        CK_OBJECT_HANDLE key = 0;
        if (const CK_RV rv = findPrivateKey(session, keyId_, cert_.publicKeyType, key); rv != CKR_OK) {
            if (rv == CKR_KEY_HANDLE_INVALID) return failWith(CryptoError{CryptoErrorCode::NoPrivateKey, "the private key was not found on the token"});
            return failWith(mapRv(rv, "cannot look up the private key"));
        }

        Bytes input = inputIn;
        if (plan.tokenDigest) {
            CK_MECHANISM digestMechanism{*plan.tokenDigest, nullptr, 0};
            CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_DigestInit, session.handle(), &digestMechanism);
            if (rv != CKR_OK) return failWith(mapRv(rv, "token digest"));
            Bytes digest(64);
            CK_ULONG digestLength = digest.size();
            Bytes message(originalMessage.begin(), originalMessage.end());
            rv = module_->call(&CK_FUNCTION_LIST::C_Digest, session.handle(), message.data(), static_cast<CK_ULONG>(message.size()),
                               digest.data(), &digestLength);
            if (rv != CKR_OK || digestLength == 0 || digestLength > digest.size()) return failWith(mapRv(rv == CKR_OK ? CKR_DEVICE_ERROR : rv, "token digest"));
            digest.resize(digestLength);
            input = std::move(digest);
        }
        if (cancel.cancelled()) return failWith(CryptoError{CryptoErrorCode::Cancelled, "operation cancelled"});

        CK_RSA_PKCS_PSS_PARAMS pssParams{};
        CK_MECHANISM mechanism{plan.mechanism, nullptr, 0};
        if (plan.pss) {
            pssParams.hashAlg = tokenDigestMechanism(plan.digest);
            pssParams.mgf = mgfFor(plan.digest);
            pssParams.sLen = static_cast<CK_ULONG>(digestSize(plan.digest)); // salt = digest length
            mechanism.pParameter = &pssParams;
            mechanism.ulParameterLen = sizeof pssParams;
        }
        CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_SignInit, session.handle(), &mechanism, key);
        if (rv != CKR_OK) return failWith(mapRv(rv, "C_SignInit"));

        Bytes signature(1024);
        CK_ULONG signatureLength = signature.size();
        rv = module_->call(&CK_FUNCTION_LIST::C_Sign, session.handle(), input.data(), static_cast<CK_ULONG>(input.size()),
                           signature.data(), &signatureLength);
        if (rv == CKR_BUFFER_TOO_SMALL && signatureLength > signature.size() && signatureLength <= (1U << 16)) {
            signature.assign(signatureLength, 0);
            rv = module_->call(&CK_FUNCTION_LIST::C_Sign, session.handle(), input.data(), static_cast<CK_ULONG>(input.size()),
                               signature.data(), &signatureLength);
        }
        if (rv != CKR_OK) return failWith(mapRv(rv, "C_Sign"));
        if (signatureLength == 0 || signatureLength > signature.size())
            return failWith(CryptoError{CryptoErrorCode::Io, "token returned an invalid signature length"});
        signature.resize(signatureLength);
        if (plan.derEcdsa) {
            attempt.result = ecdsaRawToDer(signature);
        } else {
            attempt.result = std::move(signature);
        }
        return attempt;
    }

    // Logs in as CKU_USER. The PinPrompt is called with no lock held (the module gate
    // is taken only inside each individual PKCS#11 call).
    CryptoOk login(Pkcs11Session& session, CK_SLOT_ID slot, const CancelToken& cancel) const {
        CK_TOKEN_INFO info{};
        if (module_->call(&CK_FUNCTION_LIST::C_GetTokenInfo, slot, &info) != CKR_OK) return fail(CryptoErrorCode::DeviceRemoved, "the token is not present");
        if ((info.flags & CKF_USER_PIN_LOCKED) != 0) return fail(CryptoErrorCode::PinLocked, "the token PIN is locked");
        if ((info.flags & CKF_PROTECTED_AUTHENTICATION_PATH) != 0) {
            // PIN pad / biometric reader on the token: the module collects the PIN itself.
            const CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_Login, session.handle(), CKU_USER, static_cast<CK_UTF8CHAR*>(nullptr), CK_ULONG{0});
            return interpretLogin(rv, false);
        }
        for (int attempt = 1; attempt <= kMaxPinAttempts; ++attempt) {
            if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
            if (!prompt_) return fail(CryptoErrorCode::Cancelled, "a PIN is required but no prompt is available");
            PinRequest request;
            request.providerName = "pkcs11";
            request.label = token_.label;
            request.attempt = attempt;
            const bool finalTry = (info.flags & CKF_USER_PIN_FINAL_TRY) != 0;
            request.reason = finalTry ? PinPromptReason::LastAttempt : attempt == 1 ? PinPromptReason::Required : PinPromptReason::Incorrect;
            if (finalTry) request.retriesLeft = 1;
            std::optional<SecretBuffer> pin = prompt_->requestPin(request, cancel); // no lock held
            if (!pin) return fail(CryptoErrorCode::Cancelled, "PIN entry cancelled");
            if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
            if (pin->empty()) continue; // not sent to the token: it would count as a failed try
            const CK_RV rv = module_->call(&CK_FUNCTION_LIST::C_Login, session.handle(), CKU_USER,
                                           const_cast<CK_UTF8CHAR*>(pin->bytes().data()), static_cast<CK_ULONG>(pin->size()));
            pin->clear();
            if (rv == CKR_PIN_INCORRECT || rv == CKR_PIN_INVALID || rv == CKR_PIN_LEN_RANGE) {
                (void)module_->call(&CK_FUNCTION_LIST::C_GetTokenInfo, slot, &info); // refresh the retry-counter flags
                if ((info.flags & CKF_USER_PIN_LOCKED) != 0) return fail(CryptoErrorCode::PinLocked, "the token PIN is now locked");
                continue;
            }
            return interpretLogin(rv, true);
        }
        return fail(CryptoErrorCode::WrongPassword, "the PIN was rejected");
    }

    static CryptoOk interpretLogin(CK_RV rv, bool) {
        switch (rv) {
        case CKR_OK:
        case CKR_USER_ALREADY_LOGGED_IN: return {};
        case CKR_PIN_LOCKED: return fail(CryptoErrorCode::PinLocked, "the token PIN is locked");
        case CKR_PIN_EXPIRED: return fail(CryptoErrorCode::WrongPassword, "the token PIN has expired; change it with the token vendor's tool");
        case CKR_PIN_INCORRECT: return fail(CryptoErrorCode::WrongPassword, "the PIN was rejected");
        default: return std::unexpected(mapRv(rv, "C_Login"));
        }
    }

    std::shared_ptr<const Config> cfg_;
    std::shared_ptr<Pkcs11Module> module_;
    TokenKey token_;
    CertificateInfo cert_;
    Bytes keyId_;
    std::vector<Bytes> chain_;
    std::shared_ptr<PinPrompt> prompt_;
    std::optional<std::set<CK_MECHANISM_TYPE>> mechanisms_;
};

std::vector<Bytes> buildChain(const std::vector<TokenCertificate>& certs, const TokenCertificate& leaf) {
    std::vector<Bytes> chain{leaf.info.der};
    const CertificateInfo* current = &leaf.info;
    while (chain.size() < kMaxChain && !current->isSelfIssued()) {
        const CertificateInfo* best = nullptr;
        for (const TokenCertificate& candidate : certs) {
            const CertificateInfo& c = candidate.info;
            if (!c.isCa || c.subjectDer != current->issuerDer) continue;
            if (std::find(chain.begin(), chain.end(), c.der) != chain.end()) continue;
            if (!best || (!current->authorityKeyId.empty() && c.subjectKeyId == current->authorityKeyId)) best = &c;
        }
        if (!best) break;
        chain.push_back(best->der);
        current = best;
    }
    return chain;
}

class Pkcs11Store final : public IdentityStore {
public:
    explicit Pkcs11Store(Pkcs11Options options) {
        auto cfg = std::make_shared<Config>();
        cfg->options = std::move(options);
        cfg->hasher = cfg->options.hashProvider ? cfg->options.hashProvider : makeDefaultProvider();
        if (cfg->hasher && cfg->hasher->isAvailable()) {
            auto hasher = cfg->hasher;
            cfg->digestFn = [hasher](DigestAlgorithm a, ByteView data) { return hasher->digest(a, data); };
        }
        cfg_ = std::move(cfg);
    }

    std::string_view name() const override { return "pkcs11"; }
    bool isAvailable() const override { return module().has_value(); }
    std::string unavailableReason() const override {
        auto m = module();
        return m ? std::string() : m.error().message;
    }
    ProviderAssurance assurance() const override { return ProviderAssurance::HardwareToken; }

    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken& cancel) override {
        auto mod = module();
        if (!mod) return std::unexpected(mod.error());
        auto tokens = presentTokens(**mod, cancel);
        if (!tokens) return std::unexpected(tokens.error());
        std::vector<IdentityDescriptor> out;
        const auto now = std::chrono::system_clock::now();
        for (const TokenRef& token : *tokens) {
            if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
            Pkcs11Session session;
            if (Pkcs11Session::open(*mod, token.slot, session) != CKR_OK) continue; // removed or busy: skip this token
            auto certs = readCertificates(*cfg_, session, cancel);
            if (!certs) {
                if (certs.error().code == CryptoErrorCode::Cancelled) return std::unexpected(certs.error());
                continue;
            }
            const std::vector<Bytes> privateIds = keyIds(session, CKO_PRIVATE_KEY);
            // Private keys are usually hidden (CKA_PRIVATE) until login; a public key
            // object with the same CKA_ID is then the evidence that the pair exists.
            const bool loginRequired = (token.info.flags & CKF_LOGIN_REQUIRED) != 0;
            const std::vector<Bytes> publicIds = loginRequired ? keyIds(session, CKO_PUBLIC_KEY) : std::vector<Bytes>{};
            const std::string serial = sanitizeSerial(token.serial);

            std::vector<std::pair<Bytes, std::size_t>> seen; // CKA_ID -> count
            std::size_t ordinal = 0;
            for (const TokenCertificate& cert : *certs) {
                ++ordinal;
                const bool paired = contains(privateIds, cert.id) || (loginRequired && contains(publicIds, cert.id));
                if (cert.info.isCa && !paired) continue; // chain material, not an identity
                IdentityDescriptor d;
                d.providerName = "pkcs11";
                d.certificate = cert.info;
                d.privateKeyAvailable = paired;
                d.certificateExpired = now > cert.info.notAfter;
                d.location.kind = KeyLocationKind::Token;
                d.location.label = token.label;
                d.location.device = cfg_->display() + " slot " + std::to_string(token.slot);
                if (!token.slotDescription.empty()) d.location.device += " (" + token.slotDescription + ")";
                std::string tail;
                if (cert.id.empty()) {
                    tail = "-" + std::to_string(ordinal);
                } else {
                    tail = hexOf(cert.id);
                    auto it = std::find_if(seen.begin(), seen.end(), [&](const auto& s) { return s.first == cert.id; });
                    if (it == seen.end()) {
                        seen.emplace_back(cert.id, 0);
                    } else {
                        tail += ":" + std::to_string(++it->second);
                    }
                }
                d.id = std::to_string(token.slot) + "/" + serial + "/" + tail;
                out.push_back(std::move(d));
            }
        }
        return out;
    }

    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view id, std::shared_ptr<PinPrompt> prompt,
                                                        const CancelToken& cancel) override {
        auto mod = module();
        if (!mod) return std::unexpected(mod.error());
        const auto parsed = parseId(id);
        if (!parsed) return fail(CryptoErrorCode::InvalidArgument, "malformed PKCS#11 identity id");
        if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");

        auto tokens = presentTokens(**mod, cancel);
        if (!tokens) return std::unexpected(tokens.error());
        const TokenRef* chosen = nullptr;
        for (const TokenRef& t : *tokens) {
            if (sanitizeSerial(t.serial) != parsed->serial) continue;
            if (!chosen || t.slot == parsed->slotHint) chosen = &t;
        }
        if (!chosen) return fail(CryptoErrorCode::DeviceRemoved, "the token is not present");
        if (parsed->noKeyId) return fail(CryptoErrorCode::NoPrivateKey, "the certificate has no CKA_ID, so no private key can be paired with it");

        Pkcs11Session session;
        if (const CK_RV rv = Pkcs11Session::open(*mod, chosen->slot, session); rv != CKR_OK)
            return std::unexpected(mapRv(rv, "cannot open a session"));
        auto certs = readCertificates(*cfg_, session, cancel);
        if (!certs) return std::unexpected(certs.error());

        const TokenCertificate* leaf = nullptr;
        std::size_t skipped = 0;
        for (const TokenCertificate& c : *certs) {
            if (c.id != parsed->keyId) continue;
            if (skipped++ == parsed->duplicate) {
                leaf = &c;
                break;
            }
        }
        if (!leaf) return fail(CryptoErrorCode::CertificateNotFound, "the certificate is no longer on the token");

        const bool loginRequired = (chosen->info.flags & CKF_LOGIN_REQUIRED) != 0;
        CK_OBJECT_HANDLE key = 0;
        const CK_RV keyRv = findPrivateKey(session, leaf->id, leaf->info.publicKeyType, key);
        if (keyRv != CKR_OK && keyRv != CKR_KEY_HANDLE_INVALID) return std::unexpected(mapRv(keyRv, "cannot look up the private key"));
        if (keyRv == CKR_KEY_HANDLE_INVALID) {
            // Hidden until login on most tokens: accept it then only on the public-key evidence.
            const bool evidence = loginRequired && contains(keyIds(session, CKO_PUBLIC_KEY), leaf->id);
            if (!evidence) return fail(CryptoErrorCode::NoPrivateKey, "no private key matches this certificate on the token");
        }

        Pkcs11Identity::TokenKey tokenKey{parsed->serial, chosen->slot, chosen->label};
        auto identity = std::make_unique<Pkcs11Identity>(cfg_, *mod, std::move(tokenKey), leaf->info, leaf->id,
                                                         buildChain(*certs, *leaf), std::move(prompt),
                                                         readMechanisms(**mod, chosen->slot));
        return std::unique_ptr<SigningIdentity>(std::move(identity));
    }

private:
    CryptoResult<std::shared_ptr<Pkcs11Module>> module() const {
        std::lock_guard lock(mutex_);
        if (module_) return module_;
        auto loaded = Pkcs11Module::acquire(cfg_->options.modulePath, cfg_->options.allowInsecureModulePath);
        if (!loaded) return std::unexpected(loaded.error());
        module_ = *loaded;
        return module_;
    }

    std::shared_ptr<const Config> cfg_;
    mutable std::mutex mutex_;
    mutable std::shared_ptr<Pkcs11Module> module_;
};

} // namespace

std::shared_ptr<IdentityStore> makePkcs11IdentityStore(Pkcs11Options options) {
    return std::make_shared<Pkcs11Store>(std::move(options));
}

#else // !RIVET_CRYPTO_PKCS11

namespace {

class UnavailablePkcs11Store final : public IdentityStore {
public:
    std::string_view name() const override { return "pkcs11"; }
    bool isAvailable() const override { return false; }
    std::string unavailableReason() const override { return "built without PKCS#11 support (RIVET_ENABLE_PKCS11=OFF)"; }
    ProviderAssurance assurance() const override { return ProviderAssurance::HardwareToken; }
    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken&) override { return unavailable(); }
    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view, std::shared_ptr<PinPrompt>, const CancelToken&) override {
        return unavailable();
    }

private:
    static CryptoError error() { return CryptoError{CryptoErrorCode::BackendUnavailable, "PKCS#11 support is not built in"}; }
    static std::unexpected<CryptoError> unavailable() { return std::unexpected<CryptoError>(error()); }
};

} // namespace

std::shared_ptr<IdentityStore> makePkcs11IdentityStore(Pkcs11Options) { return std::make_shared<UnavailablePkcs11Store>(); }

#endif

} // namespace rivet::crypto
