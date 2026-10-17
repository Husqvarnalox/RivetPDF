// SPDX-License-Identifier: MPL-2.0
#pragma once

// Backend-free X.509 reader built on Der.hpp. Unlike an OpenSSL-based parse it
// understands GOST R 34.10-2012 public keys and signatures, so GOST
// certificates (CryptoPro, Russian CAs) get a correct CertificateInfo even
// when no GOST-capable library is linked. It never validates a signature.

#include "crypto/Certificate.hpp"
#include "crypto/Error.hpp"
#include "crypto/Types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace rivet::crypto {

struct NameAttribute {
    std::string oid;
    std::string value; // UTF-8
};

// Parts of a certificate that CertificateInfo does not carry.
struct X509Extras {
    std::vector<NameAttribute> subjectAttributes; // in encoding order
    std::vector<NameAttribute> issuerAttributes;
    std::vector<std::string> certificatePolicyOids; // 2.5.29.32
    Bytes spkiDer;                                  // complete SubjectPublicKeyInfo
    std::string spkiAlgorithmOid;                   // e.g. 1.2.643.7.1.1.1.1
    std::vector<std::string> spkiParameterOids;     // curve / digest parameter-set OIDs (GOST)
    Bytes publicKeyBits;                            // BIT STRING content without the unused-bits octet
};

// Fingerprint source (SHA-1 / SHA-256). Typically `provider.digest`.
using DigestFn = std::function<CryptoResult<Bytes>(DigestAlgorithm, ByteView)>;

// Fills every CertificateInfo field the backend-independent model has, including
// publicKeyType = Gost3410_2012_256/512 for OIDs 1.2.643.7.1.1.1.1/2 (keyBits
// 256/512) and Rsa/Ec for the common OIDs (RSA keyBits from the modulus, EC
// from the curve OID when it is a named P-256/384/521 curve, otherwise 0).
// `digest` may be empty: fingerprints are then left empty.
CryptoResult<CertificateInfo> parseX509(ByteView der, const DigestFn& digest = {}, X509Extras* extras = nullptr);

} // namespace rivet::crypto
