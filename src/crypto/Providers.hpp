// SPDX-License-Identifier: MPL-2.0
#pragma once

// Provider interfaces of the Rivet crypto layer. Rivet owns the typed API and
// semantics; private-key math is always done by a vetted provider (OpenSSL,
// later CryptoPro CSP / PKCS#11), never by Rivet code. No backend types
// (OpenSSL, CryptoPro, PKCS#11, CNG) appear in these headers: providers hide
// them behind pimpl / opaque handles.

#include "crypto/Algorithms.hpp"
#include "crypto/Certificate.hpp"
#include "crypto/Error.hpp"
#include "crypto/SecretBuffer.hpp"
#include "crypto/Types.hpp"
#include "crypto/Validation.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto {

// Incremental digest (large detached files are streamed through this).
class Hasher {
public:
    virtual ~Hasher() = default;
    virtual void update(ByteView data) = 0;
    virtual Bytes finish() = 0;
};

// Opaque signing handle. The private key never leaves the provider and is not
// exposed through this interface. sign() may block (hardware PIN prompt,
// remote confirmation): run it through runCryptoAsync (Async.hpp).
class SigningIdentity {
public:
    virtual ~SigningIdentity() = default;

    virtual std::string_view providerName() const = 0;
    virtual const CertificateInfo& certificate() const = 0;
    // Certificate chain, leaf first (the leaf is chain()[0]).
    virtual std::vector<Bytes> chainDer() const = 0;
    virtual PublicKeyType keyType() const = 0;
    virtual int keyBits() const = 0;
    virtual std::vector<SignatureAlgorithm> supportedSignatures() const = 0;

    // Signs `message` (the data to be signed; the provider hashes it).
    // Enforces the creation policy itself (defence in depth).
    virtual CryptoResult<Bytes> sign(SignatureAlgorithm algorithm, ByteView message, const CancelToken& cancel) = 0;
    // Signs an already computed digest. Providers that cannot do this
    // (RSA-PSS, ECDSA over raw digests on some tokens) report Unsupported.
    virtual CryptoResult<Bytes> signDigest(SignatureAlgorithm algorithm, ByteView digest, const CancelToken& cancel) {
        (void)algorithm;
        (void)digest;
        (void)cancel;
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "provider cannot sign a precomputed digest");
    }
    // True only for software identities that may be exported as PKCS#12.
    virtual bool isExportable() const { return false; }
};

struct Pkcs12Contents {
    std::vector<std::unique_ptr<SigningIdentity>> identities; // one per private key
    std::vector<Bytes> otherCertificates;                      // certificates not part of an identity chain
};

struct SelfSignedRequest {
    std::string commonName;
    std::optional<std::string> email;
    std::optional<std::string> organization;
    KeyAlgorithm key = KeyAlgorithm::Rsa3072;
    std::optional<std::chrono::system_clock::time_point> notBefore; // default: now
    std::chrono::hours validity = std::chrono::hours(24 * 365 * 2);
};

class CryptoProvider {
public:
    virtual ~CryptoProvider() = default;

    virtual std::string_view name() const = 0;
    // False for the null provider: every operation fails BackendUnavailable.
    virtual bool isAvailable() const = 0;
    virtual bool supports(DigestAlgorithm) const = 0;
    virtual bool supports(SignatureAlgorithm) const = 0;

    virtual CryptoResult<std::unique_ptr<Hasher>> createHasher(DigestAlgorithm) = 0;
    virtual CryptoResult<Bytes> randomBytes(std::size_t count) = 0;

    virtual CryptoResult<CertificateInfo> parseCertificate(ByteView der) = 0;
    // True when `cert`'s signature verifies under `issuer`'s public key.
    virtual bool isIssuedBy(const CertificateInfo& cert, const CertificateInfo& issuer) = 0;
    // Verifies `signature` over `message` with the public key of `signer`.
    // Returns Ok on a valid signature, KeyMismatch / CorruptData otherwise
    // (an invalid signature is reported as CorruptData with a short message).
    virtual CryptoOk verifySignature(const CertificateInfo& signer, const SignatureScheme& scheme, ByteView message,
                                     ByteView signature) = 0;

    // PKCS#12 (.p12/.pfx): typed errors WrongPassword, CorruptData,
    // NoPrivateKey, UnsupportedAlgorithm.
    virtual CryptoResult<Pkcs12Contents> importPkcs12(ByteView data, const SecretBuffer& password) = 0;
    virtual CryptoResult<std::unique_ptr<SigningIdentity>> createSelfSignedIdentity(const SelfSignedRequest&) = 0;
    // Only identities created or imported by this provider that report
    // isExportable() can be exported (password-protected PKCS#12).
    virtual CryptoResult<Bytes> exportPkcs12(const SigningIdentity& identity, const SecretBuffer& password) = 0;

    // Convenience: one-shot digest.
    CryptoResult<Bytes> digest(DigestAlgorithm algorithm, ByteView data) {
        auto hasher = createHasher(algorithm);
        if (!hasher) return std::unexpected(hasher.error());
        (*hasher)->update(data);
        return (*hasher)->finish();
    }
};

// Process-wide provider selection: the OpenSSL provider when the build has
// RIVET_ENABLE_CRYPTO, otherwise the null provider.
std::shared_ptr<CryptoProvider> makeDefaultProvider();
std::shared_ptr<CryptoProvider> makeNullProvider();

class CertificateStore {
public:
    virtual ~CertificateStore() = default;
    virtual void add(const CertificateInfo& certificate) = 0;
    virtual std::vector<CertificateInfo> all() const = 0;
    virtual std::optional<CertificateInfo> findBySha256(std::string_view fingerprint) const = 0;
};

class InMemoryCertificateStore final : public CertificateStore {
public:
    void add(const CertificateInfo& certificate) override;
    std::vector<CertificateInfo> all() const override;
    std::optional<CertificateInfo> findBySha256(std::string_view fingerprint) const override;

private:
    mutable std::mutex mutex_;
    std::vector<CertificateInfo> certificates_;
};

struct CertificateValidationRequest {
    CertificateInfo certificate;
    std::vector<CertificateInfo> intermediates;
    std::chrono::system_clock::time_point at = std::chrono::system_clock::now();
};

class CertificateValidator {
public:
    virtual ~CertificateValidator() = default;
    virtual CertificateValidation validate(const CertificateValidationRequest& request) = 0;
};

// Chain building against a TrustAnchorSet; no network, no revocation.
class AnchorSetValidator final : public CertificateValidator {
public:
    AnchorSetValidator(std::shared_ptr<CryptoProvider> provider, TrustAnchorSet anchors)
        : provider_(std::move(provider)), anchors_(std::move(anchors)) {}
    CertificateValidation validate(const CertificateValidationRequest& request) override;

private:
    std::shared_ptr<CryptoProvider> provider_;
    TrustAnchorSet anchors_;
};

// RFC 3161 time-stamping (interface only in this phase).
class TimestampProvider {
public:
    virtual ~TimestampProvider() = default;
    // Returns a DER TimeStampToken (CMS SignedData) for `digest`.
    virtual CryptoResult<Bytes> timestamp(DigestAlgorithm algorithm, ByteView digest, const CancelToken& cancel) = 0;
};

// OCSP / CRL revocation (interface only in this phase).
class RevocationProvider {
public:
    virtual ~RevocationProvider() = default;
    virtual CryptoResult<RevocationStatus> check(const CertificateInfo& certificate, const CertificateInfo& issuer,
                                                 std::chrono::system_clock::time_point at,
                                                 const CancelToken& cancel) = 0;
};

// Remote / cloud signing (e.g. a qualified-signature service). Interface only.
struct RemoteSession {
    std::string id; // opaque provider session handle
};
struct RemoteIdentityInfo {
    std::string id;
    std::string displayName;
    CertificateInfo certificate;
};
struct RemoteTransaction {
    std::string id; // opaque transaction handle
};
struct RemoteCredentials {
    std::string userId;
    SecretBuffer secret; // password / PIN, zeroed on destruction
};

class RemoteSigningProvider {
public:
    virtual ~RemoteSigningProvider() = default;
    virtual CryptoResult<RemoteSession> authenticate(const RemoteCredentials& credentials, const CancelToken& cancel) = 0;
    virtual CryptoResult<std::vector<RemoteIdentityInfo>> listIdentities(const RemoteSession& session,
                                                                         const CancelToken& cancel) = 0;
    virtual CryptoResult<RemoteTransaction> prepareTransaction(const RemoteSession& session, std::string_view identityId,
                                                               SignatureAlgorithm algorithm, ByteView digest,
                                                               const CancelToken& cancel) = 0;
    // Blocks until the user confirmed on their device (or the token is cancelled).
    virtual CryptoOk awaitConfirmation(const RemoteTransaction& transaction, const CancelToken& cancel) = 0;
    virtual CryptoResult<Bytes> obtainSignature(const RemoteTransaction& transaction, const CancelToken& cancel) = 0;
    virtual void cancel(const RemoteTransaction& transaction) = 0;
};

} // namespace rivet::crypto
