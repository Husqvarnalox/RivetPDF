// SPDX-License-Identifier: MPL-2.0
#pragma once

// Post-save PDF encryption with the standard security handler (ADR-0046).
//
// PDFium opens encrypted documents but never writes them. This module takes
// the BYTES of a PDFium save and emits the same document encrypted. The input
// is deliberately constrained to what PDFium writes: a single classic xref
// table, no object streams, no existing /Encrypt, no signatures. Anything else
// is refused with a clear error (ErrorCode::Unsupported). Signed documents
// are refused because encrypting rewrites every object and would invalidate
// the signature's /ByteRange digest; sign AFTER encrypting instead.
//
// Output: AES-256 (V5/R6, ISO 32000-2) by default, or AES-128 (V4/R4, AESV2)
// for older readers. No RC4 content encryption, no public-key handlers.

#include "core/Error.hpp"
#include "crypto/SecretBuffer.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace rivet::pdf::encrypt {

enum class Cipher : std::uint8_t {
    Aes256, // V5 / R6, AESV3, 256-bit file key (default)
    Aes128, // V4 / R4, AESV2, 128-bit file key (compatibility)
};

// What a user who opened the file with the USER password may do. The owner
// password always grants everything.
struct Permissions {
    bool print = true;            // bit 3
    bool modify = true;           // bit 4
    bool copy = true;             // bit 5 (copy / extract text and graphics)
    bool annotate = true;         // bit 6 (add / modify annotations)
    bool fillForms = true;        // bit 9
    bool accessibility = true;    // bit 10 (extraction for assistive technology)
    bool assemble = true;         // bit 11 (insert / delete / rotate pages)
    bool printHighQuality = true; // bit 12 (only meaningful together with print)

    bool allAllowed() const {
        return print && modify && copy && annotate && fillForms && accessibility && assemble && printHighQuality;
    }
};

struct EncryptOptions {
    Cipher cipher = Cipher::Aes256;
    // UTF-8. May be empty (the file then opens without a password but still
    // enforces the permissions for readers that honour them).
    // Prefer the EncryptSecrets overload of encryptPdf(): these std::string
    // fields are the caller's copies, which this module can neither wipe nor
    // prevent from being copied (SSO / reallocation leftovers). They are never
    // copied by the module itself. See ADR-0046 "secret handling".
    std::string userPassword;
    // UTF-8. Optional: when empty a random owner password is generated and
    // discarded, so nobody can lift the permissions.
    std::string ownerPassword;
    Permissions permissions;
    // false: the XMP /Metadata stream stays readable by tools that index
    // files without a password (writes /EncryptMetadata false).
    bool encryptMetadata = true;
};

// Encrypts `pdf`. Errors: InvalidArgument (bad options), InvalidDocument
// (malformed input), Unsupported (encrypted / signed / object streams /
// incremental updates / other constructs outside the supported subset),
// NotAvailable (built without RIVET_ENABLE_CRYPTO).
core::Result<std::vector<std::uint8_t>> encryptPdf(std::span<const std::uint8_t> pdf, const EncryptOptions& options);

// Passwords held in wiping buffers. When used, options.userPassword and
// options.ownerPassword are ignored. Everything the module derives from them
// (prepared passwords, intermediate hashes, the file key, per-object keys) is
// zeroed on all paths, including errors. Unavoidable leftovers: the crypto
// backend's own internal state (OpenSSL contexts), and the output PDF holds
// only public values (/O /U /OE /UE /Perms).
struct EncryptSecrets {
    crypto::SecretBuffer userPassword;
    crypto::SecretBuffer ownerPassword;
};
core::Result<std::vector<std::uint8_t>> encryptPdf(std::span<const std::uint8_t> pdf, const EncryptOptions& options,
                                                   const EncryptSecrets& secrets);

} // namespace rivet::pdf::encrypt
