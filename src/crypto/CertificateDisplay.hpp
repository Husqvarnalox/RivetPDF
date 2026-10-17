// SPDX-License-Identifier: MPL-2.0
#pragma once

// Pure-data display model of a certificate for pickers and signature panels.
// Only attributes that are actually present in the certificate are filled; an
// absent attribute stays std::nullopt (nothing is invented or defaulted). No
// legal claim is made here: see Qualification.hpp for classification.

#include "crypto/Certificate.hpp"
#include "crypto/IdentityStore.hpp"
#include "crypto/X509Parse.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace rivet::crypto {

struct CertificateDisplay {
    std::optional<std::string> subjectName;  // CN
    // "<surname> <givenName>" (Russian convention: the given name attribute
    // usually holds name and patronymic). Only the parts that are present.
    std::optional<std::string> personName;
    std::optional<std::string> organization; // O
    std::optional<std::string> title;        // 2.5.4.12
    std::optional<std::string> country;      // 2.5.4.6
    std::optional<std::string> locality;     // 2.5.4.7
    std::optional<std::string> region;       // 2.5.4.8
    std::optional<std::string> email;
    std::optional<std::string> inn;
    std::optional<std::string> snils;
    std::optional<std::string> ogrn;
    std::optional<std::string> ogrnip;
    std::optional<std::string> innLe;
    std::optional<std::string> issuerName;         // issuer CN, else issuer O
    std::optional<std::string> issuerOrganization; // issuer O

    std::chrono::system_clock::time_point validFrom{};
    std::chrono::system_clock::time_point validTo{};
    bool expired = false;     // now > validTo
    bool notYetValid = false; // now < validFrom

    std::optional<std::string> serialHex;
    std::string sha256Fingerprint;     // empty when the certificate was parsed without a digest
    std::string keyAlgorithmLabel;     // "GOST R 34.10-2012 256", "RSA 2048", "EC P-256"; empty when unknown

    KeyLocation location;              // where the private key lives (Unknown without a descriptor)
    bool privateKeyAvailable = false;

    std::vector<std::string> certificatePolicyOids;
};

// `extras` supplies the attribute lists and policy OIDs; when null they are
// obtained by parsing `cert.der` with parseX509 (a certificate that does not
// parse leaves those fields empty). `location` may be null.
CertificateDisplay makeCertificateDisplay(const CertificateInfo& cert, const X509Extras* extras, const KeyLocation* location,
                                          bool privateKeyAvailable, std::chrono::system_clock::time_point now);

CertificateDisplay makeCertificateDisplay(const IdentityDescriptor& identity, const X509Extras* extras,
                                          std::chrono::system_clock::time_point now);

} // namespace rivet::crypto
