// SPDX-License-Identifier: MPL-2.0
#pragma once

// Signature classification: collects technical EVIDENCE about a signing
// certificate and its key custody and sorts the signature into a descriptive
// class. It is NOT a legal conclusion. The legal status of an electronic
// signature (including whether it is a qualified signature, KEP/КЭП) is
// determined by law and the trust framework, never by this software and never
// by an algorithm OID alone. `QualifiedEvidence` therefore only means "every
// technical indicator Rivet can check is present"; the UI must word it as
// evidence, and must show kLegalDisclaimer next to it.
//
// Rivet ships NO list of accredited certification authorities. The list is a
// text file loaded from a configurable path by the application and replaceable
// without rebuilding (see AccreditedCaList for the format).

#include "crypto/Certificate.hpp"
#include "crypto/Error.hpp"
#include "crypto/IdentityStore.hpp"
#include "crypto/Types.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto {

enum class SignatureClass : std::uint8_t {
    Unknown,              // nothing is known about the key / algorithm / certificate
    SimpleCrypto,         // technically valid signature with a non-GOST algorithm, no identity evidence
    GostDigitalSignature, // GOST R 34.10-2012 algorithm, nothing more
    AdvancedUnqualified,  // identity attributes + signature key usage + non-exportable key + valid
    QualifiedEvidence,    // all technical indicators of a qualified signature are present (not a legal finding)
};

std::string_view toString(SignatureClass);   // short human-readable label (English, for display)
std::string_view machineKey(SignatureClass); // stable key for storage / logs: "unknown", "simple_crypto", ...

inline constexpr std::string_view kLegalDisclaimer =
    "Legal status of an electronic signature is determined by law and the trust framework, not by this software.";

struct AccreditedCaEntry {
    std::string sha256Fingerprint; // lower-case hex of the CA certificate DER
    Bytes subjectKeyId;            // optional
    std::string name;              // free text, display only
};

// Line-based text format (UTF-8, LF or CRLF, at most 1 MiB and 100000 lines):
//
//   rivet-accredited-ca-list 1        <- first meaningful line, exactly this
//   # comment                         <- whole-line comments and blank lines are ignored
//   updated 2026-01-31                <- optional, once, YYYY-MM-DD
//   source <free text>                <- optional, once
//   ca sha256=<64 hex> [ski=<hex>] name=<rest of line>
//   policy <dotted OID>               <- certificate policy that marks a qualified certificate
//   csp <provider-id>                 <- provider declared as a certified cryptographic means, e.g. cryptopro
//
// Parsing is strict: an unknown directive or a malformed line is an error that
// names the line. `ca` lines with the same fingerprint are merged. A missing
// file is CryptoErrorCode::Io; malformed content is CorruptData; size bounds
// give LimitExceeded.
class AccreditedCaList {
public:
    static CryptoResult<AccreditedCaList> parse(std::string_view text);
    static CryptoResult<AccreditedCaList> loadFile(const std::filesystem::path& path);

    static constexpr std::size_t kMaxBytes = std::size_t{1} << 20;
    static constexpr std::size_t kMaxLines = 100000;

    int formatVersion = 0;
    std::string updated;
    std::string source;
    std::vector<AccreditedCaEntry> entries;
    std::set<std::string> qualifiedPolicyOids;
    std::set<std::string> certifiedCspProviderIds;

    // True when the certificate is listed by SHA-256 fingerprint or by a
    // non-empty subject key identifier. SKI matching is advisory (a key
    // identifier is not a secret); chain signatures are validated elsewhere.
    bool matchesIssuer(const CertificateInfo& issuerCert) const;
    bool qualifiedPolicy(std::string_view oid) const { return qualifiedPolicyOids.contains(std::string(oid)); }
};

struct ClassificationInput {
    std::optional<PublicKeyType> keyType;        // key type when known without a certificate
    std::optional<SignatureAlgorithm> signature; // algorithm actually used / offered
    const CertificateInfo* signer = nullptr;     // signing certificate, may be null
    std::vector<CertificateInfo> chain;          // candidate issuers, any order; the issuer is found by subject name
    ProviderAssurance provider = ProviderAssurance::Software;
    std::string providerId;                      // "cryptopro", "pkcs11", ... matched against the list's csp lines
    const AccreditedCaList* list = nullptr;      // nullable: without a list QualifiedEvidence is never reached
    std::chrono::system_clock::time_point validAt = std::chrono::system_clock::now();
};

struct SignatureClassification {
    struct Evidence {
        bool gostAlgorithm = false;
        bool identityAttributes = false;       // CN plus at least one of INN / SNILS / OGRN / OGRNIP
        bool signatureKeyUsage = false;        // digitalSignature or nonRepudiation
        bool nonExportableKeyCustody = false;  // provider is not a software store
        bool issuerInAccreditedList = false;   // immediate issuer linked by AKI->SKI and listed
        bool qualifiedPolicyPresent = false;   // a certificate policy OID listed as qualified
        bool certifiedCspDeclared = false;     // CertifiedCspDeclared provider whose id the list names
        bool certificateValidAtTime = false;
    };

    SignatureClass level = SignatureClass::Unknown;
    std::vector<std::string> reasons; // short statements of evidence found or missing
    Evidence evidence;
    std::string disclaimer{kLegalDisclaimer};
};

SignatureClassification classify(const ClassificationInput& input);

} // namespace rivet::crypto
