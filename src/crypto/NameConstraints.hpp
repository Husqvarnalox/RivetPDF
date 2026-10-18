// SPDX-License-Identifier: MPL-2.0
#pragma once

// X.509 nameConstraints (RFC 5280 4.2.1.10, 6.1.3) for certification paths.
// Backend-free: built on Der.hpp only. Supported subtree types: dNSName,
// rfc822Name, directoryName, iPAddress and uniformResourceIdentifier. A
// constraint of any other type (otherName, x400Address, ediPartyName,
// registeredID) is understood but cannot be evaluated: a certificate that
// carries a name of such a type below it fails (fail-closed). A malformed
// nameConstraints extension, or one using minimum/maximum, is a violation.

#include "crypto/Certificate.hpp"

#include <string>
#include <vector>

namespace rivet::crypto {

enum class NameConstraintsVerdict : std::uint8_t {
    Ok,
    Violation,  // a name is excluded, or is outside every permitted subtree
    Malformed,  // a nameConstraints extension, subject or subjectAltName could not be parsed
    Unsupported // a constrained name type that cannot be evaluated is present
};

struct NameConstraintsResult {
    NameConstraintsVerdict verdict = NameConstraintsVerdict::Ok;
    std::string detail; // short, no secrets; empty when Ok
};

// True when `certDer` has a nameConstraints extension (parsed structurally).
bool hasNameConstraintsExtension(ByteView certDer);

// Applies the constraints of every CA certificate in `chain` (leaf first, the
// trust anchor last) to the certificates below it, per RFC 5280 6.1.3:
// self-issued intermediate certificates are exempt from the name check, the
// leaf (end-entity) is always checked.
NameConstraintsResult checkNameConstraints(const std::vector<CertificateInfo>& chain);

} // namespace rivet::crypto
