// SPDX-License-Identifier: MPL-2.0
#pragma once

// Enumeration of signing identities offered by a provider (CryptoPro CSP,
// PKCS#11 tokens, the macOS keychain, the Windows certificate store). No
// backend type appears here. Identities are opaque: the private key never
// leaves the provider.

#include "crypto/Certificate.hpp"
#include "crypto/Error.hpp"
#include "crypto/PinPrompt.hpp"
#include "crypto/Providers.hpp"
#include "crypto/Types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto {

// Where the private key physically lives. Pure data for display.
enum class KeyLocationKind : std::uint8_t {
    Unknown,
    Container, // CSP key container (disk / registry-like store)
    Token,     // smart card / USB token
    Cloud,     // remote / cloud signing service
    Platform,  // OS keystore (macOS keychain, Windows cert store / CNG)
    Software,  // software key (PKCS#12 / in-memory)
};

struct KeyLocation {
    KeyLocationKind kind = KeyLocationKind::Unknown;
    std::string label;  // container / token label, as reported by the backend
    std::string device; // reader / slot / provider-specific name, may be empty
};

// What kind of key custody the provider represents. Input of the signature
// classification (Qualification.hpp); never proves legal status by itself.
enum class ProviderAssurance : std::uint8_t {
    Software,            // exportable software keys (OpenSSL)
    PlatformKeystore,    // OS keystore, non-exportable
    HardwareToken,       // PKCS#11 token, non-exportable
    CertifiedCspDeclared, // a CSP family sold as a certified cryptographic means (CryptoPro CSP).
                         // Certification of the installed build / a valid licence is NOT verified by Rivet.
    RemoteService        // key held by a remote / cloud signing service (ADR-0047). Custody, qualification
                         // and certification of the service are NOT verified by Rivet; this value never
                         // reaches QualifiedEvidence on its own.
};

struct IdentityDescriptor {
    std::string id; // opaque, stable for one store instance; pass to open()
    std::string providerName;
    CertificateInfo certificate;
    KeyLocation location;
    bool privateKeyAvailable = false; // key container / token object found and reachable
    bool certificateExpired = false;  // at enumeration time
};

class IdentityStore {
public:
    virtual ~IdentityStore() = default;

    virtual std::string_view name() const = 0;
    // False when the backend is not compiled in or not installed; list() and
    // open() then fail with BackendUnavailable and nothing crashes.
    virtual bool isAvailable() const = 0;
    // Human-readable reason when !isAvailable() (no secrets).
    virtual std::string unavailableReason() const { return {}; }
    virtual ProviderAssurance assurance() const = 0;

    virtual CryptoResult<std::vector<IdentityDescriptor>> list(const CancelToken& cancel) = 0;
    // `prompt` is kept by the returned identity and used by sign(); it may be
    // null (a PIN-protected key then fails with Cancelled).
    virtual CryptoResult<std::unique_ptr<SigningIdentity>> open(std::string_view id, std::shared_ptr<PinPrompt> prompt,
                                                                const CancelToken& cancel) = 0;
};

} // namespace rivet::crypto
