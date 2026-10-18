// SPDX-License-Identifier: MPL-2.0
#pragma once

// CryptoPro DSS (Digital Signature Service) adapter for the remote-signing
// framework (Providers.hpp, ADR-0047).
//
// STATUS: built against an in-process MOCK service only. The endpoint paths,
// field names, status words and error codes in CryptoProDss.cpp are
// ASSUMPTIONS that have NOT been verified against vendor documentation or a
// real DSS; real cloud qualified-signature operation is BLOCKED (no
// credentials). All wire details live in CryptoProDss.cpp so that they are
// easy to correct.

#include "crypto/Providers.hpp"
#include "net/Client.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace rivet::crypto {

// Consent confirmation for RemotePrivacyMode::WholeDocument. The UI shows this
// text (or a translation of it chosen by the application) and passes it back
// verbatim in CryptoProDssOptions::wholeDocumentConsent. Anything else is
// refused: uploading a document is never implicit.
inline constexpr std::string_view kDssWholeDocumentConsentText =
    "Whole-document mode: the COMPLETE document will be uploaded to the signing service, not only its hash. "
    "Do not use it for confidential documents unless you trust the service.";

struct CryptoProDssOptions {
    // Service base URL, https only (e.g. "https://dss.example.com/dss"); no
    // userinfo, no query. The adapter refuses http even if the NetworkPolicy
    // would allow it: credentials and tokens never travel in clear text.
    std::string serviceUrl;
    std::string clientId; // OAuth client id (public identifier, not a secret)

    RemotePrivacyMode privacy = RemotePrivacyMode::HashOnly;
    std::string wholeDocumentConsent; // must equal kDssWholeDocumentConsentText for WholeDocument

    int maxRetries = 2;                                    // retries after HTTP 5xx; attempts = 1 + maxRetries
    std::chrono::milliseconds retryBackoff{500};           // multiplied by the attempt number
    std::chrono::milliseconds pollInterval{2000};          // asynchronous confirmation polling
    std::chrono::milliseconds confirmationTimeout{120000}; // waiting for the user's confirmation
    int maxPinAttempts = 3;                                // PIN / OTP prompts per signature
};

class CryptoProDssProvider final : public RemoteSigningProvider {
public:
    // Errors: InvalidArgument (bad / non-https URL, missing consent for
    // WholeDocument, null dependencies).
    static CryptoResult<std::shared_ptr<CryptoProDssProvider>>
    create(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> crypto,
           CryptoProDssOptions options);

    // Installs a token obtained elsewhere. Tokens are kept in memory only
    // (never persisted by Rivet) and zeroed when replaced or on destruction.
    void setAccessToken(SecretBuffer token);
    bool hasAccessToken() const;
    void forgetAccessToken();

    std::string_view name() const override { return "cryptopro-dss"; }
    RemotePrivacyMode privacyMode() const override { return options_.privacy; }
    CryptoOk authenticate(const RemoteCredentials& credentials, const CancelToken& cancel) override;
    CryptoResult<std::vector<RemoteIdentityInfo>> listIdentities(const CancelToken& cancel) override;
    CryptoResult<Bytes> signHash(const RemoteSignRequest& request, PinPrompt* prompt, const CancelToken& cancel) override;

private:
    CryptoProDssProvider() = default;

    std::shared_ptr<const net::NetworkClient> network_;
    std::shared_ptr<CryptoProvider> crypto_;
    CryptoProDssOptions options_;
    std::string base_; // normalised service URL without trailing slash

    mutable std::mutex mutex_; // guards token_ only; never held across network or prompt calls
    SecretBuffer token_;
};

} // namespace rivet::crypto
