// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "core/Error.hpp"

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace rivet::crypto {

// Typed failure kinds of the crypto subsystem. Messages never contain
// secrets, key material or document contents.
enum class CryptoErrorCode : std::uint8_t {
    BackendUnavailable, // built without a crypto backend (RIVET_ENABLE_CRYPTO=OFF)
    InvalidArgument,
    WrongPassword,      // PKCS#12 / key password rejected
    CorruptData,        // structurally broken container (PKCS#12, certificate, key)
    NoPrivateKey,       // container holds no usable private key
    UnsupportedAlgorithm,
    WeakAlgorithm,      // refused by creation policy (SHA-1, RSA < 2048, ...)
    MalformedCms,       // CMS / DER structure is invalid
    LimitExceeded,      // size / nesting bound exceeded
    KeyMismatch,        // key does not fit the requested algorithm / certificate
    CertificateNotFound,
    NotExportable,
    Cancelled,
    Io,
    Internal,
};

std::string_view toString(CryptoErrorCode code);

struct CryptoError {
    CryptoErrorCode code = CryptoErrorCode::Internal;
    std::string message;

    bool operator==(const CryptoError&) const = default;
};

template <typename T>
using CryptoResult = std::expected<T, CryptoError>;

using CryptoOk = CryptoResult<void>;

inline std::unexpected<CryptoError> fail(CryptoErrorCode code, std::string message) {
    return std::unexpected<CryptoError>(CryptoError{code, std::move(message)});
}

// Bridge to the shared core error type ("crypto: <code>: <message>").
core::Error toCoreError(const CryptoError& error);

} // namespace rivet::crypto
