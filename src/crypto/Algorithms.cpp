// SPDX-License-Identifier: MPL-2.0
#include "crypto/Algorithms.hpp"

#include <array>

namespace rivet::crypto {

std::string_view toString(DigestAlgorithm a) {
    switch (a) {
    case DigestAlgorithm::Sha256: return "SHA-256";
    case DigestAlgorithm::Sha384: return "SHA-384";
    case DigestAlgorithm::Sha512: return "SHA-512";
    case DigestAlgorithm::Gost3411_2012_256: return "GOST R 34.11-2012 (256)";
    case DigestAlgorithm::Gost3411_2012_512: return "GOST R 34.11-2012 (512)";
    case DigestAlgorithm::Sha1: return "SHA-1 (legacy)";
    }
    return "?";
}

std::string_view toString(SignatureAlgorithm a) {
    switch (a) {
    case SignatureAlgorithm::RsaPkcs1Sha256: return "RSA PKCS#1 v1.5 SHA-256";
    case SignatureAlgorithm::RsaPkcs1Sha384: return "RSA PKCS#1 v1.5 SHA-384";
    case SignatureAlgorithm::RsaPkcs1Sha512: return "RSA PKCS#1 v1.5 SHA-512";
    case SignatureAlgorithm::RsaPssSha256: return "RSASSA-PSS SHA-256";
    case SignatureAlgorithm::RsaPssSha384: return "RSASSA-PSS SHA-384";
    case SignatureAlgorithm::RsaPssSha512: return "RSASSA-PSS SHA-512";
    case SignatureAlgorithm::EcdsaSha256: return "ECDSA SHA-256";
    case SignatureAlgorithm::EcdsaSha384: return "ECDSA SHA-384";
    case SignatureAlgorithm::EcdsaSha512: return "ECDSA SHA-512";
    case SignatureAlgorithm::Gost3410_2012_256: return "GOST R 34.10-2012 (256)";
    case SignatureAlgorithm::Gost3410_2012_512: return "GOST R 34.10-2012 (512)";
    }
    return "?";
}

std::string_view toString(SymmetricAlgorithm a) {
    switch (a) {
    case SymmetricAlgorithm::Aes128: return "AES-128";
    case SymmetricAlgorithm::Aes256: return "AES-256";
    case SymmetricAlgorithm::Kuznyechik: return "Kuznyechik";
    case SymmetricAlgorithm::Magma: return "Magma";
    }
    return "?";
}

std::string_view toString(PublicKeyType t) {
    switch (t) {
    case PublicKeyType::Unknown: return "unknown";
    case PublicKeyType::Rsa: return "RSA";
    case PublicKeyType::Ec: return "EC";
    case PublicKeyType::Gost3410_2012_256: return "GOST R 34.10-2012 (256)";
    case PublicKeyType::Gost3410_2012_512: return "GOST R 34.10-2012 (512)";
    }
    return "?";
}

std::string_view toString(KeyAlgorithm a) {
    switch (a) {
    case KeyAlgorithm::Rsa2048: return "RSA-2048";
    case KeyAlgorithm::Rsa3072: return "RSA-3072";
    case KeyAlgorithm::Rsa4096: return "RSA-4096";
    case KeyAlgorithm::EcdsaP256: return "ECDSA P-256";
    case KeyAlgorithm::EcdsaP384: return "ECDSA P-384";
    case KeyAlgorithm::EcdsaP521: return "ECDSA P-521";
    }
    return "?";
}

std::size_t digestSize(DigestAlgorithm a) {
    switch (a) {
    case DigestAlgorithm::Sha256:
    case DigestAlgorithm::Gost3411_2012_256: return 32;
    case DigestAlgorithm::Sha384: return 48;
    case DigestAlgorithm::Sha512:
    case DigestAlgorithm::Gost3411_2012_512: return 64;
    case DigestAlgorithm::Sha1: return 20;
    }
    return 0;
}

bool isLegacy(DigestAlgorithm a) { return a == DigestAlgorithm::Sha1; }

bool isGost(DigestAlgorithm a) {
    return a == DigestAlgorithm::Gost3411_2012_256 || a == DigestAlgorithm::Gost3411_2012_512;
}

bool isGost(SignatureAlgorithm a) {
    return a == SignatureAlgorithm::Gost3410_2012_256 || a == SignatureAlgorithm::Gost3410_2012_512;
}

DigestAlgorithm digestOf(SignatureAlgorithm a) {
    switch (a) {
    case SignatureAlgorithm::RsaPkcs1Sha256:
    case SignatureAlgorithm::RsaPssSha256:
    case SignatureAlgorithm::EcdsaSha256: return DigestAlgorithm::Sha256;
    case SignatureAlgorithm::RsaPkcs1Sha384:
    case SignatureAlgorithm::RsaPssSha384:
    case SignatureAlgorithm::EcdsaSha384: return DigestAlgorithm::Sha384;
    case SignatureAlgorithm::RsaPkcs1Sha512:
    case SignatureAlgorithm::RsaPssSha512:
    case SignatureAlgorithm::EcdsaSha512: return DigestAlgorithm::Sha512;
    case SignatureAlgorithm::Gost3410_2012_256: return DigestAlgorithm::Gost3411_2012_256;
    case SignatureAlgorithm::Gost3410_2012_512: return DigestAlgorithm::Gost3411_2012_512;
    }
    return DigestAlgorithm::Sha256;
}

PublicKeyType keyTypeOf(SignatureAlgorithm a) {
    switch (a) {
    case SignatureAlgorithm::RsaPkcs1Sha256:
    case SignatureAlgorithm::RsaPkcs1Sha384:
    case SignatureAlgorithm::RsaPkcs1Sha512:
    case SignatureAlgorithm::RsaPssSha256:
    case SignatureAlgorithm::RsaPssSha384:
    case SignatureAlgorithm::RsaPssSha512: return PublicKeyType::Rsa;
    case SignatureAlgorithm::EcdsaSha256:
    case SignatureAlgorithm::EcdsaSha384:
    case SignatureAlgorithm::EcdsaSha512: return PublicKeyType::Ec;
    case SignatureAlgorithm::Gost3410_2012_256: return PublicKeyType::Gost3410_2012_256;
    case SignatureAlgorithm::Gost3410_2012_512: return PublicKeyType::Gost3410_2012_512;
    }
    return PublicKeyType::Unknown;
}

SignatureScheme schemeOf(SignatureAlgorithm a) {
    SignatureScheme s;
    s.keyType = keyTypeOf(a);
    s.digest = digestOf(a);
    switch (a) {
    case SignatureAlgorithm::RsaPssSha256:
    case SignatureAlgorithm::RsaPssSha384:
    case SignatureAlgorithm::RsaPssSha512: s.padding = SignaturePadding::Pss; break;
    case SignatureAlgorithm::EcdsaSha256:
    case SignatureAlgorithm::EcdsaSha384:
    case SignatureAlgorithm::EcdsaSha512: s.padding = SignaturePadding::Ecdsa; break;
    case SignatureAlgorithm::Gost3410_2012_256:
    case SignatureAlgorithm::Gost3410_2012_512: s.padding = SignaturePadding::Gost; break;
    default: s.padding = SignaturePadding::Pkcs1; break;
    }
    return s;
}

std::optional<SignatureAlgorithm> signatureAlgorithmOf(const SignatureScheme& s) {
    using S = SignatureAlgorithm;
    auto pick = [&](S a, S b, S c) -> std::optional<S> {
        switch (s.digest) {
        case DigestAlgorithm::Sha256: return a;
        case DigestAlgorithm::Sha384: return b;
        case DigestAlgorithm::Sha512: return c;
        default: return std::nullopt;
        }
    };
    if (s.keyType == PublicKeyType::Rsa && s.padding == SignaturePadding::Pkcs1)
        return pick(S::RsaPkcs1Sha256, S::RsaPkcs1Sha384, S::RsaPkcs1Sha512);
    if (s.keyType == PublicKeyType::Rsa && s.padding == SignaturePadding::Pss && s.pssSaltLength == -1)
        return pick(S::RsaPssSha256, S::RsaPssSha384, S::RsaPssSha512);
    if (s.keyType == PublicKeyType::Ec && s.padding == SignaturePadding::Ecdsa)
        return pick(S::EcdsaSha256, S::EcdsaSha384, S::EcdsaSha512);
    if (s.keyType == PublicKeyType::Gost3410_2012_256 && s.digest == DigestAlgorithm::Gost3411_2012_256)
        return S::Gost3410_2012_256;
    if (s.keyType == PublicKeyType::Gost3410_2012_512 && s.digest == DigestAlgorithm::Gost3411_2012_512)
        return S::Gost3410_2012_512;
    return std::nullopt;
}

std::optional<SignatureAlgorithm> defaultSignatureFor(PublicKeyType key, DigestAlgorithm digest) {
    SignatureScheme s;
    s.keyType = key;
    s.digest = digest;
    s.padding = key == PublicKeyType::Ec ? SignaturePadding::Ecdsa
                : key == PublicKeyType::Rsa ? SignaturePadding::Pkcs1
                                            : SignaturePadding::Gost;
    return signatureAlgorithmOf(s);
}

std::optional<int> rsaBitsOf(KeyAlgorithm a) {
    switch (a) {
    case KeyAlgorithm::Rsa2048: return 2048;
    case KeyAlgorithm::Rsa3072: return 3072;
    case KeyAlgorithm::Rsa4096: return 4096;
    default: return std::nullopt;
    }
}

CryptoOk checkCreationAllowed(DigestAlgorithm digest) {
    if (isLegacy(digest))
        return fail(CryptoErrorCode::WeakAlgorithm, "SHA-1 is a legacy digest; creating signatures with it is refused");
    return {};
}

CryptoOk checkCreationAllowed(SignatureAlgorithm alg, PublicKeyType keyType, int keyBits) {
    if (auto digest = checkCreationAllowed(digestOf(alg)); !digest) return digest;
    if (keyTypeOf(alg) != keyType)
        return fail(CryptoErrorCode::KeyMismatch, "signature algorithm does not match the key type");
    if (keyType == PublicKeyType::Rsa && keyBits < 2048)
        return fail(CryptoErrorCode::WeakAlgorithm, "RSA keys below 2048 bits are refused for signing");
    if (keyType == PublicKeyType::Ec && keyBits < 256)
        return fail(CryptoErrorCode::WeakAlgorithm, "EC keys below 256 bits are refused for signing");
    return {};
}

bool isWeakForVerification(DigestAlgorithm digest, PublicKeyType keyType, int keyBits) {
    if (isLegacy(digest)) return true;
    if (keyType == PublicKeyType::Rsa && keyBits > 0 && keyBits < 2048) return true;
    return false;
}

// ---- OIDs -------------------------------------------------------------------

std::optional<DigestAlgorithm> digestFromOid(std::string_view d) {
    if (d == oid::kSha256) return DigestAlgorithm::Sha256;
    if (d == oid::kSha384) return DigestAlgorithm::Sha384;
    if (d == oid::kSha512) return DigestAlgorithm::Sha512;
    if (d == oid::kSha1) return DigestAlgorithm::Sha1;
    if (d == oid::kGost3411_256) return DigestAlgorithm::Gost3411_2012_256;
    if (d == oid::kGost3411_512) return DigestAlgorithm::Gost3411_2012_512;
    return std::nullopt;
}

std::string_view digestOid(DigestAlgorithm a) {
    switch (a) {
    case DigestAlgorithm::Sha256: return oid::kSha256;
    case DigestAlgorithm::Sha384: return oid::kSha384;
    case DigestAlgorithm::Sha512: return oid::kSha512;
    case DigestAlgorithm::Sha1: return oid::kSha1;
    case DigestAlgorithm::Gost3411_2012_256: return oid::kGost3411_256;
    case DigestAlgorithm::Gost3411_2012_512: return oid::kGost3411_512;
    }
    return {};
}

std::string oidName(std::string_view d) {
    struct Entry {
        std::string_view oid;
        std::string_view name;
    };
    static constexpr std::array<Entry, 30> kNames{{
        {oid::kSha1, "sha1"},
        {oid::kSha256, "sha256"},
        {oid::kSha384, "sha384"},
        {oid::kSha512, "sha512"},
        {oid::kGost3411_256, "gostR3411-2012-256"},
        {oid::kGost3411_512, "gostR3411-2012-512"},
        {oid::kRsaEncryption, "rsaEncryption"},
        {oid::kSha1WithRsa, "sha1WithRSAEncryption"},
        {oid::kSha256WithRsa, "sha256WithRSAEncryption"},
        {oid::kSha384WithRsa, "sha384WithRSAEncryption"},
        {oid::kSha512WithRsa, "sha512WithRSAEncryption"},
        {oid::kRsaPss, "RSASSA-PSS"},
        {oid::kEcPublicKey, "ecPublicKey"},
        {oid::kEcdsaSha1, "ecdsa-with-SHA1"},
        {oid::kEcdsaSha256, "ecdsa-with-SHA256"},
        {oid::kEcdsaSha384, "ecdsa-with-SHA384"},
        {oid::kEcdsaSha512, "ecdsa-with-SHA512"},
        {oid::kGost3410_256, "gostR3410-2012-256"},
        {oid::kGost3410_512, "gostR3410-2012-512"},
        {oid::kData, "id-data"},
        {oid::kSignedData, "id-signedData"},
        {oid::kContentType, "contentType"},
        {oid::kMessageDigest, "messageDigest"},
        {oid::kSigningTime, "signingTime"},
        {oid::kCounterSignature, "countersignature"},
        {oid::kSigningCertificateV1, "signingCertificate"},
        {oid::kSigningCertificateV2, "signingCertificateV2"},
        {oid::kSignatureTimeStampToken, "signatureTimeStampToken"},
        {oid::kTstInfo, "id-ct-TSTInfo"},
        {oid::kGost3410_256Key, "gostR3410-2012-256-key"},
    }};
    for (const Entry& e : kNames)
        if (e.oid == d) return std::string(e.name);
    return std::string(d);
}

} // namespace rivet::crypto
