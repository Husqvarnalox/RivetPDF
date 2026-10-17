// SPDX-License-Identifier: MPL-2.0
#pragma once

// Incremental PDF signer (ADR-0037, ADR-0038). Appends one revision holding
// the signature dictionary, the signature field/widget, the updated /AcroForm,
// /Annots and catalog entries and a new cross-reference section. The existing
// bytes are never rewritten and the document is never rebuilt, so earlier
// signatures stay byte-identical and valid.

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
};

// Signs `pdf` in memory. Refuses encrypted documents (EncryptedDocument),
// documents certified for no changes (ViolatesCertification), a second
// certification, and files over 2 GiB. The result has been self-verified.
SignResult<SignedPdf> signPdf(Bytes pdf, const SignRequest& request, crypto::CryptoProvider& provider,
                              crypto::SigningIdentity& identity);

// Transactional file signing: reads `input`, signs, writes a temporary file
// next to `output`, reads it back and verifies it (signature valid, input
// bytes unchanged), then renames atomically. Any failure leaves `output`
// (and `input`) untouched. `input` and `output` may be the same path.
SignResult<SignedPdf> signPdfFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                  const SignRequest& request, crypto::CryptoProvider& provider,
                                  crypto::SigningIdentity& identity);

} // namespace rivet::pdf::sign
