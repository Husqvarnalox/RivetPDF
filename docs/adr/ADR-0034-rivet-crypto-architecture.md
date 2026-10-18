# ADR-0034: Rivet crypto architecture

Status: Accepted

Date: 2026-10-17

## Decision

Cryptography lives in `src/crypto` (`rivet::crypto`), a portable static
library with no AppKit and no PDFium dependency. Rivet owns the typed API and
the semantics; private-key math is never home-grown and is always done by a
vetted provider behind an interface.

**Typed vocabulary.** `DigestAlgorithm`, `SignatureAlgorithm` and
`SymmetricAlgorithm` (model only, nothing encrypts yet) are enums; no
algorithm is selected by string. `SignatureAlgorithm` can only express what
Rivet may *create* (SHA-2 with RSA PKCS#1, RSA-PSS, ECDSA, and the declared
GOST identifiers). `SignatureScheme` describes what may be *verified* and can
also express SHA-1, so legacy signatures remain verifiable.

**Errors.** `CryptoResult<T>` is `std::expected<T, CryptoError>` with a typed
`CryptoErrorCode` (WrongPassword, CorruptData, NoPrivateKey, WeakAlgorithm,
MalformedCms, LimitExceeded, Cancelled, BackendUnavailable, ...). Messages
never carry secrets or content. `toCoreError` bridges to `core::Error`.
Passwords and PINs live in `SecretBuffer` (move-only, zeroed on destruction,
no formatting).

**Providers.** Interfaces hide every backend type: `CryptoProvider`,
`SigningIdentity` (opaque handle, key never exported through it),
`CertificateStore`, `CertificateValidator`, `TimestampProvider`,
`RevocationProvider` and `RemoteSigningProvider`. The last three are
interface-only in this phase. A null provider keeps the build green when no
backend is compiled in (`RIVET_ENABLE_CRYPTO=OFF`, the default): every
operation then returns `BackendUnavailable`.

**CMS is Rivet-owned.** `Der.*` is a strict, bounded DER/BER reader (depth 24,
length fields of at most 4 bytes, no high tag numbers, indefinite length only
for constructed elements, no stray end-of-contents) and a small writer.
`Cms.*` parses and generates RFC 5652 SignedData, CAdES-BES
(signing-certificate-v2), detached and attached, file-streamed detached
digests, co-signing and countersignatures. A parsed model re-encodes its
signed attributes byte for byte, so existing signatures survive co-signing.
Duplicate signed attributes are rejected. The backend is used for hashing,
signature creation/verification, X.509 parsing and PKCS#12 only; interop with
OpenSSL's own CMS is covered by tests (API and the `openssl cms` CLI).

**Policy.** Creating SHA-1/MD5 signatures or signing with RSA below 2048 bits
is refused (`WeakAlgorithm`), in `signCms` and again inside the identity.
Verifying such signatures is allowed and reported as "weak legacy algorithm".

**Validation is not a bool.** `ValidationResult` reports crypto, integrity,
certificate, trust, timestamp, revocation and policy status separately and
folds them (`foldState`) into one `SignatureState` with a fixed precedence
(Revoked, DocumentModified, InvalidSignature, UnsupportedAlgorithm,
UnableToValidate, WeakLegacy, expired/timestamp, trusted/untrusted). Trust
comes from an explicit `TrustAnchorSet`; Rivet ships no roots.

**Certificates.** `CertificateInfo` is backend-independent and carries the
Russian INN/SNILS/OGRN attributes only when present in the subject.
`buildChain` links certificates through `provider.isIssuedBy` (real signature
verification), requires CA issuers and bounds the chain at 10.

**Threading.** `runCryptoAsync` runs work on `core::TaskScheduler`, fences it
with `core::AsyncScope`, delivers the result once through
`core::IMainThreadDispatcher`, never invokes callbacks under a lock, and
reports cancellation (`CancelToken`) as `Cancelled`. Blocking operations
(hardware PIN prompts, remote confirmation) must go through it.

**Tooling.** `rivet-signature-inspect` dumps and verifies a CMS file.

## Deferred

Production GOST / qualified signing (CryptoPro CSP, PKCS#11 tokens), RFC 3161
and OCSP/CRL implementations, remote signing transports, PDF signature
embedding (a separate workstream that consumes this layer) and symmetric
encryption.

## Consequences

- One small dependency (OpenSSL 3.x) behind an option; builds without it.
- GOST signatures are declared but report `UnsupportedAlgorithm` until a
  GOST-capable provider is added; nothing in the model needs to change.
- Rivet carries its own CMS reader/writer to maintain, in exchange for
  bounded, auditable parsing and byte-faithful co-signing.
- A damaged PKCS#12 whose MAC fails is reported as `WrongPassword`; only a
  structural parse failure is `CorruptData`.
