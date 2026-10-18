// SPDX-License-Identifier: MPL-2.0
#include "crypto/Der.hpp"
#include "crypto/timestamp/TimeStamp.hpp"

namespace rivet::crypto {

using namespace der;

CryptoResult<TimeStampRequest> buildTimeStampRequest(CryptoProvider& provider, DigestAlgorithm algorithm,
                                                     ByteView digest, const TimeStampRequestOptions& options) {
    if (auto ok = checkCreationAllowed(algorithm); !ok) return std::unexpected(ok.error());
    if (digest.size() != digestSize(algorithm))
        return fail(CryptoErrorCode::InvalidArgument, "message imprint length does not match the hash algorithm");

    TimeStampRequest req;
    req.algorithm = algorithm;
    req.digest.assign(digest.begin(), digest.end());
    req.certReq = options.certReq;
    req.policyOid = options.policyOid;

    // SHA-2 imprints carry explicit NULL parameters (what TSAs and OpenSSL
    // expect from clients); GOST identifiers carry none.
    Bytes algId = isGost(algorithm) ? sequence({oidElement(digestOid(algorithm))})
                                    : sequence({oidElement(digestOid(algorithm)), nullValue()});

    std::vector<Bytes> parts;
    parts.push_back(integerFromInt(1));
    parts.push_back(sequence({algId, octetString(digest)}));
    if (options.policyOid) {
        auto policy = encodeOid(*options.policyOid);
        if (!policy) return fail(CryptoErrorCode::InvalidArgument, "invalid request policy OID");
        parts.push_back(oidElement(*options.policyOid));
    }
    if (options.includeNonce) {
        auto rnd = provider.randomBytes(8);
        if (!rnd) return std::unexpected(rnd.error());
        if (rnd->size() != 8) return fail(CryptoErrorCode::Internal, "random generator returned a short nonce");
        Bytes nonce = normalizeInteger(*rnd);
        if (nonce.empty()) nonce.push_back(1); // never the all-zero nonce
        req.nonce = nonce;
        parts.push_back(integer(nonce));
    }
    if (options.certReq) parts.push_back(Bytes{kBoolean, 0x01, 0xFF}); // DER TRUE; FALSE is the omitted DEFAULT
    req.der = sequence(parts);
    return req;
}

} // namespace rivet::crypto
