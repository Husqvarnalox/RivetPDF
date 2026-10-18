// SPDX-License-Identifier: MPL-2.0
#include "crypto/RemoteIdentity.hpp"

#include <algorithm>

namespace rivet::crypto {

CryptoResult<std::unique_ptr<RemoteSigningIdentity>>
RemoteSigningIdentity::create(std::shared_ptr<RemoteSigningProvider> remote, RemoteIdentityInfo info,
                              std::shared_ptr<CryptoProvider> hasher, std::shared_ptr<PinPrompt> prompt) {
    if (!remote || !hasher) return fail(CryptoErrorCode::InvalidArgument, "remote identity needs a provider and a hasher");
    if (!hasher->isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, "no crypto backend available");
    if (info.chainDer.empty()) return fail(CryptoErrorCode::CertificateNotFound, "the service returned no certificate");
    auto cert = hasher->parseCertificate(info.chainDer.front());
    if (!cert) return std::unexpected(cert.error());
    std::unique_ptr<RemoteSigningIdentity> out(new RemoteSigningIdentity());
    out->remote_ = std::move(remote);
    out->info_ = std::move(info);
    out->certificate_ = std::move(*cert);
    out->hasher_ = std::move(hasher);
    out->prompt_ = std::move(prompt);
    return out;
}

void RemoteSigningIdentity::setDocument(std::shared_ptr<const Bytes> document) {
    std::lock_guard lock(mutex_);
    document_ = std::move(document);
}

std::size_t RemoteSigningIdentity::remoteCalls() const {
    std::lock_guard lock(mutex_);
    return calls_;
}

std::vector<SignatureAlgorithm> RemoteSigningIdentity::supportedSignatures() const {
    switch (certificate_.publicKeyType) {
    case PublicKeyType::Rsa:
        return {SignatureAlgorithm::RsaPkcs1Sha256, SignatureAlgorithm::RsaPkcs1Sha384, SignatureAlgorithm::RsaPkcs1Sha512};
    case PublicKeyType::Ec:
        return {SignatureAlgorithm::EcdsaSha256, SignatureAlgorithm::EcdsaSha384, SignatureAlgorithm::EcdsaSha512};
    case PublicKeyType::Gost3410_2012_256: return {SignatureAlgorithm::Gost3410_2012_256};
    case PublicKeyType::Gost3410_2012_512: return {SignatureAlgorithm::Gost3410_2012_512};
    case PublicKeyType::Unknown: break;
    }
    return {};
}

CryptoResult<Bytes> RemoteSigningIdentity::remoteSign(SignatureAlgorithm algorithm, ByteView digest,
                                                      const CancelToken& cancel) {
    if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
    if (auto ok = checkCreationAllowed(algorithm, keyType(), keyBits()); !ok) return std::unexpected(ok.error());
    const auto supported = supportedSignatures();
    if (std::find(supported.begin(), supported.end(), algorithm) == supported.end())
        return fail(CryptoErrorCode::UnsupportedAlgorithm, "signature algorithm does not fit the remote key");
    if (digest.size() != digestSize(digestOf(algorithm)))
        return fail(CryptoErrorCode::InvalidArgument, "digest length does not match the signature algorithm");

    RemoteSignRequest request;
    request.identityId = info_.id;
    request.algorithm = algorithm;
    request.digest.assign(digest.begin(), digest.end());
    {
        std::lock_guard lock(mutex_);
        request.document = document_;
    }
    // No lock across the (blocking, prompting) provider call.
    auto signature = remote_->signHash(request, prompt_.get(), cancel);
    if (!signature) return std::unexpected(signature.error());
    if (cancel.cancelled()) return fail(CryptoErrorCode::Cancelled, "operation cancelled");
    if (signature->empty()) return fail(CryptoErrorCode::BadResponse, "the service returned an empty signature");
    {
        std::lock_guard lock(mutex_);
        ++calls_;
    }
    return signature;
}

CryptoResult<Bytes> RemoteSigningIdentity::sign(SignatureAlgorithm algorithm, ByteView message, const CancelToken& cancel) {
    if (auto ok = checkCreationAllowed(algorithm, keyType(), keyBits()); !ok) return std::unexpected(ok.error());
    auto digest = hasher_->digest(digestOf(algorithm), message);
    if (!digest) return std::unexpected(digest.error());
    auto signature = remoteSign(algorithm, *digest, cancel);
    if (!signature) return signature;
    // Defence in depth: a signature that does not verify under the certificate
    // never reaches a document.
    if (auto ok = hasher_->verifySignature(certificate_, schemeOf(algorithm), message, *signature); !ok)
        return fail(CryptoErrorCode::BadResponse, "the service returned a signature that does not verify under its certificate");
    return signature;
}

CryptoResult<Bytes> RemoteSigningIdentity::signDigest(SignatureAlgorithm algorithm, ByteView digest,
                                                      const CancelToken& cancel) {
    return remoteSign(algorithm, digest, cancel);
}

// ---- store -------------------------------------------------------------------

RemoteIdentityStore::RemoteIdentityStore(std::shared_ptr<RemoteSigningProvider> remote,
                                         std::shared_ptr<CryptoProvider> crypto)
    : remote_(std::move(remote)), crypto_(std::move(crypto)) {}

std::string RemoteIdentityStore::unavailableReason() const {
    return isAvailable() ? std::string{} : "no crypto backend available to parse the service's certificates";
}

CryptoResult<std::vector<IdentityDescriptor>> RemoteIdentityStore::list(const CancelToken& cancel) {
    if (!isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, unavailableReason());
    auto identities = remote_->listIdentities(cancel);
    if (!identities) return std::unexpected(identities.error());
    std::vector<IdentityDescriptor> out;
    for (const auto& info : *identities) {
        if (info.chainDer.empty()) continue;
        auto cert = crypto_->parseCertificate(info.chainDer.front());
        if (!cert) continue; // an unparsable certificate is not offered
        IdentityDescriptor d;
        d.id = info.id;
        d.providerName = std::string(remote_->name());
        d.location = {KeyLocationKind::Cloud, info.displayName, std::string(remote_->name())};
        d.privateKeyAvailable = true; // held by the service; reachability is only known when signing
        d.certificateExpired = !cert->validAt(std::chrono::system_clock::now());
        d.certificate = std::move(*cert);
        out.push_back(std::move(d));
    }
    {
        std::lock_guard lock(mutex_);
        known_ = std::move(*identities);
    }
    return out;
}

CryptoResult<std::unique_ptr<RemoteSigningIdentity>>
RemoteIdentityStore::openRemote(std::string_view id, std::shared_ptr<PinPrompt> prompt, const CancelToken& cancel) {
    if (!isAvailable()) return fail(CryptoErrorCode::BackendUnavailable, unavailableReason());
    std::optional<RemoteIdentityInfo> found;
    auto find = [&] {
        std::lock_guard lock(mutex_);
        for (const auto& k : known_)
            if (k.id == id) found = k;
    };
    find();
    if (!found) {
        auto listed = list(cancel);
        if (!listed) return std::unexpected(listed.error());
        find();
    }
    if (!found) return fail(CryptoErrorCode::CertificateNotFound, "no such remote identity");
    return RemoteSigningIdentity::create(remote_, std::move(*found), crypto_, std::move(prompt));
}

CryptoResult<std::unique_ptr<SigningIdentity>> RemoteIdentityStore::open(std::string_view id,
                                                                         std::shared_ptr<PinPrompt> prompt,
                                                                         const CancelToken& cancel) {
    auto identity = openRemote(id, std::move(prompt), cancel);
    if (!identity) return std::unexpected(identity.error());
    return std::unique_ptr<SigningIdentity>(std::move(*identity));
}

} // namespace rivet::crypto
