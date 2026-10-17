// SPDX-License-Identifier: MPL-2.0
#include "crypto/Error.hpp"

namespace rivet::crypto {

std::string_view toString(CryptoErrorCode code) {
    switch (code) {
    case CryptoErrorCode::BackendUnavailable: return "BackendUnavailable";
    case CryptoErrorCode::InvalidArgument: return "InvalidArgument";
    case CryptoErrorCode::WrongPassword: return "WrongPassword";
    case CryptoErrorCode::CorruptData: return "CorruptData";
    case CryptoErrorCode::NoPrivateKey: return "NoPrivateKey";
    case CryptoErrorCode::UnsupportedAlgorithm: return "UnsupportedAlgorithm";
    case CryptoErrorCode::WeakAlgorithm: return "WeakAlgorithm";
    case CryptoErrorCode::MalformedCms: return "MalformedCms";
    case CryptoErrorCode::LimitExceeded: return "LimitExceeded";
    case CryptoErrorCode::KeyMismatch: return "KeyMismatch";
    case CryptoErrorCode::CertificateNotFound: return "CertificateNotFound";
    case CryptoErrorCode::NotExportable: return "NotExportable";
    case CryptoErrorCode::Cancelled: return "Cancelled";
    case CryptoErrorCode::PinLocked: return "PinLocked";
    case CryptoErrorCode::DeviceRemoved: return "DeviceRemoved";
    case CryptoErrorCode::SessionExpired: return "SessionExpired";
    case CryptoErrorCode::LicenseInvalid: return "LicenseInvalid";
    case CryptoErrorCode::Io: return "Io";
    case CryptoErrorCode::Network: return "Network";
    case CryptoErrorCode::BadResponse: return "BadResponse";
    case CryptoErrorCode::Internal: return "Internal";
    }
    return "Internal";
}

core::Error toCoreError(const CryptoError& error) {
    core::ErrorCode code = core::ErrorCode::Internal;
    switch (error.code) {
    case CryptoErrorCode::BackendUnavailable: code = core::ErrorCode::NotAvailable; break;
    case CryptoErrorCode::InvalidArgument:
    case CryptoErrorCode::KeyMismatch: code = core::ErrorCode::InvalidArgument; break;
    case CryptoErrorCode::WrongPassword:
    case CryptoErrorCode::PinLocked: code = core::ErrorCode::PasswordRequired; break;
    case CryptoErrorCode::DeviceRemoved:
    case CryptoErrorCode::SessionExpired:
    case CryptoErrorCode::LicenseInvalid: code = core::ErrorCode::NotAvailable; break;
    case CryptoErrorCode::CorruptData:
    case CryptoErrorCode::NoPrivateKey:
    case CryptoErrorCode::MalformedCms:
    case CryptoErrorCode::LimitExceeded: code = core::ErrorCode::InvalidDocument; break;
    case CryptoErrorCode::UnsupportedAlgorithm:
    case CryptoErrorCode::WeakAlgorithm:
    case CryptoErrorCode::NotExportable: code = core::ErrorCode::Unsupported; break;
    case CryptoErrorCode::CertificateNotFound: code = core::ErrorCode::NotFound; break;
    case CryptoErrorCode::Cancelled: code = core::ErrorCode::Cancelled; break;
    case CryptoErrorCode::Io:
    case CryptoErrorCode::Network: code = core::ErrorCode::Io; break;
    case CryptoErrorCode::BadResponse: code = core::ErrorCode::InvalidDocument; break;
    case CryptoErrorCode::Internal: break;
    }
    return core::makeError(code, std::string(toString(error.code)) + ": " + error.message, "crypto");
}

} // namespace rivet::crypto
