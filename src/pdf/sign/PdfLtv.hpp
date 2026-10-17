// SPDX-License-Identifier: MPL-2.0
#pragma once

// Long-term validation for PAdES (ADR-0045): evidence collection, the one-shot
// B-LT / B-LTA flow behind SignRequest::targetLevel, standalone upgrades and
// archive time-stamp renewal. The embedded /DSS is written by PdfDss.hpp and
// judged by the verifier (PdfSignatureVerifier.hpp), offline only.
//
// Network access exists only here, only for evidence collection, and only
// through rivet::net via LtvOptions::network (Off / Ask / On gate and SSRF
// policy). Verification never calls the network.

#include "crypto/revocation/AiaFetcher.hpp"
#include "pdf/sign/PdfDss.hpp"
#include "pdf/sign/PdfSigner.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace rivet::pdf::sign {

// Evidence gathered for one signature (or document time-stamp).
struct LtvEvidence {
    DssEvidenceBytes evidence;            // certificates of the chains, OCSP responses, CRLs (raw DER, deduplicated)
    std::vector<std::string> missing;     // certificates without usable evidence, path problems
    std::vector<std::string> notes;       // AIA fetch notes, limitations
    bool signerComplete = false;
    bool timestampComplete = false;
    bool complete() const { return timestampComplete && (signerComplete || !hasSigner); }
    bool hasSigner = true;                // false for a document time-stamp
};

// What the collector needs to know about one signature.
struct LtvSubject {
    std::optional<crypto::CertificateInfo> signer;   // nullopt for a document time-stamp
    std::vector<Bytes> cmsCertificates;              // certificates embedded in the signature CMS
    std::vector<Bytes> tokenCertificates;            // certificates embedded in its time-stamp token
    std::string tsaSubject;                          // subject of the time-stamp authority certificate
    std::chrono::system_clock::time_point validationTime; // time-stamp genTime
};

class LtvCollector {
public:
    LtvCollector(crypto::CryptoProvider& provider, LtvOptions options);

    // Evidence already known (typically the document's /DSS) is consulted
    // before the network, so repeated upgrades make no requests.
    void seed(const DssData& existing);

    // Never throws; incomplete evidence is a result (`missing`).
    LtvEvidence collect(const LtvSubject& subject);

    std::size_t aiaRequests() const { return aia_ ? aia_->requests : 0; }

private:
    crypto::CryptoProvider& provider_;
    LtvOptions options_;
    std::shared_ptr<crypto::VectorEvidenceStore> known_;
    std::vector<Bytes> knownCerts_;
    std::shared_ptr<crypto::AiaFetchLog> aia_;
    crypto::IssuerFetcher fetcher_;
};

// B-T -> B-LT -> B-LTA on an existing document (ADR-0045).
struct UpgradeRequest {
    PadesLevel target = PadesLevel::BaselineLT;
    LtvOptions ltv;
    // B-LTA: source of the archival document time-stamp (see timestampPdf).
    TimestampFetcher archiveTimestamp;
    crypto::DigestAlgorithm archiveDigest = crypto::DigestAlgorithm::Sha256;
    std::size_t archiveExpectedTokenBytes = 0;
    // Only this signature (e.g. the one just created); default: every CAdES signature.
    std::optional<PdfRef> onlySignature;
};

// Upgrades every CAdES signature of `pdf` to `request.target`:
//  - B-B -> B-T is refused (UnsupportedUpgrade): a signature time-stamp must be
//    requested while signing, and the signed document is never altered;
//  - B-T -> B-LT collects evidence and appends a /DSS revision;
//  - B-LT -> B-LTA appends an archival document time-stamp.
// Without `ltv.allowPartial`, missing mandatory evidence returns
// EvidenceUnavailable and nothing is produced. The result is self-verified and
// reports the level the verifier measured. The input bytes are an exact prefix
// of the output.
SignResult<SignedPdf> upgradePdf(Bytes pdf, const UpgradeRequest& request, crypto::CryptoProvider& provider);
SignResult<SignedPdf> upgradePdfFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                     const UpgradeRequest& request, crypto::CryptoProvider& provider);

// Renewal (ETSI EN 319 142-1 5.4.4): adds the validation data of the newest
// document time-stamp's TSA chain to the /DSS and appends a new archival
// document time-stamp over it.
struct ArchiveRenewalRequest {
    LtvOptions ltv;
    TimestampFetcher fetch; // required
    crypto::DigestAlgorithm digest = crypto::DigestAlgorithm::Sha256;
    std::size_t expectedTokenBytes = 0;
};
SignResult<SignedPdf> addArchiveTimestamp(Bytes pdf, const ArchiveRenewalRequest& request, crypto::CryptoProvider& provider);
SignResult<SignedPdf> addArchiveTimestampFile(const std::filesystem::path& input, const std::filesystem::path& output,
                                              const ArchiveRenewalRequest& request, crypto::CryptoProvider& provider);

namespace detail {
// signPdf's B-LT / B-LTA tail: upgrades the signature just created in `signed`.
SignResult<void> finishLtv(SignedPdf& signedPdf, PadesLevel target, const LtvOptions& ltv,
                           const TimestampFetcher& archive, crypto::DigestAlgorithm archiveDigest,
                           crypto::CryptoProvider& provider);
} // namespace detail

} // namespace rivet::pdf::sign
