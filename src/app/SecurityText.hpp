// SPDX-License-Identifier: MPL-2.0
#pragma once

// User-facing wording for the digital-signature and Set Password UI (ADR-0048).
// Pure functions, no widgets: everything here is unit-tested because the
// wording carries legal weight (evidence, never a legal conclusion) and must
// never overstate what Rivet verified.

#include "core/Error.hpp"
#include "crypto/Certificate.hpp"
#include "crypto/Qualification.hpp"
#include "crypto/Validation.hpp"
#include "pdf/sign/PdfLtvTypes.hpp"
#include "pdf/sign/PdfSignatureVerifier.hpp"
#include "pdf/sign/SignError.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rivet::app {

enum class Severity : std::uint8_t { Good, Warning, Bad, Unknown };

// "2026-01-31 14:05 UTC" (portable, no locale / tz database).
std::string formatUtc(std::chrono::system_clock::time_point time);

// Short verifier verdict ("Valid, trusted signer") and its severity.
std::string signatureStateTitle(crypto::SignatureState state);
Severity signatureStateSeverity(crypto::SignatureState state);

// "B-B", "B-T", "B-LT", "B-LTA" ("not PAdES" otherwise) and the long label.
std::string levelShortName(pdf::sign::PadesLevel level);
std::string levelLabel(pdf::sign::PadesLevel level);
bool levelNeedsTimestamp(pdf::sign::PadesLevel level);  // B-T, B-LT, B-LTA
bool levelNeedsNetwork(pdf::sign::PadesLevel level);    // time-stamp and/or revocation fetches

// Evidence-based label for a classification. QualifiedEvidence is worded as
// technical evidence, never as "qualified signature"/QES. `assurance` guards
// the wording: a software store or a remote/cloud service can never produce
// the qualified-evidence wording, whatever the classification says.
std::string qualificationLabel(const crypto::SignatureClassification& classification,
                               crypto::ProviderAssurance assurance);

// One-line summary of a certificate: "Name (Organisation), issued by CA, valid until 2027-01-01".
std::string certificateSummary(const crypto::CertificateInfo& certificate, std::chrono::system_clock::time_point now);

// Everything the validation panel shows for one signature.
struct SignatureRowModel {
    std::size_t index = 0;
    std::string field;           // field name, "" when unknown
    std::string heading;         // "Signature 1: Jane Doe" / "Document time-stamp"
    std::string stateTitle;
    Severity severity = Severity::Unknown;
    std::string signer;          // certificate summary, "" when absent
    std::string signingTime;     // /M text or ""
    std::string timestamp;       // "Time-stamp: valid, 2026-.. by TSA", "No signature time-stamp", ...
    std::string level;           // levelLabel
    std::string qualification;   // evidence-based label (see qualificationLabel)
    std::string disclaimer;      // crypto::kLegalDisclaimer
    std::vector<std::string> notes; // verifier notes, ltv gaps (no secrets)
};

SignatureRowModel makeSignatureRow(const pdf::sign::PdfSignatureInfo& info, const crypto::AccreditedCaList* list,
                                   std::chrono::system_clock::time_point now);

// Error wording. None of these include secrets or document contents.
std::string describeSignError(const pdf::sign::SignError& error);
std::string describeEncryptError(const core::Error& error);

} // namespace rivet::app
