// SPDX-License-Identifier: MPL-2.0
#pragma once

// Adapts a RemoteSigningProvider (cloud signing service, ADR-0047) to the
// SigningIdentity / IdentityStore interfaces, so the CMS builder (Cms.hpp) and
// the PDF signer assemble CAdES / PAdES around a signature value computed by
// the service. The private key never exists on this machine; Rivet hashes the
// signed attributes locally and (in HashOnly mode) only the digest leaves.

#include "crypto/IdentityStore.hpp"
#include "crypto/Providers.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rivet::crypto {

class RemoteSigningIdentity final : public SigningIdentity {
public:
    // `hasher` hashes the message and verifies the returned signature locally
    // (a service answering with a signature that does not verify under the
    // certificate is rejected). `prompt` may be null: a service that asks for
    // a PIN / OTP then fails with ConfirmationRequired.
    static CryptoResult<std::unique_ptr<RemoteSigningIdentity>> create(std::shared_ptr<RemoteSigningProvider> remote,
                                                                       RemoteIdentityInfo info,
                                                                       std::shared_ptr<CryptoProvider> hasher,
                                                                       std::shared_ptr<PinPrompt> prompt);

    // The document the service may receive in WholeDocument mode (e.g. the
    // PDF about to be signed). Never sent in HashOnly mode.
    void setDocument(std::shared_ptr<const Bytes> document);

    std::string_view providerName() const override { return remote_->name(); }
    const CertificateInfo& certificate() const override { return certificate_; }
    std::vector<Bytes> chainDer() const override { return info_.chainDer; }
    PublicKeyType keyType() const override { return certificate_.publicKeyType; }
    int keyBits() const override { return certificate_.keyBits; }
    std::vector<SignatureAlgorithm> supportedSignatures() const override;

    CryptoResult<Bytes> sign(SignatureAlgorithm algorithm, ByteView message, const CancelToken& cancel) override;
    CryptoResult<Bytes> signDigest(SignatureAlgorithm algorithm, ByteView digest, const CancelToken& cancel) override;

    // Number of successful signHash calls (each one may have cost the user a confirmation).
    std::size_t remoteCalls() const;

private:
    RemoteSigningIdentity() = default;
    CryptoResult<Bytes> remoteSign(SignatureAlgorithm algorithm, ByteView digest, const CancelToken& cancel);

    std::shared_ptr<RemoteSigningProvider> remote_;
    RemoteIdentityInfo info_;
    CertificateInfo certificate_;
    std::shared_ptr<CryptoProvider> hasher_;
    std::shared_ptr<PinPrompt> prompt_;

    mutable std::mutex mutex_;
    std::shared_ptr<const Bytes> document_;
    std::size_t calls_ = 0;
};

// Lists the identities of a remote service as IdentityDescriptors
// (KeyLocationKind::Cloud, ProviderAssurance::RemoteService).
class RemoteIdentityStore final : public IdentityStore {
public:
    RemoteIdentityStore(std::shared_ptr<RemoteSigningProvider> remote, std::shared_ptr<CryptoProvider> crypto);

    std::string_view name() const override { return remote_->name(); }
    bool isAvailable() const override { return crypto_ && crypto_->isAvailable(); }
    std::string unavailableReason() const override;
    ProviderAssurance assurance() const override { return ProviderAssurance::RemoteService; }

    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken& cancel) override;
    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view id, std::shared_ptr<PinPrompt> prompt,
                                                        const CancelToken& cancel) override;
    // Same as open(), with the concrete type (needed for setDocument()).
    CryptoResult<std::unique_ptr<RemoteSigningIdentity>> openRemote(std::string_view id,
                                                                    std::shared_ptr<PinPrompt> prompt,
                                                                    const CancelToken& cancel);

private:
    std::shared_ptr<RemoteSigningProvider> remote_;
    std::shared_ptr<CryptoProvider> crypto_;
    std::mutex mutex_;
    std::vector<RemoteIdentityInfo> known_;
};

} // namespace rivet::crypto
