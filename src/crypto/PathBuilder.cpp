// SPDX-License-Identifier: MPL-2.0
#include "crypto/PathBuilder.hpp"

#include "crypto/Cms.hpp"

#include <algorithm>
#include <set>

namespace rivet::crypto {

std::string_view toString(PathFailure f) {
    switch (f) {
    case PathFailure::None: return "none";
    case PathFailure::NoIssuer: return "issuer not found";
    case PathFailure::NotAnchored: return "root is not a trust anchor";
    case PathFailure::NotValidAtTime: return "certificate not valid at the validation time";
    case PathFailure::NotACa: return "issuer is not a CA";
    case PathFailure::KeyUsage: return "issuer key usage forbids certificate signing";
    case PathFailure::PathLength: return "path length constraint exceeded";
    case PathFailure::WeakAlgorithm: return "weak certificate signature algorithm";
    case PathFailure::Loop: return "loop in the path";
    case PathFailure::TooLong: return "path too long";
    case PathFailure::BadSignature: return "certificate signature does not verify";
    case PathFailure::BackendUnavailable: return "no crypto backend";
    }
    return "?";
}

namespace {

constexpr std::size_t kMaxReasons = 24;
constexpr std::size_t kMaxPool = 512;
constexpr std::size_t kMaxSearchStepsCeiling = 65536; // hard ceiling for PathOptions::maxSearchSteps (default 4096)

bool brokenSignatureOid(std::string_view oid) {
    return oid == "1.2.840.113549.1.1.2" ||  // md2WithRSA
           oid == "1.2.840.113549.1.1.4" ||  // md5WithRSA
           oid == "1.3.14.3.2.3";            // md5WithRSA (OIW)
}

bool sha1SignatureOid(std::string_view oid) {
    return oid == "1.2.840.113549.1.1.5" || oid == "1.2.840.10045.4.1" || oid == "1.2.840.10040.4.3" ||
           oid == "1.3.14.3.2.29";
}

// id-ce-nameConstraints (2.5.29.30) DER-encoded OID: found by a byte scan; the
// extension is not interpreted (limitation recorded).
bool hasNameConstraints(const Bytes& der) {
    static constexpr std::uint8_t kOid[] = {0x06, 0x03, 0x55, 0x1D, 0x1E};
    return std::search(der.begin(), der.end(), std::begin(kOid), std::end(kOid)) != der.end();
}

std::string label(const CertificateInfo& c) {
    std::string s = c.commonName ? *c.commonName : c.subject;
    if (s.size() > 80) s.resize(80);
    return "'" + s + "'";
}

struct Search {
    const PathOptions& opt;
    CryptoProvider& provider;
    std::vector<CertificateInfo> pool;
    PathResult best;           // the best result so far (anchored wins)
    std::size_t bestScore = 0; // 3 anchored, 2 complete unanchored, 1 partial
    std::size_t fetches = 0;
    std::size_t steps = 0;
    bool stepLimitNoted = false;
    std::vector<Bytes> fetchedUsed;
    std::set<std::string> fetchedFingerprints;
    std::set<std::string> poolFingerprints;
    std::set<std::string> reasonSet;

    void reason(const std::string& r) {
        if (best.reasons.size() < kMaxReasons && reasonSet.insert(r).second) best.reasons.push_back(r);
    }

    void addToPool(const CertificateInfo& c) {
        if (pool.size() >= kMaxPool) return;
        if (poolFingerprints.insert(c.sha256Fingerprint).second) pool.push_back(c);
    }

    void record(const std::vector<CertificateInfo>& chain, std::size_t score, bool complete, bool anchored, PathFailure why) {
        if (score <= bestScore) return;
        bestScore = score;
        best.chain = chain;
        best.complete = complete;
        best.anchored = anchored;
        best.failure = anchored ? PathFailure::None : why;
        best.fetched.clear();
        for (const CertificateInfo& c : chain)
            if (fetchedFingerprints.count(c.sha256Fingerprint)) best.fetched.push_back(c.der);
    }

    // `chain` ends with the certificate whose issuer is needed.
    bool extend(std::vector<CertificateInfo>& chain) {
        if (opt.cancel.cancelled()) return false;
        if (++steps > std::min(opt.maxSearchSteps, kMaxSearchStepsCeiling)) {
            if (!stepLimitNoted) {
                stepLimitNoted = true;
                reason("path search stopped: the step budget (" + std::to_string(opt.maxSearchSteps) + ") was exhausted");
            }
            return false;
        }
        const CertificateInfo cur = chain.back();

        if (opt.anchors && opt.anchors->contains(cur)) {
            if (!cur.validAt(opt.validationTime)) {
                reason("trust anchor " + label(cur) + " is not valid at the validation time");
                record(chain, 1, true, false, PathFailure::NotValidAtTime);
                return false;
            }
            record(chain, 3, true, true, PathFailure::None);
            return true;
        }
        if (chain.size() >= opt.maxLength) {
            reason("the path would exceed " + std::to_string(opt.maxLength) + " certificates");
            record(chain, 1, false, false, PathFailure::TooLong);
            return false;
        }
        if (brokenSignatureOid(cur.signatureAlgorithmOid)) {
            reason("certificate " + label(cur) + " is signed with a broken algorithm (MD2/MD5)");
            record(chain, 1, false, false, PathFailure::WeakAlgorithm);
            return false;
        }

        auto candidatesFor = [&] {
            std::vector<CertificateInfo> out;
            std::set<std::string> seen;
            auto add = [&](const CertificateInfo& c) {
                if (seen.insert(c.sha256Fingerprint).second) out.push_back(c);
            };
            if (opt.anchors)
                for (const CertificateInfo& a : opt.anchors->findBySubject(cur.issuerDer)) add(a);
            for (const CertificateInfo& p : pool)
                if (p.subjectDer == cur.issuerDer) add(p);
            return out;
        };
        std::vector<CertificateInfo> candidates = candidatesFor();
        bool anyVerified = false;
        if (candidates.empty() && opt.fetchIssuers && fetches < opt.maxFetches && !cur.isSelfIssued()) {
            ++fetches;
            for (const Bytes& der : opt.fetchIssuers(cur)) {
                if (pool.size() >= kMaxPool) break;
                auto parsed = provider.parseCertificate(der);
                if (!parsed) continue;
                if (poolFingerprints.count(parsed->sha256Fingerprint)) continue;
                fetchedFingerprints.insert(parsed->sha256Fingerprint);
                addToPool(*parsed);
            }
            candidates = candidatesFor();
        }

        if (cur.isSelfIssued() && !(opt.anchors && opt.anchors->contains(cur))) {
            // A self-issued certificate that verifies against itself is a root, trusted or not.
            if (provider.isIssuedBy(cur, cur)) {
                reason("the path ends in " + label(cur) + ", which is not a trust anchor");
                record(chain, 2, true, false, PathFailure::NotAnchored);
            } else {
                reason("self-issued certificate " + label(cur) + " does not verify against itself");
                record(chain, 1, false, false, PathFailure::BadSignature);
            }
            return false;
        }

        // Intermediates below the candidate: chain.size() - 1 (the leaf is index 0, `cur` is last).
        const std::size_t below = chain.size() - 1;
        for (const CertificateInfo& cand : candidates) {
            if (opt.cancel.cancelled()) return false;
            if (std::any_of(chain.begin(), chain.end(),
                            [&](const CertificateInfo& c) { return c.sha256Fingerprint == cand.sha256Fingerprint; })) {
                reason("issuer " + label(cand) + " is already in the path (loop)");
                record(chain, 1, false, false, PathFailure::Loop);
                continue;
            }
            if (!cand.isCa) {
                reason("issuer " + label(cand) + " is not a CA (basicConstraints)");
                record(chain, 1, false, false, PathFailure::NotACa);
                continue;
            }
            if (cand.hasKeyUsage && (cand.keyUsage & keyusage::kKeyCertSign) == 0) {
                reason("issuer " + label(cand) + " has no keyCertSign key usage");
                record(chain, 1, false, false, PathFailure::KeyUsage);
                continue;
            }
            if (!cand.validAt(opt.validationTime)) {
                reason("issuer " + label(cand) + " is not valid at the validation time");
                record(chain, 1, false, false, PathFailure::NotValidAtTime);
                continue;
            }
            if (cand.pathLength && below > static_cast<std::size_t>(std::max(*cand.pathLength, 0))) {
                reason("issuer " + label(cand) + " allows at most " + std::to_string(*cand.pathLength) +
                       " intermediate certificate(s) below it");
                record(chain, 1, false, false, PathFailure::PathLength);
                continue;
            }
            if (!provider.isIssuedBy(cur, cand)) {
                reason("certificate " + label(cur) + " is not signed by the key of " + label(cand));
                record(chain, 1, false, false, PathFailure::BadSignature);
                continue;
            }
            anyVerified = true;
            chain.push_back(cand);
            if (extend(chain)) return true;
            chain.pop_back();
        }
        if (!anyVerified) {
            if (candidates.empty()) reason("no issuer found for " + label(cur));
            record(chain, 1, false, false, candidates.empty() ? PathFailure::NoIssuer : best.failure);
            if (candidates.empty() && bestScore == 1 && best.failure == PathFailure::None) best.failure = PathFailure::NoIssuer;
        }
        return false;
    }
};

} // namespace

PathResult buildValidationPath(const CertificateInfo& leaf, const PathOptions& options, CryptoProvider& provider) {
    PathResult empty;
    if (!provider.isAvailable()) {
        empty.chain = {leaf};
        empty.failure = PathFailure::BackendUnavailable;
        empty.reasons.push_back("no crypto backend available");
        return empty;
    }
    Search s{options, provider, {}, {}, 0, 0, 0, false, {}, {}, {}, {}};
    for (const CertificateInfo& c : options.pool) s.addToPool(c);
    s.best.chain = {leaf};
    s.best.failure = PathFailure::NoIssuer;

    if (options.checkLeafValidity && !leaf.validAt(options.validationTime)) {
        s.reason("certificate " + label(leaf) + " is not valid at the validation time");
        s.best.failure = PathFailure::NotValidAtTime;
        s.bestScore = 0;
        // Still try to build the chain so that callers see where it ends, but it is never reported as a good path.
    }
    std::vector<CertificateInfo> chain{leaf};
    s.extend(chain);
    PathResult out = std::move(s.best);
    if (options.checkLeafValidity && !leaf.validAt(options.validationTime)) {
        if (out.failure == PathFailure::None) out.failure = PathFailure::NotValidAtTime;
        out.anchored = false;
    }
    for (const CertificateInfo& c : out.chain) {
        if (sha1SignatureOid(c.signatureAlgorithmOid) && out.weakNotes.size() < 8)
            out.weakNotes.push_back("certificate " + label(c) + " uses a SHA-1 signature (accepted for verification only)");
        if (hasNameConstraints(c.der))
            out.limitations.push_back("certificate " + label(c) + " carries nameConstraints: not enforced");
    }
    out.limitations.push_back("certificate policies and policy constraints are not evaluated");
    return out;
}

} // namespace rivet::crypto
