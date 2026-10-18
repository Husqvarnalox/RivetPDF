// SPDX-License-Identifier: MPL-2.0
#pragma once

// Types shared by the verifier, the signer and the LTV code (ADR-0045):
// the PAdES baseline level and the options of long-term-validation evidence
// collection. No dependency on the signer or the verifier.

#include "crypto/Certificate.hpp"
#include "crypto/revocation/RevocationChecker.hpp"

#include <memory>
#include <string_view>
#include <vector>

namespace rivet::pdf::sign {

// PAdES baseline level of a signature (ETSI EN 319 142-1).
enum class PadesLevel : std::uint8_t {
    NotPades,    // adbe.pkcs7.detached
    BaselineB,   // ETSI.CAdES.detached, no signature time-stamp
    BaselineT,   // ... plus a signature time-stamp (id-aa-signatureTimeStampToken)
    BaselineLT,  // B-T plus /DSS /VRI validation data for the signer and TSA chains
    BaselineLTA, // B-LT plus a later archival document time-stamp covering the validation data
};

std::string_view toString(PadesLevel level);

// How validation material is gathered for B-LT / B-LTA.
struct LtvOptions {
    // Trust anchors of signer chains (required). The OS store is never used.
    const crypto::TrustAnchorSet* anchors = nullptr;
    // Trust anchors of time-stamp authority chains; nullptr: `anchors`.
    const crypto::TrustAnchorSet* timestampAnchors = nullptr;
    // Extra candidate intermediates (besides the CMS, /DSS and AIA sources).
    std::vector<crypto::Bytes> extraCertificates;
    // Reaches OCSP responders, CRL points and AIA caIssuers URLs, and only
    // through rivet::net (Off / Ask / On gate, SSRF policy). nullptr: no network.
    std::shared_ptr<const net::NetworkClient> network;
    // Revocation policy; `allowNetwork` is forced false when `network` is null.
    // `store` (e.g. evidence from an earlier /DSS) is consulted before the network.
    // Mode Off is refused: B-LT cannot be produced without revocation evidence.
    crypto::RevocationPolicy revocation;
    // Fetch missing issuer certificates through AIA caIssuers (needs `network`).
    bool fetchMissingIssuers = false;
    // false (default): missing mandatory evidence yields no output file and a
    // typed error (SignErrorCode::EvidenceUnavailable) listing the certificates.
    // true: produce what could be collected; the result reports the level
    // actually reached and why it is lower than requested.
    bool allowPartial = false;
    crypto::CancelToken cancel;
};

} // namespace rivet::pdf::sign
