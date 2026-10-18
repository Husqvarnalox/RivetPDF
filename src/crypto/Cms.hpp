// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crypto/Algorithms.hpp"
#include "crypto/Certificate.hpp"
#include "crypto/Error.hpp"
#include "crypto/Providers.hpp"
#include "crypto/Types.hpp"
#include "crypto/Validation.hpp"
#include "crypto/timestamp/TimestampInfo.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Rivet-owned CMS (RFC 5652) SignedData model. Parsing and encoding are done
// by Rivet's strict, bounded DER code (Der.hpp); hashing, signature
// verification and key operations are delegated to a CryptoProvider. A model
// that was parsed re-encodes with its signed attributes byte-for-byte intact,
// so existing signatures survive co-signing and countersigning.
namespace rivet::crypto {

inline constexpr std::size_t kMaxAttachedContent = std::size_t{256} << 20; // 256 MiB

struct CmsAttribute {
    std::string oid;
    std::vector<Bytes> values; // DER of each value
    Bytes der;                 // the complete Attribute SEQUENCE
};

struct CmsSignerInfo {
    int version = 1;
    Bytes sidDer;                    // complete SignerIdentifier element
    Bytes issuerDer;                 // issuerAndSerialNumber form
    Bytes serial;                    // normalized INTEGER content
    Bytes subjectKeyId;              // subjectKeyIdentifier form
    std::string digestOid;
    Bytes digestAlgDer;              // complete AlgorithmIdentifier
    std::vector<CmsAttribute> signedAttrs;
    Bytes signedAttrsDer;            // [0] IMPLICIT element exactly as received/produced; empty when absent
    std::string signatureAlgOid;
    Bytes signatureAlgDer;           // complete AlgorithmIdentifier
    Bytes signature;
    std::vector<CmsAttribute> unsignedAttrs;

    bool hasSignedAttrs() const { return !signedAttrsDer.empty(); }
    const CmsAttribute* findSigned(std::string_view oid) const;
    const CmsAttribute* findUnsigned(std::string_view oid) const;
};

struct CmsSignedData {
    std::string contentType{oid::kData}; // eContentType
    std::optional<Bytes> content;         // attached content; nullopt = detached
    std::vector<Bytes> certificates;      // DER certificates
    std::vector<Bytes> crls;              // DER CRLs, opaque
    std::vector<CmsSignerInfo> signers;

    bool isDetached() const { return !content.has_value(); }
};

// Parses a CMS ContentInfo holding SignedData (DER, or BER with indefinite
// lengths). All sizes and the nesting depth are bounded; duplicate signed
// attributes are rejected.
CryptoResult<CmsSignedData> parseCms(ByteView data);
// Accepts DER/BER or PEM ("-----BEGIN CMS/PKCS7-----").
CryptoResult<CmsSignedData> parseCmsAny(ByteView data);
CryptoResult<Bytes> decodePem(ByteView pem);
CryptoResult<Bytes> encodeCms(const CmsSignedData& model);

// Source of the signed content: memory or a file hashed in streaming chunks.
class ContentSource {
public:
    static ContentSource fromBytes(Bytes bytes);
    static ContentSource fromFile(std::filesystem::path path);
    // NON-OWNING concatenation of memory ranges (e.g. the two signed ranges of
    // a PDF /ByteRange). The caller keeps the memory alive and unchanged for
    // the lifetime of the source and of every use of it.
    static ContentSource fromSpans(std::vector<ByteView> parts);

    CryptoResult<Bytes> digest(CryptoProvider& provider, DigestAlgorithm algorithm, const CancelToken& cancel) const;
    // Reads the whole content (bounded by kMaxAttachedContent).
    CryptoResult<Bytes> read() const;

private:
    std::shared_ptr<const Bytes> bytes_;
    std::vector<ByteView> spans_;
    bool useSpans_ = false;
    std::filesystem::path path_;
};

struct CmsSignOptions {
    DigestAlgorithm digest = DigestAlgorithm::Sha256;
    // Default: RSA PKCS#1 v1.5 or ECDSA over `digest`, by the key type.
    std::optional<SignatureAlgorithm> signature;
    bool cadesBes = true; // adds signing-certificate-v2
    // Adds the signing-time attribute. CAdES signatures embedded in PDF
    // (PAdES, EN 319 142-1) must NOT carry it: the time is the signature
    // dictionary's /M entry; set false together with cadesBes for them.
    bool includeSigningTime = true;
    bool includeChain = true; // embed the identity's whole chain, not just the leaf
    std::chrono::system_clock::time_point signingTime = std::chrono::system_clock::now();
    CancelToken cancel;
};

// Creates a SignedData with one signer. Detached: the content is hashed but
// not embedded. Creation policy (no SHA-1, no RSA < 2048) is enforced here
// and again inside the identity.
CryptoResult<CmsSignedData> signCms(CryptoProvider& provider, SigningIdentity& identity, const ContentSource& content,
                                    bool detached, const CmsSignOptions& options = {});

// Co-sign: appends one more SignerInfo over the same content. Existing
// signers are untouched. For a detached message, `content` is required.
CryptoOk addSigner(CmsSignedData& model, CryptoProvider& provider, SigningIdentity& identity,
                   const ContentSource* content, const CmsSignOptions& options = {});

// Countersign (RFC 5652 section 11.4): signs the signature value of signer
// `signerIndex` and stores the result as an unsigned countersignature attribute.
CryptoOk addCountersignature(CmsSignedData& model, std::size_t signerIndex, CryptoProvider& provider,
                             SigningIdentity& identity, const CmsSignOptions& options = {});

struct CmsVerifyOptions {
    const TrustAnchorSet* anchors = nullptr; // nullptr: nothing is trusted
    std::chrono::system_clock::time_point validationTime = std::chrono::system_clock::now();
    // Trust anchors for the TSA chain of an embedded signature time-stamp.
    // nullptr: the same set as `anchors`.
    const TrustAnchorSet* timestampAnchors = nullptr;
    std::vector<Bytes> extraCertificates;    // additional chain material
    CancelToken cancel;
};

struct CountersignatureVerification {
    std::optional<CertificateInfo> signer;
    ValidationResult validation;
};

struct SignerVerification {
    std::size_t index = 0;
    std::optional<CertificateInfo> signer;
    std::optional<DigestAlgorithm> digest;
    std::optional<std::chrono::system_clock::time_point> signingTime;
    bool hasTimestampToken = false;
    // Set when the signer carries a signature time-stamp attribute: genTime,
    // TSA subject and whether the token verified (validation.timestamp
    // mirrors timestamp->status).
    std::optional<SignatureTimestampInfo> timestamp;
    ValidationResult validation;
    std::vector<CountersignatureVerification> countersignatures;
};

// Verifies every signer. Detached messages need `detachedContent`. Returns an
// error only for conditions that prevent verification altogether (no
// backend, missing detached content, unreadable file, cancellation); a bad
// signature is a successful result whose state says so.
CryptoResult<std::vector<SignerVerification>> verifyCms(const CmsSignedData& model, CryptoProvider& provider,
                                                        const ContentSource* detachedContent,
                                                        const CmsVerifyOptions& options = {});

// Multi-line human-readable dump (type, digests, algorithms, certificates,
// signed attributes, timestamp presence). Never prints secrets or content.
// `provider` may be null: certificates are then listed by size only.
std::string describeCms(const CmsSignedData& model, CryptoProvider* provider);

} // namespace rivet::crypto
