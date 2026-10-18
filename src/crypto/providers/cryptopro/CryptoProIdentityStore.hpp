// SPDX-License-Identifier: MPL-2.0
#pragma once

// CryptoPro CSP provider (GOST R 34.10-2012 / GOST R 34.11-2012) for the Rivet
// crypto layer. No CryptoPro types appear here: the CSP SDK headers are used
// only inside CryptoProApi.cpp / CryptoProBackend.cpp and the CSP libraries are
// loaded with dlopen at run time (ADR-0040).
//
// Without RIVET_ENABLE_CRYPTOPRO, or when the CSP is not installed, the
// factories return an unavailable store / provider: isAvailable() is false and
// every operation fails with BackendUnavailable.

#include "crypto/IdentityStore.hpp"
#include "crypto/Providers.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rivet::crypto {

struct CryptoProOptions {
    // Directory holding libcapi10 / libcapi20. Empty: probe the standard
    // /opt/cprocsp/lib locations.
    std::string libraryDirectory;
    // Development override, default false: skip the ownership / permission / symlink check that
    // normally precedes loading libcapi10 / libcapi20 (the directory must still be absolute).
    bool allowInsecureLibraryPath = false;
    // Certificate store enumerated by list() ("MY" = the user's personal store).
    std::string storeName = "MY";
    // Extra DER certificates (intermediate / root CAs) used to complete the
    // chain of a signing identity, in addition to the CSP CA / ROOT stores.
    std::vector<Bytes> extraChainCertificates;
};

// True when this build contains the CryptoPro backend (RIVET_ENABLE_CRYPTOPRO).
bool cryptoProCompiledIn();

// Verification of a GOST R 34.10-2012 signature over an already computed digest
// (e.g. the RFC 7091 test vectors, CMS signedAttrs digests). Implemented by the
// provider returned from makeCryptoProProvider (dynamic_cast).
class GostDigestVerifier {
public:
    virtual ~GostDigestVerifier() = default;
    // `digest` and `signature` use the on-the-wire byte order of
    // CryptoProvider::digest / SigningIdentity::sign.
    virtual CryptoOk verifyDigest(const CertificateInfo& signer, ByteView digest, ByteView signature) = 0;
};

// IdentityStore over the CryptoPro certificate store. Never null; check
// isAvailable() / unavailableReason().
std::shared_ptr<IdentityStore> makeCryptoProIdentityStore(const CryptoProOptions& options = {});

// CryptoProvider backed by the CSP: Streebog hashing, GOST R 34.10-2012
// signature verification and certificate parsing (backend-free X.509 reader).
// It does not generate keys, import PKCS#12 or create self-signed identities
// (those report UnsupportedAlgorithm). Never null; the null provider is
// returned when the CSP is unavailable.
std::shared_ptr<CryptoProvider> makeCryptoProProvider(const CryptoProOptions& options = {});

} // namespace rivet::crypto
