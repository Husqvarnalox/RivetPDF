// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "crypto/Validation.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto {

// Why a time-stamp token (or response) was not accepted. None = accepted.
enum class TimestampFailure : std::uint8_t {
    None,
    MalformedToken,
    BadStatus,                  // PKIStatus other than granted / grantedWithMods
    ImprintMismatch,
    NonceMismatch,
    PolicyMismatch,
    UnsupportedAlgorithm,       // the token uses an algorithm the provider cannot verify
    SignatureInvalid,           // signature, content digest or ESS signing-certificate binding failed
    NotATimestampCert,          // EKU is not (only, critical) id-kp-timeStamping, or key usage forbids signing
    UntrustedTsa,               // the TSA chain does not reach a caller-supplied trust anchor
    GenTimeOutsideCertValidity,
};

std::string_view toString(TimestampFailure);

// What verifying a signature time-stamp reports for a CMS signer.
struct SignatureTimestampInfo {
    TimestampFailure failure = TimestampFailure::None;
    TimestampStatus status = TimestampStatus::None;
    std::optional<std::chrono::system_clock::time_point> genTime;
    std::string tsaSubject; // subject of the TSA certificate that signed the token
    std::string serialHex;  // TSTInfo serial number
    std::string policy;     // TSA policy OID
    bool verified = false;  // status == Valid
    std::vector<std::string> notes;
};

} // namespace rivet::crypto
