// SPDX-License-Identifier: MPL-2.0
// CryptoPro CSP provider tests.
//
// Always run: the unavailable-backend behaviour. Run only when the CSP is
// installed and the build has RIVET_ENABLE_CRYPTOPRO: Streebog vectors
// (RFC 6986), GOST R 34.10-2012 verification of the RFC 7091 vector through the
// real CSP, store enumeration. Signing needs a key container, which cannot be
// created non-interactively (ADR-0040): those tests run only when
// RIVET_TEST_CRYPTOPRO_PIN is set (the PIN of a throwaway container the
// operator created in the CSP user store) and print SKIP otherwise.

#include "RivetTest.h"

#include "crypto/Cms.hpp"
#include "crypto/Der.hpp"
#include "crypto/X509Parse.hpp"
#include "crypto/providers/cryptopro/CryptoProIdentityStore.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace rivet::crypto;

namespace {

Bytes fromHex(std::string_view hex) {
    Bytes out;
    auto val = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(val(hex[i]) * 16 + val(hex[i + 1])));
    return out;
}

std::string toHex(ByteView b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto c : b) {
        s.push_back(d[c >> 4]);
        s.push_back(d[c & 15]);
    }
    return s;
}

Bytes reversedCopy(ByteView b) { return Bytes(b.rbegin(), b.rend()); }

std::shared_ptr<CryptoProvider> cspProvider() {
    static auto provider = makeCryptoProProvider();
    return provider;
}

#define REQUIRE_CSP()                                                                         \
    auto provider = cspProvider();                                                            \
    if (!provider->isAvailable()) {                                                           \
        std::puts("SKIP: CryptoPro CSP provider not built or CSP not installed");             \
        return;                                                                               \
    }

std::string digestHex(CryptoProvider& p, DigestAlgorithm a, ByteView data) {
    auto d = p.digest(a, data);
    CHECK(d.has_value());
    return d ? toHex(*d) : std::string{};
}

// RFC 6986 appendix A message M1 and M2 (M2 is CP1251 text).
const std::string kM1 = "012345678901234567890123456789012345678901234567890123456789012";
const Bytes kM2 = fromHex(
    "d1e520e2e5f2f0e82c20d1f2f0e8e1ee"
    "e6e820e2edf3f6e82c20e2e5fef2fa20"
    "f120eceef0ff20f1f2f0e5ebe0ece820"
    "ede020f5f0e0e1f0fbff20efebfaeafb"
    "20c8e3eef0e5e2fb");

// A minimal syntactically valid certificate carrying a GOST R 34.10-2012 public key
// (the signature is dummy: only the SubjectPublicKeyInfo is used).
Bytes makeGostCert(std::string_view curveOid, std::string_view digestOid, std::string_view keyAlgOid, ByteView publicKey) {
    namespace d = rivet::crypto::der;
    auto name = d::sequence({d::setOf({d::sequence({d::oidElement("2.5.4.3"), d::tlv(d::kUtf8String, viewOf("RFC7091"))})})});
    auto sigAlg = d::sequence({d::oidElement(oid::kGost3410_256)});
    const auto now = std::chrono::system_clock::now();
    auto validity = d::sequence({d::timeElement(now - std::chrono::hours(24)), d::timeElement(now + std::chrono::hours(24 * 365))});
    auto spki = d::sequence({d::sequence({d::oidElement(keyAlgOid), d::sequence({d::oidElement(curveOid), d::oidElement(digestOid)})}),
                             d::tlv(d::kBitString, d::concat({Bytes{0}, d::octetString(publicKey)}))});
    auto tbs = d::sequence({d::tlv(d::contextTag(0, true), d::integerFromInt(2)), d::integerFromInt(1), sigAlg, name, validity, name, spki});
    return d::sequence({tbs, sigAlg, d::tlv(d::kBitString, d::concat({Bytes{0}, Bytes(64, 0)}))});
}

} // namespace

// ---- always run -------------------------------------------------------------

RIVET_TEST(cryptoProUnavailableBackendIsGraceful) {
    CryptoProOptions options;
    options.libraryDirectory = "/nonexistent/cryptopro/lib";
    auto store = makeCryptoProIdentityStore(options);
    CHECK(store != nullptr);
    CHECK(!store->isAvailable());
    CHECK(!store->unavailableReason().empty());
    CHECK(store->assurance() == ProviderAssurance::CertifiedCspDeclared);
    auto listed = store->list(CancelToken{});
    CHECK(!listed.has_value());
    if (!listed) CHECK(listed.error().code == CryptoErrorCode::BackendUnavailable);
    auto opened = store->open("cryptopro:nope", nullptr, CancelToken{});
    CHECK(!opened.has_value());
    if (!opened) CHECK(opened.error().code == CryptoErrorCode::BackendUnavailable);
    auto provider = makeCryptoProProvider(options);
    CHECK(provider != nullptr);
    CHECK(!provider->isAvailable());
}

// ---- real CSP ---------------------------------------------------------------

RIVET_TEST(cryptoProStreebogEmptyMessage) {
    REQUIRE_CSP();
    CHECK(provider->supports(DigestAlgorithm::Gost3411_2012_256));
    CHECK(provider->supports(DigestAlgorithm::Gost3411_2012_512));
    CHECK_EQ(digestHex(*provider, DigestAlgorithm::Gost3411_2012_256, {}),
             std::string("3f539a213e97c802cc229d474c6aa32a825a360b2a933a949fd925208d9ce1bb"));
    CHECK_EQ(digestHex(*provider, DigestAlgorithm::Gost3411_2012_512, {}),
             std::string("8e945da209aa869f0455928529bcae4679e9873ab707b55315f56ceb98bef0a7"
                         "362f715528356ee83cda5f2aac4c6ad2ba3a715c1bcd81cb8e9f90bf4c1c1a8a"));
}

RIVET_TEST(cryptoProStreebogRfc6986Vectors) {
    REQUIRE_CSP();
    // Output byte order of the hash functions, as in RFC 6986 test vectors read as byte strings.
    CHECK_EQ(digestHex(*provider, DigestAlgorithm::Gost3411_2012_256, viewOf(kM1)),
             std::string("9d151eefd8590b89daa6ba6cb74af9275dd051026bb149a452fd84e5e57b5500"));
    CHECK_EQ(digestHex(*provider, DigestAlgorithm::Gost3411_2012_512, viewOf(kM1)),
             std::string("1b54d01a4af5b9d5cc3d86d68d285462b19abc2475222f35c085122be4ba1ffa"
                         "00ad30f8767b3a82384c6574f024c311e2a481332b08ef7f41797891c1646f48"));
    CHECK_EQ(kM2.size(), std::size_t{72});
    CHECK_EQ(digestHex(*provider, DigestAlgorithm::Gost3411_2012_256, kM2),
             std::string("9dd2fe4e90409e5da87f53976d7405b0c0cac628fc669a741d50063c557e8f50"));
    CHECK_EQ(digestHex(*provider, DigestAlgorithm::Gost3411_2012_512, kM2),
             std::string("1e88e62226bfca6f9994f1f2d51569e0daf8475a3b0fe61a5300eee46d961376"
                         "035fe83549ada2b8620fcd7c496ce5b33f0cb9dddc2b6460143b03dabac9fb28"));
}

RIVET_TEST(cryptoProStreamingHasherMatchesOneShot) {
    REQUIRE_CSP();
    Bytes big(100000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i * 31 + 7);
    for (auto alg : {DigestAlgorithm::Gost3411_2012_256, DigestAlgorithm::Gost3411_2012_512}) {
        auto oneShot = provider->digest(alg, big);
        auto hasher = provider->createHasher(alg);
        CHECK(oneShot.has_value());
        CHECK(hasher.has_value());
        if (!oneShot || !hasher) return;
        for (std::size_t off = 0; off < big.size(); off += 4099)
            (*hasher)->update(ByteView(big).subspan(off, std::min<std::size_t>(4099, big.size() - off)));
        CHECK(*oneShot == (*hasher)->finish());
    }
    auto unsupported = provider->createHasher(DigestAlgorithm::Sha512);
    CHECK(!unsupported.has_value());
}

RIVET_TEST(cryptoProRfc7091Gost3410_256Verify) {
    REQUIRE_CSP();
    auto* verifier = dynamic_cast<GostDigestVerifier*>(provider.get());
    CHECK(verifier != nullptr);
    if (!verifier) return;
    // RFC 7091 appendix A.1 (id-GostR3410-2001-TestParamSet curve): public key Q, digest e, signature (r, s).
    const Bytes qx = fromHex("7f2b49e270db6d90d8595bec458b50c58585ba1d4e9b788f6689dbd8e56fd80b");
    const Bytes qy = fromHex("26f1b489d6701dd185c8413a977b3cbbaf64d1c593d26627dffb101a87ff77da");
    const Bytes e = fromHex("2dfbc1b372d89a1188c09c52e0eec61fce52032ab1022e8e67ece6672b043ee5");
    const Bytes r = fromHex("41aa28d2f1ab148280cd9ed56feda41974053554a42767b83ad043fd39dc0493");
    const Bytes s = fromHex("01456c64ba4642a1653c235a98a60249bcd6d3f746b631df928014f6c5bf9c40");

    // SubjectPublicKeyInfo key value: x || y, little-endian (CSP / RFC 4491 key blob order).
    Bytes key = reversedCopy(qx);
    const Bytes yle = reversedCopy(qy);
    key.insert(key.end(), yle.begin(), yle.end());
    const Bytes certDer = makeGostCert("1.2.643.2.2.35.0", oid::kGost3411_256, oid::kGost3410_256Key, key);
    auto cert = provider->parseCertificate(certDer);
    CHECK(cert.has_value());
    if (!cert) return;
    CHECK(cert->publicKeyType == PublicKeyType::Gost3410_2012_256);

    // The standard's e is a number; the digest byte string of a Streebog output is its little-endian form.
    const Bytes digest = reversedCopy(e);
    // Candidate wire encodings of (r, s); exactly one is accepted by the CSP after the provider's byte reversal.
    Bytes rs = r;
    rs.insert(rs.end(), s.begin(), s.end());
    Bytes sr = s;
    sr.insert(sr.end(), r.begin(), r.end());
    const Bytes candidates[4] = {rs, sr, reversedCopy(rs), reversedCopy(sr)};
    int accepted = -1;
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        if (verifier->verifyDigest(*cert, digest, candidates[i]).has_value()) {
            accepted = i;
            ++count;
        }
    }
    std::printf("RFC7091 A.1 verify: accepted encoding index %d (0=r||s BE, 1=s||r BE, 2=r||s LE reversed, 3=s||r reversed), matches=%d\n",
                accepted, count);
    CHECK_EQ(count, 1);
    // Measured against the real CSP: the wire value is s||r with both numbers big-endian (RFC 4491 layout),
    // i.e. the byte reversal of the CSP's native little-endian r||s.
    CHECK_EQ(accepted, 1);

    // Corrupted signature / digest are rejected.
    Bytes bad = candidates[accepted < 0 ? 0 : accepted];
    bad[10] ^= 1;
    CHECK(!verifier->verifyDigest(*cert, digest, bad).has_value());
    Bytes badDigest = digest;
    badDigest[0] ^= 1;
    CHECK(!verifier->verifyDigest(*cert, badDigest, candidates[accepted < 0 ? 0 : accepted]).has_value());
}

RIVET_TEST(cryptoProStoreEnumeratesAndRejectsUnknownIdentity) {
    auto store = makeCryptoProIdentityStore();
    if (!store->isAvailable()) {
        std::puts("SKIP: CryptoPro CSP provider not built or CSP not installed");
        return;
    }
    CHECK(store->assurance() == ProviderAssurance::CertifiedCspDeclared);
    auto listed = store->list(CancelToken{});
    CHECK(listed.has_value());
    auto opened = store->open("cryptopro:0000", nullptr, CancelToken{});
    CHECK(!opened.has_value());
    if (!opened) CHECK(opened.error().code == CryptoErrorCode::CertificateNotFound);
    CancelToken cancelled;
    cancelled.cancel();
    auto none = store->list(cancelled);
    // An empty store has nothing to cancel; with entries the call reports Cancelled.
    if (listed && !listed->empty()) CHECK(!none.has_value());
}

// ---- signing: needs a throwaway container made by the operator ------------------

namespace {
class EnvPinPrompt final : public PinPrompt {
public:
    std::optional<SecretBuffer> requestPin(const PinRequest&, const CancelToken&) override {
        ++calls;
        const char* pin = std::getenv("RIVET_TEST_CRYPTOPRO_PIN");
        if (!pin) return std::nullopt;
        return SecretBuffer(std::string_view(pin));
    }
    int calls = 0;
};
} // namespace

RIVET_TEST(cryptoProSignAndVerifyWithOperatorContainer) {
    auto store = makeCryptoProIdentityStore();
    if (!store->isAvailable() || !std::getenv("RIVET_TEST_CRYPTOPRO_PIN")) {
        std::puts("SKIP: set RIVET_TEST_CRYPTOPRO_PIN and install a throwaway GOST container+certificate (ADR-0040)");
        return;
    }
    auto listed = store->list(CancelToken{});
    CHECK(listed.has_value());
    if (!listed || listed->empty()) {
        std::puts("SKIP: no CryptoPro identity with a certificate in the MY store");
        return;
    }
    const IdentityDescriptor* usable = nullptr;
    for (const auto& d : *listed)
        if (d.privateKeyAvailable && !d.certificateExpired) usable = &d;
    CHECK(usable != nullptr);
    if (!usable) return;
    auto prompt = std::make_shared<EnvPinPrompt>();
    auto identity = store->open(usable->id, prompt, CancelToken{});
    CHECK(identity.has_value());
    if (!identity) return;
    auto provider = cspProvider();
    const auto algs = (*identity)->supportedSignatures();
    CHECK(!algs.empty());
    if (algs.empty()) return;
    const Bytes message = fromHex("00112233445566778899aabbccddeeff");
    auto sig = (*identity)->sign(algs[0], message, CancelToken{});
    CHECK(sig.has_value());
    if (!sig) return;
    CHECK(provider->verifySignature((*identity)->certificate(), schemeOf(algs[0]), message, *sig).has_value());
    Bytes tampered = message;
    tampered[0] ^= 1;
    CHECK(!provider->verifySignature((*identity)->certificate(), schemeOf(algs[0]), tampered, *sig).has_value());
    // Wrong algorithm for the key is refused.
    CHECK(!(*identity)->sign(SignatureAlgorithm::EcdsaSha256, message, CancelToken{}).has_value());
    // Cancelled before signing.
    CancelToken cancelled;
    cancelled.cancel();
    auto c = (*identity)->sign(algs[0], message, cancelled);
    CHECK(!c.has_value());
    if (!c) CHECK(c.error().code == CryptoErrorCode::Cancelled);
}
