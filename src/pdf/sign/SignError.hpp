// SPDX-License-Identifier: MPL-2.0
#pragma once

// Typed errors of the PDF signing engine (ADR-0037). Messages never contain
// secrets or document contents.

#include "crypto/Error.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rivet::pdf::sign {

enum class SignErrorCode : std::uint8_t {
    InvalidArgument,
    MalformedPdf,            // the file cannot be navigated well enough to append a revision
    LimitExceeded,           // size / nesting / count bound exceeded
    EncryptedDocument,       // /Encrypt present: signing is refused (ADR-0037)
    UnsupportedStructure,    // valid PDF, but a shape the minimal reader does not handle
    FieldNotFound,
    FieldNotEmpty,           // the named signature field is already signed
    CertificationNotAllowed, // a certification (DocMDP) signature must be the first signature
    ViolatesCertification,   // the document is certified with "no changes" (P=1)
    SignatureTooLarge,       // CMS did not fit even after the retry budget
    SelfVerifyFailed,
    Crypto,
    Io,
    Cancelled,
    Internal,
};

std::string_view toString(SignErrorCode code);

struct SignError {
    SignErrorCode code = SignErrorCode::Internal;
    std::string message;
    std::optional<crypto::CryptoErrorCode> cryptoCode;
};

template <typename T>
using SignResult = std::expected<T, SignError>;
using SignOk = SignResult<void>;

inline std::unexpected<SignError> fail(SignErrorCode code, std::string message) {
    return std::unexpected<SignError>(SignError{code, std::move(message), std::nullopt});
}

// Maps a crypto error (Cancelled stays Cancelled).
std::unexpected<SignError> failCrypto(const crypto::CryptoError& error);

} // namespace rivet::pdf::sign
