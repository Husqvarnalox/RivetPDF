# ADR-0047: Cloud signing (RemoteSigningProvider) and the CryptoPro DSS adapter

Status: accepted (mock-verified only; real cloud operation BLOCKED)

## Context

Qualified electronic signatures in the cloud keep the private key inside a
service (here a CryptoPro DSS, Digital Signature Service). Rivet must be able to
sign a PDF without ever holding that key: the service signs a digest, Rivet
assembles the CMS (CAdES / PAdES) around the returned signature value and
certificate chain, and the existing verifier checks the result.

**Verification status.** No real DSS endpoint, tenant, credentials or vendor
documentation was available. The adapter is built and tested against an
in-process MOCK service only (`tests/crypto/remote/MockDss.hpp`) that speaks the
adapter's own assumed wire format. Passing the tests proves the adapter's
logic, framing, error handling and the PAdES assembly. It does NOT prove
interoperability with a real DSS. Real cloud QES end to end is BLOCKED until
credentials and documentation exist. Nothing in the product, tests or docs may
claim otherwise.

## Decisions

### Abstraction (rivet_crypto)

- `RemoteSigningProvider` (`crypto/Providers.hpp`) replaces the earlier
  interface-only stub (it had no implementation and no users). It is net-free:
  `authenticate`, `listIdentities`, `signHash(RemoteSignRequest, PinPrompt*,
  CancelToken)`. The provider returns a raw signature value over a digest; the
  chain comes from `listIdentities`.
- `RemoteSigningIdentity` (`crypto/RemoteIdentity.hpp`) implements the existing
  `SigningIdentity` interface. `sign()` hashes the signed attributes locally,
  sends only the digest through the provider and then verifies the returned
  signature under the certificate (a service answering with a signature that
  does not verify is rejected with `BadResponse` before it reaches a document).
  `signCms` and `signPdf` therefore work unchanged: no change to `src/pdf/sign`.
- `RemoteIdentityStore` exposes remote identities through `IdentityStore`
  (`KeyLocationKind::Cloud`, new `ProviderAssurance::RemoteService`).
- A PDF-signer retry (placeholder too small) changes the `/ByteRange`, hence the
  digest, and costs the user another confirmation. The default reservation
  already includes the whole chain, so this does not happen in normal use
  (tested: exactly one service call).
- GOST: a GOST key would need a GOST-capable `CryptoProvider` for the local
  hashing (the OpenSSL provider has none). The mapping is present, but only
  RSA / ECDSA were exercised, against the mock.

### Privacy modes

- `HashOnly` (default): only the digest of the signed attributes leaves the
  machine. The adapter never serialises a document in this mode, even if the
  caller supplies one (tested).
- `WholeDocument`: explicit opt-in. Creating the provider requires
  `wholeDocumentConsent == kDssWholeDocumentConsentText` (the UI shows the text
  and passes it back); anything else is refused. In this mode the COMPLETE
  document is uploaded to the service (base64 in the sign request) in addition
  to the digest. The returned value is still a raw signature over the digest,
  verified locally, and Rivet builds the CMS itself. (A real DSS "sign
  document" operation that returns a finished CMS is not implemented; see
  Assumptions.) The caller hands the document to the identity with
  `setDocument()`.

### Network and secrets

- Only `rivet::net`: consent gate (Off / Ask / On), SSRF policy, size and time
  limits. The adapter additionally refuses a non-https service URL, userinfo
  and queries, even when `NetworkPolicy::allowHttp` is true.
- Redirects are never followed (`maxRedirects = 0`): a redirect could re-send
  the token to another host. The net layer also refuses https to http
  downgrades.
- Secrets (password, PIN / OTP, token) live in `SecretBuffer`; copies in
  request bodies are wiped by the adapter where it owns the string. They are
  never put in URLs, never logged, and never copied into error messages
  (server-supplied text is not echoed, only the HTTP status). The adapter
  writes no log lines.
- The access token is kept in memory only, in this tranche. Rivet does not
  persist it; `forgetAccessToken()` and destruction zero it. No credential
  store is added.
- Threading: all calls block and run through `runCryptoAsync` (main-thread
  callbacks, `AsyncScope`, `CancelToken`). The token mutex is never held across
  network or prompt calls; the `PinPrompt` is called without any lock.
  Cancellation of the crypto token is bridged to the net token.

### Error taxonomy

New `CryptoErrorCode` values: `AuthFailed`, `ConfirmationRequired` (PIN / OTP
needed, no prompt), `ConfirmationRejected` (wrong, attempts exhausted, or
declined on the device), `RateLimited`, `ServerError`, `Timeout`,
`NotQualified`. Existing codes are reused: `PinLocked`, `Cancelled`, `Network`
(gate, policy, transport), `BadResponse`, `SessionExpired`.

- HTTP 5xx is retried at most `maxRetries` times (default 2) with linear
  backoff, re-sending the same body including one `request_id` idempotency key.
  4xx refusals (401, 403, 429, not-qualified) are never retried.
- PIN / OTP: up to `maxPinAttempts` (default 3) prompts; the prompt reasons are
  Required, Incorrect, LastAttempt; a service lock gives `PinLocked`.
- Asynchronous confirmation (e.g. on the user's phone) is polled every
  `pollInterval` until `confirmationTimeout` (then `Timeout`).
- User cancel (in the prompt, or the token during polling) returns `Cancelled`,
  tells the service best-effort (`cancel` endpoint, short timeout, no document)
  and produces no output: `signPdfFile` writes nothing and leaves no temporary
  file (tested).

### Classification stays evidence-based

`ProviderAssurance::RemoteService` counts as a non-exportable custody but is
never `CertifiedCspDeclared`, so a signature made through it cannot reach
`SignatureClass::QualifiedEvidence` (tested with the mock CA on an accredited
list, a listed policy and a listed provider id). Making a real, verified
cloud QES classifiable needs a new, explicit evidence input (for example a
list directive declaring a remote QSCD service) and is a follow-up. Mock chains
are never QES.

## Assumptions (unverified; all in `src/crypto/remote/CryptoProDss.cpp`)

The whole wire format is an assumption, kept in the `dss` namespace of that one
file so it is easy to correct:

- Paths: `/oauth/token`, `/api/v1/identities`, `/api/v1/sign`,
  `/api/v1/sign/confirm`, `/api/v1/sign/status`, `/api/v1/sign/cancel` below the
  configured service URL.
- OAuth2 password grant (form-encoded) for the token; JSON bodies (base64
  binary fields) for everything else.
- **The token travels in the JSON body (`access_token`).** A real DSS most
  likely expects `Authorization: Bearer`. The net layer forbids a caller-set
  `Authorization` header on purpose (no credential forwarding), so supporting
  it needs a deliberate net extension (a per-request bearer token that is never
  forwarded across hosts). Follow-up; not done here.
- Status words (`signed`, `pending`, `confirmation_required`,
  `confirmation_wrong`, `rejected`), field names (`transaction_id`,
  `confirmation_kind`, `confirmation_code`, `retries_left`, `request_id`,
  `identity_id`, `algorithm`, `digest`, `privacy`, `document`, `signature`,
  `certificate_chain`, `display_name`) and error codes
  (`certificate_not_qualified`, `confirmation_locked`, `transaction_expired`,
  `invalid_grant`, `invalid_client`).
- Algorithm names are Rivet's own `toString(SignatureAlgorithm)`; ECDSA
  signatures are expected DER-encoded, RSA PKCS#1 v1.5 raw.
- A real DSS may require its own SOAP or document-based operations, different
  confirmation channels or a different certificate model.

## Not done

Real DSS interoperability, bearer-token header support in the net layer,
persistent token or credential storage, CLI / UI wiring, GOST end to end,
QSCD-aware classification, any claim about legal qualified status.

## Related

ADR-0042 (network layer), ADR-0037 / ADR-0038 (PDF signing), classification in
`crypto/Qualification.hpp`.
