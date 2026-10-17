// SPDX-License-Identifier: MPL-2.0
#include "crypto/providers/platform/PlatformIdentityStore.hpp"

#include "crypto/providers/platform/PlatformStoreImpl.hpp"

namespace rivet::crypto {

namespace {

// Used where the OS has no keystore. Nothing is faked: every operation fails
// with BackendUnavailable and unavailableReason() says what to use instead.
class UnavailablePlatformStore final : public IdentityStore {
public:
    std::string_view name() const override { return "platform"; }
    bool isAvailable() const override { return false; }
    std::string unavailableReason() const override { return "no platform keystore on this OS: use PKCS#11 or files"; }
    ProviderAssurance assurance() const override { return ProviderAssurance::PlatformKeystore; }

    CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken&) override {
        return fail(CryptoErrorCode::BackendUnavailable, unavailableReason());
    }
    CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view, std::shared_ptr<PinPrompt>,
                                                        const CancelToken&) override {
        return fail(CryptoErrorCode::BackendUnavailable, unavailableReason());
    }
};

} // namespace

std::shared_ptr<IdentityStore> makePlatformIdentityStore() {
#if defined(RIVET_CRYPTO_PLATFORM_MACOS)
    return platform_detail::makeMacKeychainStore();
#elif defined(RIVET_CRYPTO_PLATFORM_WINDOWS)
    return platform_detail::makeCngStore();
#else
    return std::make_shared<UnavailablePlatformStore>();
#endif
}

bool platformStoreSupported() {
#if defined(RIVET_CRYPTO_PLATFORM_MACOS) || defined(RIVET_CRYPTO_PLATFORM_WINDOWS)
    return true;
#else
    return false;
#endif
}

} // namespace rivet::crypto
