// SPDX-License-Identifier: MPL-2.0
#include "crypto/Symmetric.hpp"

// Keeps the build green without a crypto backend (RIVET_ENABLE_CRYPTO=OFF).

namespace rivet::crypto {

namespace {
std::unexpected<CryptoError> unavailable() {
    return fail(CryptoErrorCode::BackendUnavailable,
                "Rivet was built without a crypto backend (RIVET_ENABLE_CRYPTO=OFF)");
}
} // namespace

CryptoResult<Bytes> aesEncrypt(AesMode, ByteView, ByteView, ByteView, bool) { return unavailable(); }
CryptoResult<Bytes> aesDecrypt(AesMode, ByteView, ByteView, ByteView, bool) { return unavailable(); }
CryptoResult<Bytes> md5(ByteView) { return unavailable(); }

} // namespace rivet::crypto
