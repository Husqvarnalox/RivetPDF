// SPDX-License-Identifier: MPL-2.0
#include "crypto/Certificate.hpp"

#include "crypto/Providers.hpp"

#include <algorithm>

namespace rivet::crypto {

void TrustAnchorSet::add(const CertificateInfo& cert) {
    if (!contains(cert)) anchors_.push_back(cert);
}

bool TrustAnchorSet::contains(const CertificateInfo& cert) const {
    return std::any_of(anchors_.begin(), anchors_.end(),
                       [&](const CertificateInfo& a) { return a.sha256Fingerprint == cert.sha256Fingerprint; });
}

std::vector<CertificateInfo> TrustAnchorSet::findBySubject(ByteView subjectDer) const {
    std::vector<CertificateInfo> out;
    for (const CertificateInfo& a : anchors_)
        if (a.subjectDer.size() == subjectDer.size() &&
            std::equal(subjectDer.begin(), subjectDer.end(), a.subjectDer.begin()))
            out.push_back(a);
    return out;
}

ChainResult buildChain(const CertificateInfo& leaf, const std::vector<CertificateInfo>& pool,
                       const TrustAnchorSet& anchors, CryptoProvider& provider) {
    constexpr std::size_t kMaxChain = 10;
    ChainResult result;
    result.chain.push_back(leaf);

    while (result.chain.size() <= kMaxChain) {
        const CertificateInfo current = result.chain.back();
        if (anchors.contains(current)) {
            result.complete = true;
            result.anchored = true;
            return result;
        }

        // Candidate issuers: trust anchors first, then the supplied pool.
        std::vector<CertificateInfo> candidates = anchors.findBySubject(current.issuerDer);
        for (const CertificateInfo& p : pool)
            if (p.subjectDer == current.issuerDer) candidates.push_back(p);

        const CertificateInfo* issuer = nullptr;
        for (const CertificateInfo& cand : candidates) {
            if (!cand.isCa) continue;
            if (std::any_of(result.chain.begin(), result.chain.end(), [&](const CertificateInfo& c) {
                    return c.sha256Fingerprint == cand.sha256Fingerprint;
                }))
                continue; // loop (including a self-issued root already in the chain)
            if (provider.isIssuedBy(current, cand)) {
                issuer = &cand;
                break;
            }
        }
        if (!issuer) {
            // A self-issued certificate that verifies against itself is a root.
            if (current.isSelfIssued() && provider.isIssuedBy(current, current)) {
                result.complete = true;
                result.anchored = anchors.contains(current);
            }
            return result;
        }
        result.chain.push_back(*issuer);
    }
    return result; // too long: incomplete
}

CertificateValidation validateCertificate(const CertificateInfo& leaf, const std::vector<CertificateInfo>& pool,
                                          const TrustAnchorSet& anchors, CryptoProvider& provider,
                                          std::chrono::system_clock::time_point at) {
    CertificateValidation out;
    out.chain = buildChain(leaf, pool, anchors, provider);

    out.certificate = CertificateStatus::Valid;
    for (const CertificateInfo& c : out.chain.chain) {
        if (at < c.notBefore) {
            out.certificate = CertificateStatus::NotYetValid;
            break;
        }
        if (at > c.notAfter) {
            out.certificate = CertificateStatus::Expired;
            break;
        }
    }
    if (out.certificate == CertificateStatus::Valid && leaf.hasKeyUsage) {
        const std::uint16_t needed = keyusage::kDigitalSignature | keyusage::kNonRepudiation;
        if ((leaf.keyUsage & needed) == 0) out.certificate = CertificateStatus::UsageInvalid;
    }

    out.trust = out.chain.anchored ? TrustStatus::Trusted : TrustStatus::Untrusted;
    return out;
}

} // namespace rivet::crypto
