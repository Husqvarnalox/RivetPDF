// SPDX-License-Identifier: MPL-2.0
#pragma once

// Revocation policy and orchestration over OCSP and CRL evidence, from the
// network (through net::NetworkClient only) or from a pre-supplied
// EvidenceStore (embedded PDF /DSS evidence: zero network calls).

#include "core/async/AsyncScope.hpp"
#include "core/async/IMainThreadDispatcher.hpp"
#include "core/async/TaskScheduler.hpp"
#include "crypto/Providers.hpp"
#include "crypto/revocation/Crl.hpp"
#include "crypto/revocation/Ocsp.hpp"
#include "crypto/revocation/Revocation.hpp"
#include "net/Client.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace rivet::crypto {

enum class RevocationMode : std::uint8_t {
    Off,               // no revocation checking: every certificate is reported NotChecked
    PreferOcspThenCrl, // OCSP first, CRL when OCSP yields no usable answer
    OcspOnly,
    CrlOnly,
    RequireAny,        // like PreferOcspThenCrl, but fail closed: no usable evidence is a hard failure
};

std::string_view toString(RevocationMode);

// Raw evidence supplied up front (typically read from a PDF's /DSS). Looking
// into it never touches the network.
class EvidenceStore {
public:
    virtual ~EvidenceStore() = default;
    virtual std::vector<Bytes> ocspResponses() const = 0; // raw OCSPResponse DER
    virtual std::vector<Bytes> crls() const = 0;          // raw CRL DER (or PEM)
};

class VectorEvidenceStore final : public EvidenceStore {
public:
    void addOcsp(Bytes der);
    void addCrl(Bytes der);
    std::vector<Bytes> ocspResponses() const override;
    std::vector<Bytes> crls() const override;

private:
    mutable std::mutex mutex_;
    std::vector<Bytes> ocsp_;
    std::vector<Bytes> crl_;
};

struct RevocationPolicy {
    RevocationMode mode = RevocationMode::PreferOcspThenCrl;
    // false (soft fail): "unknown" is reported as Unknown but does not make
    // acceptable() false. true (hard fail): Unknown is not acceptable. In
    // both cases unknown is NEVER reported as good. RequireAny implies true.
    bool hardFail = false;
    bool useOcspNonce = true;
    bool allowSha1CertIdFallback = true;
    // false: only `store` is consulted (offline verification).
    bool allowNetwork = true;
    std::chrono::milliseconds clockTolerance = std::chrono::minutes(5);
    std::chrono::milliseconds maxAge = std::chrono::hours(24); // evidence without nextUpdate
    std::size_t maxOcspBytes = std::size_t{1} << 20;
    std::size_t maxCrlBytes = std::size_t{4} << 20;
    std::optional<TimePoint> now;                 // clock override (tests); default: system clock
    std::shared_ptr<const EvidenceStore> store;   // consulted before the network
};

enum class RevocationSource : std::uint8_t { None, Ocsp, Crl };

struct CertRevocation {
    std::size_t chainIndex = 0;
    std::string subject;
    RevocationStatus status = RevocationStatus::NotChecked;
    RevocationSource source = RevocationSource::None;
    std::optional<TimePoint> revokedAt;
    std::optional<int> reasonCode;       // CRLReason
    bool onHold = false;
    bool ocspNoCheck = false;            // skipped: id-pkix-ocsp-nocheck
    std::string unknownReason;           // set when status == Unknown: why no usable evidence
    std::vector<OcspEvidence> ocsp;      // every response examined (accepted one: verified == true)
    std::vector<CrlEvidence> crl;
    std::optional<std::size_t> acceptedOcsp; // index into `ocsp`
    std::optional<std::size_t> acceptedCrl;  // index into `crl`
    std::vector<std::string> notes;      // skipped URLs, limitations, informational
};

struct RevocationOutcome {
    std::vector<CertRevocation> certs;   // one per checked certificate (chain[0 .. n-2])
    RevocationStatus status = RevocationStatus::NotChecked; // Revoked > Unknown > Good > NotChecked
    bool hardFail = false;
    std::vector<std::string> notes;

    // False when revoked, or unknown under a hard-fail policy.
    bool acceptable() const;
};

// Checks every non-anchor certificate of a path: chain[i + 1] is the issuer
// of chain[i], leaf first, and the last element is the trust anchor (it is
// not checked itself). `validationTime` is the instant the status is needed
// for: now for live validation, the signing time for historical validation.
//
// Evidence rules (see evidenceTimeProblem): live validation needs fresh
// evidence. For historical validation evidence produced after the validation
// time is accepted only when it still proves the certificate was not revoked
// at that time (status good, or a revocation time later than the validation
// time); evidence older than the validation time must have been valid then.
//
// Never reports a certificate good without verified evidence; unusable
// evidence, refusals by the network layer (gate Off / declined, SSRF policy)
// and transport failures all yield Unknown with the reason recorded.
class RevocationChecker {
public:
    RevocationChecker(std::shared_ptr<CryptoProvider> provider, std::shared_ptr<const net::NetworkClient> network,
                      std::shared_ptr<CrlCache> cache = std::make_shared<CrlCache>());

    // Blocking when the network is used: run through checkAsync, never on the UI thread.
    RevocationOutcome check(const std::vector<CertificateInfo>& chain, TimePoint validationTime,
                            const RevocationPolicy& policy, const CancelToken& cancel) const;

    // `done` runs once on the main thread (runCryptoAsync contract); returns
    // false (no callback) when the scope is closed.
    bool checkAsync(core::TaskScheduler& scheduler, core::IMainThreadDispatcher& dispatcher, core::AsyncScope& scope,
                    CancelToken cancel, std::vector<CertificateInfo> chain, TimePoint validationTime,
                    RevocationPolicy policy, std::function<void(CryptoResult<RevocationOutcome>)> done) const;

private:
    CertRevocation checkOne(const std::vector<CertificateInfo>& chain, std::size_t index, const EvidenceClock& clock,
                            const RevocationPolicy& policy, const CancelToken& cancel) const;

    std::shared_ptr<CryptoProvider> provider_;
    std::shared_ptr<const net::NetworkClient> network_;
    std::shared_ptr<CrlCache> cache_;
};

// Additive integration with the existing validation model: sets
// result.revocation / result.revocationDetail, appends notes and re-folds the
// state (Revoked takes precedence). Under a hard-fail policy an Unknown
// outcome turns a valid state into UnableToValidate. Callers that never pass
// a policy never call this, so their results are unchanged.
void applyRevocation(ValidationResult& result, const RevocationOutcome& outcome);

} // namespace rivet::crypto
