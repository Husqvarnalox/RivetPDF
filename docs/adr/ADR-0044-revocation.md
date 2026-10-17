# ADR-0044: OCSP and CRL revocation checking

Status: accepted

## Context

Signature validation needs the revocation status of every certificate on the
path, live and, for historical validation, as of the signing time. Rivet needs
OCSP (RFC 6960) and CRL (RFC 5280) clients, an evidence model that can be
stored in a PDF `/DSS`, and a policy that never turns "could not find out" into
"valid". No new dependencies, no hard-coded responders.

## Decisions

- Layering: `rivet_revocation` (`src/crypto/revocation`, alias
  `rivet::revocation`) links `rivet::crypto`, `rivet::net` and `rivet::core`.
  `rivet_crypto` does not depend on `rivet_net`; the revocation code is
  additive and `crypto/Validation.hpp` only gains an optional
  `ValidationResult::revocationDetail`. `applyRevocation()` folds an outcome
  into a result; callers that do not pass a policy see no change.
- Crypto: hashing, signature verification and random bytes go through the
  `CryptoProvider`; the DER walking uses the strict Rivet reader (`Der.*`).
  Certificate extensions (AIA, CRL distribution points, freshestCRL,
  ocsp-nocheck) are read by this library, not by `X509Parse`. The null
  provider gives `BackendUnavailable`; extension parsing needs no backend.
- Network: only `net::NetworkClient`, so the Off / Ask / On gate, the SSRF
  policy (no loopback, private or link-local targets), redirects and size /
  time limits apply. A gate refusal stops further attempts for that
  certificate (no prompt per URL).
- Untrusted URLs: AIA and CRL DP URIs come from certificates. Only `http` and
  `https` are used; `ldap://`, `file://` and anything else are skipped with a
  recorded note, as are over-long or non-printable URLs. There is no built-in
  responder or CRL URL outside tests.

### OCSP

- Request: SHA-256 CertID by default, nonce on by default (16 random bytes),
  HTTP POST `application/ocsp-request`. One retry with a SHA-1 CertID, only
  when the responder answered `malformedRequest` or `unauthorized` (the
  SHA-1 CertID only identifies a certificate; it protects nothing). The retry
  can be disabled. `tryLater`, `sigRequired`, `internalError` are distinct
  statuses and are not retried.
- Response: the HTTP status must be 200 and the media type
  `application/ocsp-response` (parameters tolerated). Every `responseStatus` is
  parsed; `byName` and `byKey` responder IDs are matched; good, revoked
  (time, reason) and unknown are reported. A signed `unknown` is evidence that
  is never treated as good.
- Signer: the issuing CA itself, or a delegated responder certificate carried
  in the response that is (a) issued by the CA (signature checked through the
  provider), (b) marked `id-kp-OCSPSigning` and (c) valid at `producedAt`.
  Anything else is rejected with a specific failure.
- Nonce: when sent it must be echoed. Both the RFC form (OCTET STRING inside the
  extension) and the raw form that OpenSSL echoes are accepted. If the nonce is
  missing or different the response is rejected.
- `OcspEvidence` keeps the raw DER exactly as received.

### CRL

- GET, size cap (4 MiB default, `NetworkPolicy` applies too), content type not
  checked, DER or PEM. Signature, issuer match (direct CRLs only), CRL-signing
  key usage, authority key identifier, freshness, CRL number and
  `issuingDistributionPoint` scope are checked. Unknown critical extensions are
  rejected. `certificateHold` is reported as revoked with `onHold`.
  `removeFromCRL` outside a delta CRL is rejected.
- Cache: in memory, keyed by URL, at most 32 entries and 16 MiB, expires at
  `nextUpdate`, thread-safe, never written to disk. Only verified CRLs are
  cached; rejected ones are not.

### Time rules (`evidenceTimeProblem`)

- `thisUpdate` or `producedAt` later than now + 5 min: rejected (all modes).
- Live validation: `nextUpdate >= now`; without `nextUpdate`,
  `thisUpdate + maxAge (24 h) >= now`.
- Historical validation (validation time more than the tolerance in the past):
  evidence must speak for that time. Either `thisUpdate >= validationTime`
  (a later "good" proves the certificate was not revoked earlier, because
  revocation is not undone; a revocation dated after the validation time proves
  the same and yields Good), or `thisUpdate <= validationTime <= nextUpdate`.
  Otherwise `DoesNotCoverTime`.

### Policy and verdicts

- Modes: `Off`, `PreferOcspThenCrl`, `OcspOnly`, `CrlOnly`, `RequireAny`.
- Verdict per certificate: Good, Revoked (time, reason), Unknown (reason) or
  NotChecked. Unknown covers no usable evidence, gate refusal, SSRF refusal,
  timeouts, rejected evidence and "unknown to the responder". It is reported
  as Unknown in every mode and is never folded into valid.
- Fail closed: `hardFail` (and `RequireAny`, which implies it) makes
  `RevocationOutcome::acceptable()` false and `applyRevocation()` turn an
  otherwise valid state into `UnableToValidate`. With soft fail the status is
  still Unknown and noted, but does not block. Revoked always wins.
- Certificates carrying `id-pkix-ocsp-nocheck` are skipped (NotChecked, with a
  note). The trust anchor itself is not checked.
- Offline: an `EvidenceStore` of raw OCSP/CRL DER (typically from a PDF `/DSS`)
  is consulted before the network; with `allowNetwork = false` the transport is
  never touched.
- Async: `RevocationChecker::checkAsync` uses `runCryptoAsync` (main-thread
  completion, `AsyncScope` fencing, `CancelToken`, no callbacks under locks).

## Privacy

An OCSP request tells a third party (the responder, and anyone on the path for
plain http) which certificate is being checked, i.e. who signed the document
and, indirectly, that the document is being opened. A CRL fetch reveals only
the CA. This is why every request goes through the online-validation gate: Off
makes no request at all, Ask prompts once, On is the explicit opt-in. The
request carries no document data and the user agent is fixed.

## Not supported

- Delta CRLs: `freshestCRL` is parsed and recorded, a delta-CRL indicator is
  rejected as unsupported, and the limitation is reported.
- Indirect CRLs, reason-partitioned distribution points and relative-name
  distribution points (skipped with a note).
- OCSP stapling in TLS, OCSP GET, multiple CertIDs per request, OCSP response
  caching (responses are cheap; the evidence is stored in the document).
- `ldap://` and `file://` locations.
- Delta CRLs. (Path building from `caIssuers` now exists: `crypto/PathBuilder`
  with `crypto/revocation/AiaFetcher`, over `rivet::net` only, see ADR-0045.)

## Consequences

- Revocation behaviour is testable without a network: tests serve OpenSSL
  produced responses through the mock transport and also interoperate with the
  OpenSSL command line (request read by `openssl ocsp`, CLI responses and an
  `openssl ca -gencrl` CRL verified by Rivet, a Rivet-fetched CRL checked by
  `openssl crl` and `openssl verify -crl_check`).
