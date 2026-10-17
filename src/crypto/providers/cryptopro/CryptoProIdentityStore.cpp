// SPDX-License-Identifier: MPL-2.0
#include "crypto/providers/cryptopro/CryptoProIdentityStore.hpp"

#ifdef RIVET_CRYPTO_CRYPTOPRO
#include "crypto/providers/cryptopro/CryptoProBackend.hpp"
#endif

namespace rivet::crypto {

namespace {

class UnavailableCryptoProStore final : public IdentityStore {
public:
    explicit UnavailableCryptoProStore(std::string reason) : reason_(std::move(reason)) {}

    std::string_view name() const override { return "cryptopro"; }
    bool isAvailable() const override { return false; }
    std::string unavailableReason() const override { return reason_; }
    ProviderAssurance assurance() const override { return ProviderAssurance::CertifiedCspDeclared; }

    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken&) override {
        return fail(CryptoErrorCode::BackendUnavailable, reason_);
    }
    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view, std::shared_ptr<PinPrompt>,
                                                        const CancelToken&) override {
        return fail(CryptoErrorCode::BackendUnavailable, reason_);
    }

private:
    std::string reason_;
};

} // namespace

bool cryptoProCompiledIn() {
#ifdef RIVET_CRYPTO_CRYPTOPRO
    return true;
#else
    return false;
#endif
}

std::shared_ptr<IdentityStore> makeCryptoProIdentityStore(const CryptoProOptions& options) {
#ifdef RIVET_CRYPTO_CRYPTOPRO
    std::string reason;
    if (auto store = cryptopro_detail::makeBackendStore(options, reason)) return store;
    return std::make_shared<UnavailableCryptoProStore>(std::move(reason));
#else
    (void)options;
    return std::make_shared<UnavailableCryptoProStore>("built without RIVET_ENABLE_CRYPTOPRO");
#endif
}

std::shared_ptr<CryptoProvider> makeCryptoProProvider(const CryptoProOptions& options) {
#ifdef RIVET_CRYPTO_CRYPTOPRO
    std::string reason;
    if (auto provider = cryptopro_detail::makeBackendProvider(options, reason)) return provider;
#else
    (void)options;
#endif
    return makeNullProvider();
}

} // namespace rivet::crypto
