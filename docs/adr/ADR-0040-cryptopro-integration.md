# ADR-0040: CryptoPro CSP integration

Status: Accepted

Date: 2026-10-17

## Context

GOST R 34.10-2012 signing keys are mostly held by CryptoPro CSP (key
containers, smart cards, tokens). Rivet needs a provider that enumerates those
identities, signs with Streebog (GOST R 34.11-2012) and verifies GOST
signatures, without making the CSP a build or run dependency of Rivet.

## Decision

1. **dlopen, not link.** `libcapi10` (CAPI / CryptoAPI 1.0 entry points) and
   `libcapi20` (certificate stores, CAPILite) are loaded at run time from
   `CryptoProOptions::libraryDirectory` or the standard `/opt/cprocsp/lib`
   locations. One Rivet binary therefore works with or without a CSP; without
   it the factories return an unavailable store (`isAvailable() == false`,
   `BackendUnavailable`, a reason string) and a null provider. Libraries are
   never unloaded (the CSP keeps process-wide state). `GetLastError` is
   resolved from `libcapi10` (it is exported there on Unix).
2. **SDK headers are private and not vendored.** They are proprietary. The
   build (`RIVET_ENABLE_CRYPTOPRO=ON`, default OFF) needs
   `RIVET_CRYPTOPRO_ROOT` (default `/opt/cprocsp`) containing
   `include/cpcsp/WinCryptEx.h`, adds them as `-isystem` to exactly two
   translation units (`CryptoProApi.cpp`, `CryptoProBackend.cpp`), and no CSP
   type appears in a Rivet header. The compile definitions `UNIX` and
   `SIZEOF_VOID_P` are set on the same two files.
3. **Public surface.** `makeCryptoProIdentityStore()` (an `IdentityStore`,
   assurance `CertifiedCspDeclared`), `makeCryptoProProvider()` (Streebog
   hashing, GOST verification, certificate parsing through the backend-free
   `parseX509`, CSP-computed fingerprints) and `GostDigestVerifier` (verify a
   signature over a precomputed digest). The provider implements
   `CryptoProvider` because `signCms` hashes through the provider; it refuses
   PKCS#12 import, self-signed creation and export (`NotExportable`: CSP keys
   are never exported).
4. **Enumeration.** The user's `MY` store is read with `CertOpenSystemStoreA`;
   a certificate is an identity when it has a `CERT_KEY_PROV_INFO` link to a
   GOST provider type (80 / 81). `privateKeyAvailable` is true when the
   container opens silently and holds the key (`CryptGetUserKey`). The id is
   `cryptopro:<sha256 fingerprint>`.
5. **Signing and PIN.** Every attempt acquires a fresh `CRYPT_SILENT` context
   and signs; if the CSP reports that a PIN is needed or wrong, the injected
   `PinPrompt` is called (no lock held), the PIN is set with
   `PP_SIGNATURE_PIN` / `PP_KEYEXCHANGE_PIN` from a `SecretBuffer`, and the
   attempt repeats (at most 3 PINs). Error mapping: wrong PIN to
   `WrongPassword`, blocked PIN to `PinLocked`, card or reader gone to
   `DeviceRemoved`, user cancel or a cancelled token to `Cancelled`, missing
   container to `NoPrivateKey`. `signDigest` sets the hash value directly
   (`HP_HASHVAL`). Callers run `sign` through `runCryptoAsync`; one mutex
   serialises CSP calls.
6. **Signature byte order.** The CSP's native signature is little-endian
   `r||s`. The value carried in CMS / X.509 is its byte reversal, i.e. `s||r`
   with both numbers big-endian (RFC 4491 layout). This was measured against
   the real CSP: the RFC 7091 appendix A.1 signature verifies through
   `CryptVerifySignature` if and only if it is presented as `s||r` big-endian
   and reversed by the provider (the test tries the four orderings; exactly
   one is accepted). Digest values keep the CSP output order, which matches
   the RFC 6986 vectors read as byte strings.
7. **Chain.** The identity chain is built from the `CA`, `ROOT`, `MY` stores
   plus `CryptoProOptions::extraChainCertificates` with the shared
   `buildChain`; `isIssuedBy` verifies the TBS signature through the CSP. Rivet
   does not decide trust here (empty anchor set): trust and legal status stay
   with the validation and classification layers (ADR-0039).
8. **Windows.** Not supported by this provider in this pass (the LoadLibrary
   path and `.dll` names are not written); the Windows CNG store (ADR-0041)
   covers certificates in the Windows store. Unix and macOS only.

## Legal status

Using this provider does not make a signature a qualified one. The provider is
declared `CertifiedCspDeclared`, which is an input to classification only;
Rivet does not check that the installed CSP build is certified, that its
licence is valid, or that the certificate was issued by an accredited CA.
Legal status is outside the software (ADR-0039).

## Verification status (what was and was not checked)

Against the real CryptoPro CSP 5.0 KC1 (macOS arm64), automated tests pass:
Streebog-256/512 of the empty message and RFC 6986 M1 / M2, streaming equals
one-shot, GOST R 34.10-2012 256-bit verification of the RFC 7091 A.1 vector
including tamper rejection and the byte-order measurement above, store
enumeration (empty store), unknown-id and cancellation behaviour, and the
unavailable-backend path.

Not verified, and why:

- **Signing with a real container, PIN flow, `cryptcp` / `cpverify` / `csptest`
  cross-verification of Rivet signatures, CAdES-BES CMS verified by `cryptcp`.**
  The test machine's CSP licence is expired ("License is expired or not yet
  valid" in `cpconfig -license -view`) and key generation could not complete
  non-interactively: creating a container needs the CSP random generator, whose
  silent mode fails with `NTE_SILENT_CONTEXT` (0x80090022) and whose interactive
  mode needs keyboard / mouse input (a pty with synthetic keystrokes did not
  finish). The signing code path is therefore written to the CAPI contract but
  unexercised; `cryptoProSignAndVerifyWithOperatorContainer` runs it when an
  operator supplies a throwaway container and `RIVET_TEST_CRYPTOPRO_PIN`, and
  prints SKIP otherwise. No key material is committed.
- The 512-bit RFC 7091 signature vector is not included.
- `isIssuedBy` against real CA certificates; the licence-expiry error code
  (no distinct code is documented in the SDK headers), so `LicenseInvalid` is
  not yet produced by this provider.
- Smart-card / token paths (`DeviceRemoved`, PIN lock) are mapped from SDK
  error constants but untested.

## Consequences

Rivet gains GOST signing and verification on machines with a CSP at no cost
elsewhere. The CSP licence and RNG are operator responsibilities. Windows
support and a documented licence check are future work.
