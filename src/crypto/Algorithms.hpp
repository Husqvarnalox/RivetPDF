// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crypto/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rivet::crypto {

// Typed algorithm identifiers. No algorithm is ever selected by string.

enum class DigestAlgorithm : std::uint8_t {
    Sha256,
    Sha384,
    Sha512,
    Gost3411_2012_256,
    Gost3411_2012_512,
    Sha1, // legacy: verification only, never used to create signatures
};

enum class SignatureAlgorithm : std::uint8_t {
    RsaPkcs1Sha256,
    RsaPkcs1Sha384,
    RsaPkcs1Sha512,
    RsaPssSha256,
    RsaPssSha384,
    RsaPssSha512,
    EcdsaSha256,
    EcdsaSha384,
    EcdsaSha512,
    Gost3410_2012_256,
    Gost3410_2012_512,
};

// Model only: no symmetric encryption is implemented in this phase.
enum class SymmetricAlgorithm : std::uint8_t { Aes128, Aes256, Kuznyechik, Magma };

enum class PublicKeyType : std::uint8_t { Unknown, Rsa, Ec, Gost3410_2012_256, Gost3410_2012_512 };

// Key generation choices for new (software) identities.
enum class KeyAlgorithm : std::uint8_t { Rsa2048, Rsa3072, Rsa4096, EcdsaP256, EcdsaP384, EcdsaP521 };

enum class SignaturePadding : std::uint8_t { Pkcs1, Pss, Ecdsa, Gost };

// The verification-side description of a signature: it may use a legacy
// digest (SHA-1), which SignatureAlgorithm deliberately cannot express.
struct SignatureScheme {
    PublicKeyType keyType = PublicKeyType::Unknown;
    SignaturePadding padding = SignaturePadding::Pkcs1;
    DigestAlgorithm digest = DigestAlgorithm::Sha256;
    int pssSaltLength = -1; // -1: digest length (the only length Rivet creates)

    bool operator==(const SignatureScheme&) const = default;
};

std::string_view toString(DigestAlgorithm);
std::string_view toString(SignatureAlgorithm);
std::string_view toString(SymmetricAlgorithm);
std::string_view toString(PublicKeyType);
std::string_view toString(KeyAlgorithm);

std::size_t digestSize(DigestAlgorithm);
bool isLegacy(DigestAlgorithm);
bool isGost(DigestAlgorithm);
bool isGost(SignatureAlgorithm);

DigestAlgorithm digestOf(SignatureAlgorithm);
PublicKeyType keyTypeOf(SignatureAlgorithm);
SignatureScheme schemeOf(SignatureAlgorithm);
// nullopt when the scheme is not creatable (legacy digest, unknown key type).
std::optional<SignatureAlgorithm> signatureAlgorithmOf(const SignatureScheme&);
// Default signature algorithm for a key type and digest (RSA PKCS#1, ECDSA).
std::optional<SignatureAlgorithm> defaultSignatureFor(PublicKeyType, DigestAlgorithm);

std::optional<int> rsaBitsOf(KeyAlgorithm);

// ---- Policy -------------------------------------------------------------

// Creation policy: refuses SHA-1/legacy digests and RSA keys below 2048 bits.
CryptoOk checkCreationAllowed(SignatureAlgorithm, PublicKeyType keyType, int keyBits);
CryptoOk checkCreationAllowed(DigestAlgorithm);

// Verification flags a signature as "weak legacy algorithm" (but still
// verifies it) for legacy digests or RSA keys below 2048 bits.
bool isWeakForVerification(DigestAlgorithm, PublicKeyType keyType, int keyBits);

// ---- OIDs ---------------------------------------------------------------

namespace oid {
inline constexpr std::string_view kSha1 = "1.3.14.3.2.26";
inline constexpr std::string_view kSha256 = "2.16.840.1.101.3.4.2.1";
inline constexpr std::string_view kSha384 = "2.16.840.1.101.3.4.2.2";
inline constexpr std::string_view kSha512 = "2.16.840.1.101.3.4.2.3";
inline constexpr std::string_view kGost3411_256 = "1.2.643.7.1.1.2.2";
inline constexpr std::string_view kGost3411_512 = "1.2.643.7.1.1.2.3";

inline constexpr std::string_view kRsaEncryption = "1.2.840.113549.1.1.1";
inline constexpr std::string_view kSha1WithRsa = "1.2.840.113549.1.1.5";
inline constexpr std::string_view kSha256WithRsa = "1.2.840.113549.1.1.11";
inline constexpr std::string_view kSha384WithRsa = "1.2.840.113549.1.1.12";
inline constexpr std::string_view kSha512WithRsa = "1.2.840.113549.1.1.13";
inline constexpr std::string_view kRsaPss = "1.2.840.113549.1.1.10";
inline constexpr std::string_view kMgf1 = "1.2.840.113549.1.1.8";
inline constexpr std::string_view kEcPublicKey = "1.2.840.10045.2.1";
inline constexpr std::string_view kEcdsaSha1 = "1.2.840.10045.4.1";
inline constexpr std::string_view kEcdsaSha256 = "1.2.840.10045.4.3.2";
inline constexpr std::string_view kEcdsaSha384 = "1.2.840.10045.4.3.3";
inline constexpr std::string_view kEcdsaSha512 = "1.2.840.10045.4.3.4";
inline constexpr std::string_view kGost3410_256 = "1.2.643.7.1.1.3.2";
inline constexpr std::string_view kGost3410_512 = "1.2.643.7.1.1.3.3";
inline constexpr std::string_view kGost3410_256Key = "1.2.643.7.1.1.1.1";
inline constexpr std::string_view kGost3410_512Key = "1.2.643.7.1.1.1.2";

inline constexpr std::string_view kData = "1.2.840.113549.1.7.1";
inline constexpr std::string_view kSignedData = "1.2.840.113549.1.7.2";
inline constexpr std::string_view kContentType = "1.2.840.113549.1.9.3";
inline constexpr std::string_view kMessageDigest = "1.2.840.113549.1.9.4";
inline constexpr std::string_view kSigningTime = "1.2.840.113549.1.9.5";
inline constexpr std::string_view kCounterSignature = "1.2.840.113549.1.9.6";
inline constexpr std::string_view kSigningCertificateV1 = "1.2.840.113549.1.9.16.2.12";
inline constexpr std::string_view kSigningCertificateV2 = "1.2.840.113549.1.9.16.2.47";
inline constexpr std::string_view kSignatureTimeStampToken = "1.2.840.113549.1.9.16.2.14";
inline constexpr std::string_view kTstInfo = "1.2.840.113549.1.9.16.1.4";
} // namespace oid

std::optional<DigestAlgorithm> digestFromOid(std::string_view dotted);
std::string_view digestOid(DigestAlgorithm);
// Friendly name of a well-known OID, or the dotted string itself.
std::string oidName(std::string_view dotted);

} // namespace rivet::crypto
