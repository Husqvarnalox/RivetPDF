// SPDX-License-Identifier: MPL-2.0
#include "pdf/sign/LtvChain.hpp"

#include <algorithm>

namespace rivet::pdf::sign::ltv {

namespace {

constexpr std::size_t kMaxMissing = 24;

std::string label(const crypto::CertificateInfo& c) {
    std::string s = c.commonName ? *c.commonName : c.subject;
    if (s.size() > 80) s.resize(80);
    return "'" + s + "'";
}

} // namespace

std::shared_ptr<crypto::CryptoProvider> borrowProvider(crypto::CryptoProvider& p) {
    return std::shared_ptr<crypto::CryptoProvider>(std::shared_ptr<void>(), &p);
}

std::vector<crypto::CertificateInfo> parseCertificates(crypto::CryptoProvider& provider,
                                                       const std::vector<crypto::Bytes>& der) {
    std::vector<crypto::CertificateInfo> out;
    for (const crypto::Bytes& d : der)
        if (auto c = provider.parseCertificate(d)) out.push_back(std::move(*c));
    return out;
}

ChainEvidence checkChain(crypto::CryptoProvider& provider, const ChainInput& in,
                         const std::shared_ptr<const net::NetworkClient>& network, crypto::RevocationPolicy policy,
                         const crypto::CancelToken& cancel) {
    ChainEvidence out;
    auto miss = [&](std::string s) {
        if (out.missing.size() < kMaxMissing) out.missing.push_back(std::move(s));
    };
    if (policy.mode == crypto::RevocationMode::Off) {
        miss("revocation checking is off: no evidence can be produced for the " + in.role + " chain");
        return out;
    }
    if (!in.anchors) {
        miss("no trust anchors were supplied for the " + in.role + " chain");
        return out;
    }
    crypto::PathOptions po;
    po.validationTime = in.validationTime;
    po.anchors = in.anchors;
    po.pool = in.pool;
    po.fetchIssuers = in.fetchIssuers;
    po.maxSearchSteps = in.maxSearchSteps;
    po.cancel = cancel;
    out.path = crypto::buildValidationPath(in.leaf, po, provider);
    if (!out.path.anchored) {
        std::string why = "the " + in.role + " certificate " + label(in.leaf) + " has no valid path to a trust anchor (" +
                          std::string(crypto::toString(out.path.failure)) + ")";
        for (const std::string& r : out.path.reasons) why += "; " + r;
        miss(std::move(why));
        return out;
    }

    policy.hardFail = true;
    policy.now = in.now;
    if (!network) policy.allowNetwork = false;
    const crypto::RevocationChecker checker(borrowProvider(provider), network);
    out.revocation = checker.check(out.path.chain, in.validationTime, policy, cancel);
    bool ok = true;
    for (const crypto::CertRevocation& c : out.revocation.certs) {
        if (c.ocspNoCheck) continue; // id-pkix-ocsp-nocheck: no evidence is defined for this certificate
        if (c.status == crypto::RevocationStatus::Good) {
            if (c.acceptedOcsp && *c.acceptedOcsp < c.ocsp.size()) out.ocsps.push_back(c.ocsp[*c.acceptedOcsp].rawResponseDer);
            else if (c.acceptedCrl && *c.acceptedCrl < c.crl.size()) {
                out.crls.push_back(c.crl[*c.acceptedCrl].rawCrlDer);
                if (c.acceptedDeltaCrl && *c.acceptedDeltaCrl < c.crl.size()) out.crls.push_back(c.crl[*c.acceptedDeltaCrl].rawCrlDer);
            }
            else {
                ok = false;
                miss("certificate " + label(out.path.chain[c.chainIndex]) + ": status is good but no raw evidence is available");
            }
            continue;
        }
        ok = false;
        const crypto::CertificateInfo& cert = out.path.chain[std::min(c.chainIndex, out.path.chain.size() - 1)];
        if (c.status == crypto::RevocationStatus::Revoked)
            miss("certificate " + label(cert) + " is revoked");
        else
            miss("certificate " + label(cert) + ": no usable OCSP or CRL evidence" +
                 (c.unknownReason.empty() ? std::string() : " (" + c.unknownReason + ")"));
    }
    if (out.revocation.certs.empty() && out.path.chain.size() >= 2) {
        ok = false;
        miss("revocation status of the " + in.role + " chain was not determined");
    }
    out.complete = ok;
    return out;
}

} // namespace rivet::pdf::sign::ltv
