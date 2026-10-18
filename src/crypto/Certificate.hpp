// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crypto/Algorithms.hpp"
#include "crypto/Types.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rivet::crypto {

class CryptoProvider;

namespace keyusage {
inline constexpr std::uint16_t kDigitalSignature = 0x001;
inline constexpr std::uint16_t kNonRepudiation = 0x002; // contentCommitment
inline constexpr std::uint16_t kKeyEncipherment = 0x004;
inline constexpr std::uint16_t kDataEncipherment = 0x008;
inline constexpr std::uint16_t kKeyAgreement = 0x010;
inline constexpr std::uint16_t kKeyCertSign = 0x020;
inline constexpr std::uint16_t kCrlSign = 0x040;
inline constexpr std::uint16_t kEncipherOnly = 0x080;
inline constexpr std::uint16_t kDecipherOnly = 0x100;
} // namespace keyusage

// Russian qualified-certificate subject attributes. Only attributes that are
// actually present in the subject are set.
struct RussianAttributes {
    std::optional<std::string> inn;    // 1.2.643.3.131.1.1
    std::optional<std::string> snils;  // 1.2.643.100.3
    std::optional<std::string> ogrn;   // 1.2.643.100.1
    std::optional<std::string> ogrnip; // 1.2.643.100.5
    std::optional<std::string> innLe;  // 1.2.643.100.4 (legal entity INN)

    bool any() const { return inn || snils || ogrn || ogrnip || innLe; }
};

// Parsed X.509 certificate, independent of any crypto backend.
struct CertificateInfo {
    Bytes der;
    std::string subject; // RFC 2253 style
    std::string issuer;
    Bytes subjectDer;    // DER Name, as encoded in the certificate
    Bytes issuerDer;
    Bytes serial;        // INTEGER content, leading zeros removed
    std::string serialHex;
    std::chrono::system_clock::time_point notBefore{};
    std::chrono::system_clock::time_point notAfter{};

    bool hasKeyUsage = false;
    std::uint16_t keyUsage = 0; // keyusage::k* bits
    std::vector<std::string> extendedKeyUsageOids;
    bool isCa = false;
    std::optional<int> pathLength;

    std::string sha1Fingerprint;   // lowercase hex
    std::string sha256Fingerprint; // lowercase hex
    Bytes subjectKeyId;
    Bytes authorityKeyId;

    PublicKeyType publicKeyType = PublicKeyType::Unknown;
    int keyBits = 0;
    std::string signatureAlgorithmOid;

    std::optional<std::string> commonName;
    std::optional<std::string> email;
    std::optional<std::string> organization;
    RussianAttributes russian;

    bool isSelfIssued() const { return !subjectDer.empty() && subjectDer == issuerDer; }
    bool validAt(std::chrono::system_clock::time_point t) const { return t >= notBefore && t <= notAfter; }
};

// Pluggable set of trust anchors, keyed by SHA-256 fingerprint. Rivet ships
// no built-in roots; the application (or a test) adds anchors explicitly.
class TrustAnchorSet {
public:
    void add(const CertificateInfo& cert);
    bool contains(const CertificateInfo& cert) const;
    // Anchors whose subject DN equals `issuerDer`.
    std::vector<CertificateInfo> findBySubject(ByteView subjectDer) const;
    std::size_t size() const { return anchors_.size(); }

private:
    std::vector<CertificateInfo> anchors_;
};

struct ChainResult {
    std::vector<CertificateInfo> chain; // leaf first
    bool complete = false;              // ends in a self-issued root or a trust anchor
    bool anchored = false;              // the chain ends in a trust anchor
};

// Builds the issuer chain for `leaf` from `pool` and `anchors`, verifying each
// link's signature through `provider`. Issuers must be CAs; the chain is
// bounded to 10 certificates. Never throws; an incomplete chain is a result,
// not an error.
ChainResult buildChain(const CertificateInfo& leaf, const std::vector<CertificateInfo>& pool,
                       const TrustAnchorSet& anchors, CryptoProvider& provider);

// Chain-validity summary of one validation run.
enum class CertificateStatus : std::uint8_t { Valid, Expired, NotYetValid, UsageInvalid, Unknown };
enum class TrustStatus : std::uint8_t { Trusted, Untrusted, Unknown };

struct CertificateValidation {
    CertificateStatus certificate = CertificateStatus::Unknown;
    TrustStatus trust = TrustStatus::Unknown;
    ChainResult chain;
};

// Validates a signing certificate at a given time: validity period of every
// chain element, key-usage suitability for document signing, trust anchor.
CertificateValidation validateCertificate(const CertificateInfo& leaf, const std::vector<CertificateInfo>& pool,
                                          const TrustAnchorSet& anchors, CryptoProvider& provider,
                                          std::chrono::system_clock::time_point at);

} // namespace rivet::crypto
