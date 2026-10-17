// SPDX-License-Identifier: MPL-2.0
#pragma once

// Existing digital signatures of a document, for DISPLAY only (ADR-0025).
// Rivet does not verify signatures (no cryptography): it reports what the
// signature dictionary says and whether its byte range still describes the
// file. Everything here is plain Rivet-owned data, shareable across threads.
//
// Strings (reason, time, sub-filter) are UNTRUSTED file data: never log
// them, never interpret them beyond display.

#include "pdf/PdfPageGeometry.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace rivet::pdf {

inline constexpr std::size_t kMaxSignatures = 256;        // more are not listed
inline constexpr std::size_t kMaxSignatureByteRangeInts = 64;
inline constexpr std::size_t kMaxSignatureStringBytes = 4096;

// What a /ByteRange says about the file it is read from.
struct PdfByteRangeCoverage {
    // Well-formed: an even, non-zero count of non-negative (offset, length)
    // pairs in ascending, non-overlapping order that end within the file.
    bool valid = false;
    // Starts at 0, ends at the file size and leaves exactly one hole (the
    // /Contents string): the signature was made over this whole file.
    bool coversWholeDocument = false;
    // Ends before the file does: bytes were appended after signing (an
    // incremental update - possibly a later signature, possibly an edit).
    bool modifiedAfterSigning = false;
    std::uint64_t signedEnd = 0; // offset + length of the last range
};

// `pairs` is the raw /ByteRange (offset, length, offset, length, ...).
PdfByteRangeCoverage analyzeByteRange(std::span<const std::int64_t> pairs, std::uint64_t fileSize);

struct PdfSignatureInfo {
    // Raw /ByteRange (offset, length) pairs and its analysis.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> byteRange;
    PdfByteRangeCoverage coverage;
    std::string subFilter;   // e.g. "adbe.pkcs7.detached" (ASCII)
    std::string reason;      // /Reason, UTF-8 (may be empty)
    std::string signingTime; // /M as stored, e.g. "D:20240102030405+01'00'"
    // /DocMDP permission of a certification signature (1..3), 0 = none.
    unsigned docMdpPermission = 0;
    // The widget of the signature field when it could be located: source
    // page index and rect in that page's user space.
    std::optional<std::size_t> pageIndex;
    PdfBox widgetRect;
    // The signer's /Name and /Location are not exposed by the engine API and
    // are therefore not reported (ADR-0025, Consequences).
};

struct PdfSignatureList {
    std::vector<PdfSignatureInfo> signatures; // in /AcroForm /Fields order
    bool truncated = false;                   // more than kMaxSignatures
    bool empty() const { return signatures.empty(); }
};

using PdfSignatureListPtr = std::shared_ptr<const PdfSignatureList>;

// One line describing the state for the UI ("Signature present (not
// verified)" plus coverage), never containing file strings.
std::string signatureStatusText(const PdfSignatureInfo& info);

} // namespace rivet::pdf
