// SPDX-License-Identifier: MPL-2.0
#pragma once

// Block-cipher and legacy-digest primitives needed by the PDF standard
// security handler (ADR-0046). The math is done by the crypto backend
// (OpenSSL when RIVET_ENABLE_CRYPTO=ON); without a backend every function
// reports BackendUnavailable. Keys and plaintext are never logged.

#include "crypto/Error.hpp"
#include "crypto/Types.hpp"

namespace rivet::crypto {

enum class AesMode : std::uint8_t { Cbc, Ecb };

// AES-128 (16-byte key) or AES-256 (32-byte key) encryption.
// - Cbc needs a 16-byte `iv`; Ecb ignores it.
// - pkcs7 == true appends PKCS#7 padding; pkcs7 == false requires `data` to
//   be a multiple of 16 bytes.
CryptoResult<Bytes> aesEncrypt(AesMode mode, ByteView key, ByteView iv, ByteView data, bool pkcs7);
// Inverse of aesEncrypt; with pkcs7 == true the padding is validated and removed.
CryptoResult<Bytes> aesDecrypt(AesMode mode, ByteView key, ByteView iv, ByteView data, bool pkcs7);

// MD5. Legacy: used ONLY for the key derivation and the /O /U validation
// entries of the standard security handler revisions 3 and 4. Never used for
// signatures or integrity.
CryptoResult<Bytes> md5(ByteView data);

} // namespace rivet::crypto
