// SPDX-License-Identifier: MPL-2.0
#pragma once

// Generic PKCS#11 (Cryptoki) identity store: smart cards and USB tokens
// (Rutoken, JaCarta, SoftHSM, ...) through the vendor's PKCS#11 module, which
// is loaded at run time from a configurable path. No PKCS#11 type appears in
// this header. See docs/adr/ADR-0041-pkcs11-and-platform-stores.md.

#include "crypto/IdentityStore.hpp"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace rivet::crypto {

// GOST R 34.10-2012 mechanism identifiers are vendor-specific (CKM_VENDOR_DEFINED
// space) and differ between token vendors and SDK versions, so Rivet has no
// built-in values: the application configures the ones its token SDK documents.
// A value that is not set disables the corresponding GOST path.
struct GostMechanisms {
    // Raw signing of a precomputed digest (token signs the 32 / 64 byte hash).
    std::optional<unsigned long> sign256;
    std::optional<unsigned long> sign512;
    // Combined "hash then sign" mechanisms (token hashes the message itself).
    std::optional<unsigned long> signWithDigest256;
    std::optional<unsigned long> signWithDigest512;
    // Token-side GOST R 34.11-2012 digest mechanisms (used to hash when no
    // software GOST hasher is available and a raw sign mechanism is configured).
    std::optional<unsigned long> digest256;
    std::optional<unsigned long> digest512;
};

struct Pkcs11Options {
    std::filesystem::path modulePath; // vendor PKCS#11 library (.so / .dylib / .dll)
    // Development override, default false: skip the ownership / permission / symlink check that
    // normally precedes loading the module (the path must still be absolute and exist).
    bool allowInsecureModulePath = false;
    std::string displayName;          // shown in diagnostics and key-location text; default "PKCS#11"
    GostMechanisms gost;
    // Provider used for digests that are computed in software (ECDSA /
    // RSA fallbacks, GOST raw signing). Null: the process default provider.
    std::shared_ptr<CryptoProvider> hashProvider;
};

// Creates the store. The module is loaded lazily (first isAvailable() / list() /
// open()), so construction never blocks or fails. Without RIVET_ENABLE_PKCS11
// the returned store reports isAvailable() == false and list() / open() fail
// with BackendUnavailable.
std::shared_ptr<IdentityStore> makePkcs11IdentityStore(Pkcs11Options options);

} // namespace rivet::crypto
