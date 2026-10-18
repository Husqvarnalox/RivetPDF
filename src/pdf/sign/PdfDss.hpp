// SPDX-License-Identifier: MPL-2.0
#pragma once

// Document Security Store (ISO 32000-2 12.8.4.3, ETSI EN 319 142-1 5.4.2):
// reader and incremental writer of the catalog /DSS dictionary with its
// /Certs, /OCSPs, /CRLs arrays of raw-DER streams and the per-signature /VRI
// dictionaries (ADR-0045). The reader treats everything as hostile and never
// fails: malformed parts are skipped and listed in `problems`.

#include "crypto/Providers.hpp"
#include "pdf/sign/PdfFile.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rivet::pdf::sign {

inline constexpr std::size_t kMaxDssItems = 4096;               // streams per array / VRI entries
inline constexpr std::size_t kMaxDssItemBytes = std::size_t{8} << 20;
inline constexpr std::size_t kMaxDssTotalBytes = std::size_t{64} << 20;

struct DssItem {
    PdfRef ref;
    Bytes data; // decoded stream bytes (raw DER)
};

struct DssEvidence {
    std::vector<DssItem> certs, ocsps, crls;
    bool empty() const { return certs.empty() && ocsps.empty() && crls.empty(); }
};

struct VriEntry {
    std::string key; // uppercase hex SHA-1 of the signature /Contents
    DssEvidence evidence;
    std::string tu;  // /TU as text, empty when absent
    bool hasTs = false;
};

struct DssData {
    bool present = false;       // the catalog has a /DSS dictionary
    DssEvidence global;         // /Certs /OCSPs /CRLs
    std::map<std::string, VriEntry> vri;
    std::vector<std::string> problems; // malformed parts that were skipped (bounded)
};

// Reads the /DSS as of cross-reference section `view` (0: newest).
DssData readDss(PdfFile& file, std::size_t view = 0);

// Uppercase hex SHA-1 of `contentsDer` (the signature /Contents CMS DER, padding trimmed).
SignResult<std::string> vriKey(crypto::CryptoProvider& provider, ByteView contentsDer);

struct DssEvidenceBytes {
    std::vector<Bytes> certs, ocsps, crls;
};

struct VriAddition {
    Bytes signatureContents; // /Contents DER of the signature (or document time-stamp token)
    DssEvidenceBytes evidence;
    std::chrono::system_clock::time_point tu = std::chrono::system_clock::now();
};

struct DssAdditions {
    // Global material not tied to a VRI entry; evidence of every VriAddition is
    // added to the global arrays too.
    DssEvidenceBytes global;
    std::vector<VriAddition> vri;
};

struct DssWriteResult {
    Bytes data;                 // the new file; identical to the input when !changed
    bool changed = false;
    std::size_t newStreams = 0; // evidence streams written
    std::size_t reusedStreams = 0; // evidence already present (deduplicated by SHA-256)
};

// Appends one incremental revision that creates or extends the catalog /DSS.
// Existing entries are always kept (merge, never drop); evidence is
// deduplicated by SHA-256, so repeating an upgrade appends nothing. Refuses
// encrypted documents (EncryptedDocument) and a catalog or /DSS that cannot
// be rewritten faithfully (UnsupportedStructure).
SignResult<DssWriteResult> appendDss(Bytes pdf, const DssAdditions& additions, crypto::CryptoProvider& provider);

} // namespace rivet::pdf::sign
