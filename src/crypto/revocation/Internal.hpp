// SPDX-License-Identifier: MPL-2.0
#pragma once

// Helpers shared by the revocation sources. Not part of the public surface.

#include "crypto/Algorithms.hpp"
#include "crypto/Certificate.hpp"
#include "crypto/Der.hpp"
#include "crypto/Providers.hpp"
#include "crypto/revocation/Revocation.hpp"
#include "net/Client.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace rivet::crypto::revocation_detail {

std::string hexOf(ByteView data);

// OIDs.
inline constexpr std::string_view kOidAia = "1.3.6.1.5.5.7.1.1";
inline constexpr std::string_view kOidAdOcsp = "1.3.6.1.5.5.7.48.1";
inline constexpr std::string_view kOidAdCaIssuers = "1.3.6.1.5.5.7.48.2";
inline constexpr std::string_view kOidCrlDp = "2.5.29.31";
inline constexpr std::string_view kOidFreshestCrl = "2.5.29.46";
inline constexpr std::string_view kOidOcspNoCheck = "1.3.6.1.5.5.7.48.1.5";
inline constexpr std::string_view kOidOcspNonce = "1.3.6.1.5.5.7.48.1.2";
inline constexpr std::string_view kOidOcspBasic = "1.3.6.1.5.5.7.48.1.1";
inline constexpr std::string_view kOidKpOcspSigning = "1.3.6.1.5.5.7.3.9";

struct RawExtension {
    std::string oid;
    bool critical = false;
    ByteView value; // content of the extnValue OCTET STRING
};

// Reads a SEQUENCE OF Extension. `bytes` must outlive the result.
CryptoResult<std::vector<RawExtension>> readExtensions(const der::Element& seq);

// Maps a signature AlgorithmIdentifier (complete DER) and the signer's key
// type to the provider's SignatureScheme. Legacy SHA-1 schemes are accepted
// (verification only); unknown algorithms are UnsupportedAlgorithm.
CryptoResult<SignatureScheme> resolveSignatureScheme(ByteView algorithmIdentifierDer, PublicKeyType signerKeyType);

// Verifies `signature` (BIT STRING content, unused-bits octet included) over
// `signedBytes` with `signer`'s key.
CryptoOk verifyWithCertificate(CryptoProvider& provider, const CertificateInfo& signer, ByteView algorithmIdentifierDer,
                               ByteView signedBytes, ByteView signatureBitString);

// SubjectPublicKey BIT STRING content (without the unused-bits octet).
CryptoResult<Bytes> publicKeyBitsOf(const CertificateInfo& cert);

// Hash of a (DER) value through the provider.
CryptoResult<Bytes> hashOf(CryptoProvider& provider, DigestAlgorithm algorithm, ByteView data);

// Single place that turns an HTTP exchange into a CryptoResult. Errors:
// InvalidArgument (unusable URL), Network ("<NetFailure>: ..." from the net
// layer: gate, SSRF policy, transport, size cap, timeout), Cancelled.
CryptoResult<net::HttpResponse> httpExchange(const net::NetworkClient& network, net::HttpMethod method,
                                             std::string_view url, std::string body, net::HttpHeaders headers,
                                             std::size_t maxResponseBytes, const CancelToken& cancel);

// "application/ocsp-response; charset=x" -> "application/ocsp-response".
std::string lowerMediaType(std::string_view value);

// True when the Network error means the user / policy switched online
// validation off or declined: further URLs should not be tried (no repeated prompts).
bool isGateRefusal(const CryptoError& error);

// URIs of the fullName entries of a CRLDistributionPoints / FreshestCRL extension value.
CryptoResult<std::vector<std::string>> parseDistributionPointUris(ByteView extensionValue);

CryptoResult<Bytes> decodePemCrl(ByteView text); // "-----BEGIN X509 CRL-----" -> DER

} // namespace rivet::crypto::revocation_detail
