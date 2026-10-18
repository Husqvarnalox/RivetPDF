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
#include "pdf/sign/PdfLtvTypes.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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

// CPU / size budgets against hostile documents (thousands of signatures or DSS entries). Exceeding a
// budget never silently skips work: too many signatures fails the call with SignErrorCode::LimitExceeded
// and a message naming the limit; an oversized /DSS leaves LtvInfo::limitExceeded set with a reason and the
// affected signatures are not evaluated beyond B-T; an exhausted chain-building budget is reported as a
// path failure reason.
struct VerifyLimits {
    std::size_t maxSignatures = 256;          // signature fields (plus the DocMDP reference) examined per document
    std::size_t maxDssCertificates = 1024;    // /DSS + all /VRI certificates together
    std::size_t maxDssCrls = 512;
    std::size_t maxDssOcsps = 512;
    std::size_t maxChainSearchSteps = 4096;   // per chain build (ceiling 65536)
    std::size_t maxTsaCandidates = 8;         // TSA leaf candidates tried per time-stamp token
};

struct VerifyOptions {
    const crypto::TrustAnchorSet* anchors = nullptr;
    // Trust anchors for TSA chains (signature time-stamps and document
    // time-stamps). nullptr: the same set as `anchors`. The OS store is never used.
    const crypto::TrustAnchorSet* timestampAnchors = nullptr;
    std::chrono::system_clock::time_point validationTime = std::chrono::system_clock::now();
    std::vector<crypto::Bytes> extraCertificates;
    // Long-term validation (ADR-0045). The embedded /DSS and /VRI evidence is
    // always examined offline (zero network calls); the full B-LT / B-LTA
    // evaluation needs `anchors`.
    bool evaluateLtv = true;
    // true (default): B-LT needs a /VRI entry for the signature (ETSI EN 319 142-1 5.4.2).
    // false: a document-wide /DSS without a /VRI entry is accepted as evidence.
    bool requireVri = true;
    VerifyLimits limits;
    crypto::CancelToken cancel;
};

// An ordinary signature, or an RFC 3161 document time-stamp (/DocTimeStamp,
// SubFilter ETSI.RFC3161), which is reported separately and never counts as a signature
// by a person.
enum class SignatureKind : std::uint8_t { Signature, DocumentTimestamp };

std::string_view toString(SignatureKind kind);

// Long-term-validation view of one signature or document time-stamp, from the
// embedded /DSS only. Nothing here is ever fetched.
struct LtvInfo {
    bool dssPresent = false;
    bool vriPresent = false;        // a /VRI entry keyed by the SHA-1 of this signature's /Contents
    std::string vriKey;
    std::size_t vriCertificates = 0, vriOcspResponses = 0, vriCrls = 0;
    bool evaluated = false;         // path and offline revocation were evaluated
    bool signerEvidenceComplete = false;     // every non-anchor certificate of the signer chain has verified evidence
    bool timestampEvidenceComplete = false;  // same for the TSA chain of the signature time-stamp
    std::optional<std::chrono::system_clock::time_point> validationTime; // the signature time-stamp genTime
    // A valid document time-stamp, made after the revision that completed the
    // validation data, covers the /DSS (the archival B-LTA property).
    bool dssCoveredByTimestamp = false;
    bool limitExceeded = false;     // the /DSS is larger than VerifyOptions::limits allow: no LTV evaluation was done
    std::optional<std::chrono::system_clock::time_point> coveringTimestampTime;
    std::vector<std::string> missing; // certificates without evidence, path problems
    std::vector<std::string> reasons; // why the achieved level is not higher
    std::vector<std::string> limitations;
};

// Raw material the LTV collector needs (filled for every signature and
// document time-stamp whose /Contents parsed).
struct LtvMaterial {
    crypto::Bytes contentsDer;                    // /Contents, zero padding trimmed
    std::vector<crypto::Bytes> cmsCertificates;   // certificates embedded in the signature CMS
    std::vector<crypto::Bytes> tokenCertificates; // certificates embedded in the time-stamp token
};

struct PdfSignatureInfo {
    std::size_t index = 0;            // order of discovery (field tree order)
    std::string fieldName;            // empty when the signature is not referenced by a field
    std::optional<PdfRef> signatureRef; // the signature dictionary, when indirect
    std::string subFilter;
    std::string type;                 // "Sig" or "DocTimeStamp"
    SignatureKind kind = SignatureKind::Signature;
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
    // B-B container plus a signature time-stamp that verified (trusted TSA,
    // imprint over this signature value): PAdES Baseline B-T. Needs TSA anchors.
    bool padesBaselineT = false;
    // Embedded signature time-stamp (kind == Signature), or the token of a
    // document time-stamp (kind == DocumentTimestamp): genTime, TSA subject,
    // failure reason and status.
    std::optional<crypto::SignatureTimestampInfo> timestamp;
    crypto::ValidationResult validation;
    // The honest PAdES baseline level reached, with reasons in `ltv.reasons`.
    // Meaningful for kind == Signature; document time-stamps report NotPades.
    PadesLevel level = PadesLevel::NotPades;
    LtvInfo ltv;
    LtvMaterial material;
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
