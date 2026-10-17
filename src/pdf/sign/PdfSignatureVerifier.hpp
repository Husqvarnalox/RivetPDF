// SPDX-License-Identifier: MPL-2.0
#pragma once

// PDF signature verifier (ADR-0037). Every signature dictionary is untrusted
// input: the /ByteRange, /Contents and CMS are validated structurally before
// any digest is computed, and the CMS is verified through rivet_crypto. The
// result is the rich crypto::ValidationResult; modification analysis of later
// revisions is conservative (a change it cannot classify is "Unknown", never
// "Permitted").

#include "crypto/Cms.hpp"
#include "pdf/sign/PdfFile.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rivet::pdf::sign {

// ISO 32000-1 section 12.8.2.2 (DocMDP) access permissions.
enum class DocMdpLevel : std::uint8_t { None = 0, NoChanges = 1, FormFill = 2, Annotate = 3 };

enum class FieldLockAction : std::uint8_t { None, All, Include, Exclude };

struct FieldLock {
    FieldLockAction action = FieldLockAction::None;
    std::vector<std::string> fields;
};

inline constexpr std::size_t kMaxSignatureContentsBytes = std::size_t{8} << 20; // 8 MiB of CMS

struct VerifyOptions {
    const crypto::TrustAnchorSet* anchors = nullptr;
    std::chrono::system_clock::time_point validationTime = std::chrono::system_clock::now();
    std::vector<crypto::Bytes> extraCertificates;
    crypto::CancelToken cancel;
};

struct PdfSignatureInfo {
    std::size_t index = 0;            // order of discovery (field tree order)
    std::string fieldName;            // empty when the signature is not referenced by a field
    std::optional<PdfRef> signatureRef; // the signature dictionary, when indirect
    std::string subFilter;
    std::string type;                 // "Sig" or "DocTimeStamp"
    std::vector<std::int64_t> byteRange;
    bool byteRangeValid = false;
    bool coversWholeFile = false;     // the signed bytes reach the end of the file
    std::uint64_t signedRevisionEnd = 0;
    std::optional<DocMdpLevel> certification; // set when this signature is the document's DocMDP signature
    std::optional<FieldLock> fieldMdp;        // /Reference FieldMDP transform, if any
    std::string reason, location, signerName, contactInfo;
    std::string signingTimeText;      // the /M entry, undecoded
    std::optional<crypto::CertificateInfo> signer;
    std::optional<crypto::DigestAlgorithm> digest;
    bool hasTimestampToken = false;
    // The signature container itself meets the PAdES B-B requirements of ETSI
    // EN 319 142-1 (ETSI.CAdES.detached, ESS signing-certificate-v2, no
    // signing-time attribute, non-weak digest). It says nothing about trust.
    bool padesBaselineB = false;
    crypto::ValidationResult validation;
};

// Verifies every signature reachable from /AcroForm /Fields and /Perms
// /DocMDP. Returns an error only when the file structure cannot be read at
// all (or the crypto backend / cancellation prevents verification); a broken
// signature is a successful result whose state says so.
SignResult<std::vector<PdfSignatureInfo>> verifyPdfSignatures(ByteView pdf, crypto::CryptoProvider& provider,
                                                              const VerifyOptions& options = {});

// Human-readable multi-line description of one signature (no secrets).
std::string describeSignature(const PdfSignatureInfo& info);

} // namespace rivet::pdf::sign
