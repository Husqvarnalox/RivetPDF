// SPDX-License-Identifier: MPL-2.0
#include "crypto/Providers.hpp"

namespace rivet::crypto {

namespace {

// Keeps the build green without a crypto backend: every operation fails with
// BackendUnavailable, nothing is faked.
class NullProvider final : public CryptoProvider {
public:
    std::string_view name() const override { return "null"; }
    bool isAvailable() const override { return false; }
    bool supports(DigestAlgorithm) const override { return false; }
    bool supports(SignatureAlgorithm) const override { return false; }

    CryptoResult<std::unique_ptr<Hasher>> createHasher(DigestAlgorithm) override { return unavailable(); }
    CryptoResult<Bytes> randomBytes(std::size_t) override { return unavailable(); }
    CryptoResult<CertificateInfo> parseCertificate(ByteView) override { return unavailable(); }
    bool isIssuedBy(const CertificateInfo&, const CertificateInfo&) override { return false; }
    CryptoOk verifySignature(const CertificateInfo&, const SignatureScheme&, ByteView, ByteView) override {
        return unavailable();
    }
    CryptoResult<Pkcs12Contents> importPkcs12(ByteView, const SecretBuffer&) override { return unavailable(); }
    CryptoResult<std::unique_ptr<SigningIdentity>> createSelfSignedIdentity(const SelfSignedRequest&) override {
        return unavailable();
    }
    CryptoResult<Bytes> exportPkcs12(const SigningIdentity&, const SecretBuffer&) override { return unavailable(); }

private:
    static std::unexpected<CryptoError> unavailable() {
        return fail(CryptoErrorCode::BackendUnavailable, "Rivet was built without a crypto backend (RIVET_ENABLE_CRYPTO=OFF)");
    }
};

} // namespace

std::shared_ptr<CryptoProvider> makeNullProvider() { return std::make_shared<NullProvider>(); }

#if !defined(RIVET_CRYPTO_OPENSSL)
std::shared_ptr<CryptoProvider> makeDefaultProvider() { return makeNullProvider(); }
#endif

} // namespace rivet::crypto
