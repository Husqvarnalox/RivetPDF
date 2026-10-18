// SPDX-License-Identifier: MPL-2.0
#pragma once

// Standard security handler primitives (ISO 32000-1 7.6.3, ISO 32000-2 7.6.4):
// password preparation, key derivation, /O /U /OE /UE /Perms construction and
// per-object data encryption. Exposed (rather than file-local) so the tests
// can check the algorithms; applications use encryptPdf().

#include "crypto/Types.hpp"
#include "pdf/encrypt/PdfEncrypt.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace rivet::pdf::encrypt {

using Bytes = crypto::Bytes;
using ByteView = crypto::ByteView;

// R6 password: UTF-8, SASLprep-lite (non-ASCII spaces -> U+0020, "mapped to
// nothing" characters removed, control characters rejected, no NFKC), then
// truncated to 127 bytes on a code point boundary.
core::Result<Bytes> prepareR6Password(std::string_view utf8);
// R4 password: PDFDocEncoding approximated as Latin-1 (every code point must
// be below U+0100), at most 32 bytes.
core::Result<Bytes> prepareR4Password(std::string_view utf8);

// ISO 32000-2 Algorithm 2.B (the R6 hash). Returns 32 bytes.
core::Result<Bytes> hashR6(ByteView password, ByteView salt, ByteView userKeyForOwner);

// The /P value for `permissions` (reserved bits set as the standard requires).
std::int32_t permissionBits(const Permissions& permissions);

struct EncryptionSetup {
    EncryptionSetup() = default;
    EncryptionSetup(const EncryptionSetup&) = default;
    EncryptionSetup(EncryptionSetup&&) noexcept = default;
    EncryptionSetup& operator=(const EncryptionSetup&) = default;
    EncryptionSetup& operator=(EncryptionSetup&&) noexcept = default;
    // Zeroes the file key (the only secret member) when the setup dies.
    ~EncryptionSetup();

    Cipher cipher = Cipher::Aes256;
    bool encryptMetadata = true;
    std::int32_t p = 0;
    Bytes fileKey; // 32 bytes (R6) or 16 bytes (R4)
    Bytes o;       // 48 (R6) or 32 (R4) bytes
    Bytes u;       // 48 (R6) or 32 (R4) bytes
    Bytes oe;      // R6 only: 32 bytes
    Bytes ue;      // R6 only: 32 bytes
    Bytes perms;   // R6 only: 16 bytes
};

// Builds a fresh setup (random file key, salts and IVs). `id0` is the first
// element of the trailer /ID (used by R4 only).
// Uses options.userPassword / options.ownerPassword.
core::Result<EncryptionSetup> buildSetup(const EncryptOptions& options, ByteView id0);
// Same, but takes the passwords separately (UTF-8) so that callers holding them in a
// SecretBuffer need not put them into std::string fields. options' password fields are ignored.
core::Result<EncryptionSetup> buildSetup(const EncryptOptions& options, std::string_view userPassword,
                                         std::string_view ownerPassword, ByteView id0);

// The /Encrypt dictionary text ("<< ... >>") for the setup.
std::string encryptDictionary(const EncryptionSetup& setup);

// Encrypts one string or stream body of indirect object (num, gen): a fresh
// random 16-byte IV followed by AES-CBC/PKCS#7 ciphertext.
core::Result<Bytes> encryptObjectData(const EncryptionSetup& setup, std::uint32_t num, std::uint32_t gen,
                                      ByteView plain);

// Per-object key for R4 (ISO 32000-1 Algorithm 1); the file key itself for R6.
core::Result<Bytes> objectKey(const EncryptionSetup& setup, std::uint32_t num, std::uint32_t gen);

} // namespace rivet::pdf::encrypt
