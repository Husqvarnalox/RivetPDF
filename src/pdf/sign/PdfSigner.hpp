// SPDX-License-Identifier: MPL-2.0
#pragma once

// Incremental PDF signer (ADR-0037, ADR-0038). Appends one revision holding
// the signature dictionary, the signature field/widget, the updated /AcroForm,
// /Annots and catalog entries and a new cross-reference section. The existing
// bytes are never rewritten and the document is never rebuilt, so earlier
// signatures stay byte-identical and valid.

#include "crypto/timestamp/TimeStamp.hpp"
#include "pdf/sign/PdfSignatureVerifier.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace rivet::pdf::sign {

enum class SignatureProfile : std::uint8_t {
    // /SubFilter adbe.pkcs7.detached, CMS with signing-time.
    Pkcs7Detached,
    // /SubFilter ETSI.CAdES.detached, CAdES-BES with ESS signing-certificate-v2
    // and no signing-time attribute: PAdES Baseline B-B (ETSI EN 319 142-1).
    PadesBaselineB,
};

struct SignaturePlacement {
    enum class Kind : std::uint8_t {
        Invisible,     // new field, empty rectangle on page 0 (or `pageIndex`)
        NewVisible,    // new field with an appearance in `rect` on `pageIndex`
        ExistingField, // fill the empty signature field named `fieldName`
    };
    Kind kind = Kind::Invisible;
    std::size_t pageIndex = 0;
    std::array<double, 4> rect{0, 0, 0, 0}; // x0 y0 x1 y1 in default user space (NewVisible)
    std::string fieldName;                  // existing field, or the name of the new one (default "SignatureN")
};

// Obtains an RFC 3161 token for `digest` (blocking; never call it on the UI
// thread). The signer itself has no network code: callers adapt
// crypto::TimeStampClient::request (which verifies the token against the
// caller's TSA anchors and goes through rivet::net) or a test double. Only a
// result whose verification.ok() is true is embedded; a transport error or a
// rejected token is a timestamp failure.
using TimestampFetcher = std::function<crypto::CryptoResult<crypto::TimeStampVerification>(
    crypto::DigestAlgorithm algorithm, crypto::ByteView digest, const crypto::CancelToken& cancel)>;

// Default /Contents room added for an RFC 3161 token (TSA chain included).
inline constexpr std::size_t kDefaultTimestampTokenReserve = 8192;

// PAdES baseline level actually reached by a produced signature.
enum class PadesLevel : std::uint8_t {
    NotPades,   // adbe.pkcs7.detached
    BaselineB,  // ETSI.CAdES.detached, no signature time-stamp
    BaselineT,  // ... plus a signature time-stamp (id-aa-signatureTimeStampToken)
};

std::string_view toString(PadesLevel level);

// PAdES B-T / signature time-stamp step of signPdf (ADR-0038).
struct SignatureTimestampOptions {
    // Empty: no time-stamp (B-B). Otherwise the token is requested for the hash
    // of the signerInfo signature value, once per signing attempt.
    TimestampFetcher fetch;
    crypto::DigestAlgorithm digest = crypto::DigestAlgorithm::Sha256; // messageImprint algorithm
    // Estimate of the token size in bytes added to the /Contents reservation
    // (0: kDefaultTimestampTokenReserve).
    std::size_t expectedTokenBytes = 0;
    // When the final CMS does not fit the reservation: rebuild once with the
    // size now known (this costs a second TSA request). false: fail at once.
    bool retryWithLargerReservation = true;
    // false (default): a failing TSA fails the whole signing and nothing is
    // produced. true: sign without a time-stamp; the result is B-B and says why.
    bool allowSignWithoutTimestamp = false;
};

struct SignRequest {
    SignatureProfile profile = SignatureProfile::PadesBaselineB;
    crypto::DigestAlgorithm digest = crypto::DigestAlgorithm::Sha256; // SHA-256/384/512 (creation policy)
    std::optional<crypto::SignatureAlgorithm> signature;              // default: by key type and digest
    SignaturePlacement placement;
    std::string reason, location, signerName, contactInfo; // optional /Reason /Location /Name /ContactInfo
    // Certification signature (/Reference DocMDP + /Perms). Only valid as the
    // first signature of the document.
    DocMdpLevel certification = DocMdpLevel::None;
    // Field lock (ISO 32000-1 12.7.4.5): written as the signature field's /Lock
    // dictionary and as a /FieldMDP transform in /Reference.
    std::optional<FieldLock> fieldLock;
    std::chrono::system_clock::time_point signingTime = std::chrono::system_clock::now(); // /M
    // Visible text of a NewVisible field (Latin-1 only; other characters become '?').
    // Empty: derived from the signer name, time, reason and location.
    std::vector<std::string> appearanceLines;
    bool includeChain = true;
    // Extension points for later PAdES levels (B-T/B-LT/B-LTA): extra bytes
    // of /Contents space for a timestamp token, and a hook that may add
    // unsigned attributes (signature-time-stamp) to the CMS before encoding.
    // The hook runs on every attempt.
    std::size_t extraReserveBytes = 0;
    std::function<crypto::CryptoOk(crypto::CmsSignedData&)> finalizeCms;
    // Testing / tuning: when non-zero, the exact initial /Contents capacity in bytes.
    std::size_t initialReserveOverride = 0;
    SignatureTimestampOptions timestamp;
    crypto::CancelToken cancel;
};

struct SignedPdf {
    Bytes data;                  // the complete signed file; the input is its exact prefix
    std::size_t inputSize = 0;
    PdfRef signatureRef;
    std::string fieldName;
    std::size_t attempts = 1;    // 1 + placeholder-overflow retries
    std::size_t reservedBytes = 0; // final /Contents capacity
    std::size_t cmsBytes = 0;
    bool classicXref = true;     // the new revision uses a cross-reference table (else a stream)
    // Timestamps (signPdf with a timestamp step, or timestampPdf).
    PadesLevel level = PadesLevel::NotPades;
    bool timestamped = false;            // a verified RFC 3161 token is embedded
    std::size_t timestampTokenBytes = 0;
    std::string timestampSkippedReason;  // signing went ahead without a time-stamp (allowSignWithoutTimestamp)
    std::optional<std::chrono::system_clock::time_point> timestampGenTime;
    std::string timestampTsaSubject;
};

// Signs `pdf` in memory. Refuses encrypted documents (EncryptedDocument),
// documents certified for no changes (ViolatesCertification), a second
// certification, and files over 2 GiB. The result has been self-verified.
SignResult<SignedPdf> signPdf(Bytes pdf, const SignRequest& request, crypto::CryptoProvider& provider,
                              crypto::SigningIdentity& identity);

// Adds an RFC 3161 document time-stamp (ETSI EN 319 142-1 5.4.3, ISO 32000-2
// 12.8.5) as one more incremental revision: an invisible signature field whose
// dictionary is /Type /DocTimeStamp /SubFilter /ETSI.RFC3161 and whose
// /Contents is the raw TimeStampToken. The digest sent to the TSA is the hash
// of the /ByteRange bytes. Allowed after any signature, including a
// certification signature; it can never precede one (a certification must be
// the first signature). Encrypted documents and files over 2 GiB are refused.
struct DocTimestampRequest {
    TimestampFetcher fetch; // required
    crypto::DigestAlgorithm digest = crypto::DigestAlgorithm::Sha256;
    std::size_t expectedTokenBytes = 0; // 0: kDefaultTimestampTokenReserve
    bool retryWithLargerReservation = true;
    // Only Invisible (new field) or ExistingField are accepted; `fieldName` names the field.
    SignaturePlacement placement;
    std::size_t initialReserveOverride = 0; // testing: exact /Contents capacity
    crypto::CancelToken cancel;
};

// The result reuses SignedPdf: timestampTokenBytes / cmsBytes hold the token
// size, level is NotPades and timestamped is true.
SignResult<SignedPdf> timestampPdf(Bytes pdf, const DocTimestampRequest& request, crypto::CryptoProvider& provider);
SignResult<SignedPdf> timestampPdfFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                       const DocTimestampRequest& request, crypto::CryptoProvider& provider);

// Transactional file signing: reads `input`, signs, writes a temporary file
// next to `output`, reads it back and verifies it (signature valid, input
// bytes unchanged), then renames atomically. Any failure leaves `output`
// (and `input`) untouched. `input` and `output` may be the same path.
SignResult<SignedPdf> signPdfFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                  const SignRequest& request, crypto::CryptoProvider& provider,
                                  crypto::SigningIdentity& identity);

} // namespace rivet::pdf::sign
