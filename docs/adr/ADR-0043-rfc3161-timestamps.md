# ADR-0043: RFC 3161 time-stamps and CMS signature-timestamps

Status: accepted

## Context

CAdES-T and PAdES-T signatures carry a trusted time-stamp over the signature
value. Rivet needs a time-stamp client, a token verifier and the CMS
attribute plumbing, without new dependencies and without any hard-coded
service.

## Decisions

- Layering: parsing, verification and CMS attach/verify live in `rivet_crypto`
  (`src/crypto/timestamp/TimeStamp*.cpp`, no network). The HTTP client is the
  separate `rivet_timestamp` library (`rivet::timestamp`), which depends on
  `rivet::net`, so `rivet_crypto` has no network dependency.
- All hashing, signature verification and RNG go through the `CryptoProvider`
  API. The DER/CMS code is Rivet-owned (`Der.*`, `Cms.*`). The null provider
  yields `BackendUnavailable`; parsers need no backend.
- Request: SHA-256 default, SHA-384/512 allowed, GOST R 34.11-2012 typed
  (imprint without NULL parameters), SHA-1 refused (`WeakAlgorithm`). Random
  64-bit nonce, `certReq=TRUE`, optional `reqPolicy`.
- Transport: only `net::NetworkClient` (so `OnlineValidationGate`, SSRF rules and
  `NetworkPolicy` size/time limits apply). POST `application/timestamp-query`;
  HTTP status other than 200 or a Content-Type other than
  `application/timestamp-reply` is `BadResponse`. No built-in TSA URL; the
  caller supplies it. Async variant uses the existing async patterns; callbacks
  never run under locks.
- Attribute: the token is attached as unsigned attribute
  `id-aa-signatureTimeStampToken` on `signerInfo[0]`. Signed attributes,
  signature, certificates and algorithms are byte-identical; the imprint is the
  hash of the signature value octets. Attaching refuses an imprint mismatch, an
  existing token, or a bad signer index.
- States: `SignatureState::ValidTimestamped` (valid, token verified) and
  `ValidTimestampInvalid` (token present but not verified). Untimestamped
  signatures behave exactly as before. A trusted token whose genTime is within
  the signer certificate validity also lets an expired signer certificate be
  evaluated at genTime (`ValidAtTrustedTimestamp`).
- Trust anchors are caller-supplied only (`timestampAnchors`, falling back to the
  signature anchors); the OS store is never consulted implicitly.

## What is verified

PKIStatus granted/grantedWithMods; TSTInfo imprint (algorithm and digest),
nonce (required when requested) and policy (when requested); CMS signature
over the signed attributes, message-digest of TSTInfo, content type; ESSCertID
or ESSCertIDv2 binding of the signing certificate; exactly one signer; TSA
certificate EKU is `id-kp-timeStamping`, critical and the only EKU, with a
compatible key usage; genTime inside the certificate validity; chain to the
supplied anchors. Failures map to: MalformedToken, BadStatus, ImprintMismatch,
NonceMismatch, PolicyMismatch, UnsupportedAlgorithm, SignatureInvalid,
NotATimestampCert, UntrustedTsa, GenTimeOutsideCertValidity.

## What is NOT verified

- No revocation checking of the TSA certificate (no OCSP/CRL for the TSA).
- No check that TSA accuracy/ordering is plausible, and no TSA policy
  allow-list beyond an equality check on a requested policy.
- genTime is not compared with the local clock (TSA clock trust is the TSA's).
- Archival time-stamps (RFC 5126 -A), document time-stamps (PAdES-DTS) and
  embedding the token into PDF signatures are out of scope here.
- Only signerInfo[0] is stamped; countersignature time-stamps are not handled.

## Testing

A test TSA is built in the tests (hand-assembled tokens so every field can be
bent), plus OpenSSL's own TS responder as an independent producer, and the
`openssl ts` / `openssl cms` CLI for interop when installed (skipped
otherwise). Tests skip cleanly with crypto OFF.
