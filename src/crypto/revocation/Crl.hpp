// SPDX-License-Identifier: MPL-2.0
#pragma once

// CRL (RFC 5280 section 5): parsing / verification of base CRLs, an in-memory
// per-session cache and an HTTP fetcher. Network access only through
// net::NetworkClient. Not supported (reported, never silently ignored): delta
// CRLs, indirect CRLs, CRLs partitioned by reason code.

#include "crypto/Providers.hpp"
#include "crypto/revocation/Revocation.hpp"
#include "net/Client.hpp"

#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace rivet::crypto {

// Verifies a CRL (DER or PEM) for (`cert`, `issuer`):
//  - the CRL issuer equals the certificate's issuer (direct CRLs only);
//  - the issuer certificate may sign CRLs (cRLSign when keyUsage is present);
//  - the signature verifies under the issuer's key;
//  - critical extensions other than the supported ones reject the CRL
//    (a deltaCRLIndicator is an unsupported feature);
//  - issuingDistributionPoint scope: a CRL whose scope does not cover the
//    certificate (user / CA only, other distribution point, partial reasons)
//    is NotApplicable;
//  - time rules of evidenceTimeProblem() with thisUpdate as production time;
//  - lookup of the serial; certificateHold is revoked-with-hold, an entry
//    with removeFromCRL is rejected (meaningful only in a delta CRL).
// Never throws; problems are in the evidence (verified == false).
CrlEvidence verifyCrl(ByteView crlDerOrPem, CryptoProvider& provider, const CertificateInfo& cert,
                      const CertificateInfo& issuer, const RevocationExtensions& certExtensions,
                      const EvidenceClock& clock);

// Bounded, thread-safe, in-memory cache of raw CRLs keyed by URL. Entries
// expire at the CRL's nextUpdate; the oldest-used entry is evicted first. It
// never touches the disk.
class CrlCache {
public:
    struct Limits {
        std::size_t maxEntries = 32;
        std::size_t maxBytes = std::size_t{16} << 20;
    };
    CrlCache() = default;
    explicit CrlCache(Limits limits) : limits_(limits) {}

    // The cached DER, if present and not past its expiry at `now`.
    std::optional<Bytes> find(const std::string& url, TimePoint now);
    // A CRL larger than maxBytes is not cached.
    void insert(const std::string& url, Bytes der, TimePoint expiry);
    void clear();
    std::size_t size() const;
    std::size_t totalBytes() const;

private:
    struct Entry {
        Bytes der;
        TimePoint expiry;
        std::list<std::string>::iterator order;
    };
    void evictLocked();

    Limits limits_;
    mutable std::mutex mutex_;
    std::map<std::string, Entry> entries_;
    std::list<std::string> lru_; // front = most recently used
    std::size_t bytes_ = 0;
};

struct CrlFetchOptions {
    std::size_t maxResponseBytes = std::size_t{4} << 20;
    bool useCache = true;
    EvidenceClock clock;
};

class CrlClient {
public:
    CrlClient(std::shared_ptr<const net::NetworkClient> network, std::shared_ptr<CryptoProvider> provider,
              std::shared_ptr<CrlCache> cache = std::make_shared<CrlCache>());

    // Blocking GET (content type is not checked: CRL servers are sloppy).
    // Errors: InvalidArgument, BackendUnavailable, Network, BadResponse (HTTP
    // status != 200 / empty body), Cancelled. A CRL that arrives but is
    // rejected is a successful result with verified == false.
    CryptoResult<CrlEvidence> fetch(std::string_view url, const CertificateInfo& cert, const CertificateInfo& issuer,
                                    const RevocationExtensions& certExtensions, const CrlFetchOptions& options,
                                    const CancelToken& cancel) const;

    const std::shared_ptr<CrlCache>& cache() const { return cache_; }

private:
    std::shared_ptr<const net::NetworkClient> network_;
    std::shared_ptr<CryptoProvider> provider_;
    std::shared_ptr<CrlCache> cache_;
};

} // namespace rivet::crypto
