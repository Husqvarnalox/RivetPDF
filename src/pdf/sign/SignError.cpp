// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/SignError.hpp"

namespace rivet::pdf::sign {

std::string_view toString(SignErrorCode code) {
    switch (code) {
    case SignErrorCode::InvalidArgument: return "invalid argument";
    case SignErrorCode::MalformedPdf: return "malformed PDF";
    case SignErrorCode::LimitExceeded: return "limit exceeded";
    case SignErrorCode::EncryptedDocument: return "encrypted document";
    case SignErrorCode::UnsupportedStructure: return "unsupported structure";
    case SignErrorCode::FieldNotFound: return "signature field not found";
    case SignErrorCode::FieldNotEmpty: return "signature field already signed";
    case SignErrorCode::CertificationNotAllowed: return "certification signature not allowed";
    case SignErrorCode::ViolatesCertification: return "document certified, no changes permitted";
    case SignErrorCode::SignatureTooLarge: return "signature too large";
    case SignErrorCode::ContentsPlaceholderTooSmall: return "contents placeholder too small";
    case SignErrorCode::TimestampFailed: return "timestamp failed";
    case SignErrorCode::SelfVerifyFailed: return "self-verification failed";
    case SignErrorCode::Crypto: return "crypto error";
    case SignErrorCode::Io: return "I/O error";
    case SignErrorCode::Cancelled: return "cancelled";
    case SignErrorCode::Internal: return "internal error";
    }
    return "?";
}

std::unexpected<SignError> failCrypto(const crypto::CryptoError& error) {
    SignError e;
    e.code = error.code == crypto::CryptoErrorCode::Cancelled ? SignErrorCode::Cancelled : SignErrorCode::Crypto;
    e.message = std::string(crypto::toString(error.code)) + ": " + error.message;
    e.cryptoCode = error.code;
    return std::unexpected<SignError>(std::move(e));
}

} // namespace rivet::pdf::sign
