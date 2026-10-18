# Phase 8 closure report

Branch `phase8-crypto-release`, PR #4. CI green on all six jobs at d264e78: linux-gcc, linux-pdfium,
macos, macos-universal, windows-msvc-x64, windows-msvc-pdfium. Local gate (p-off, p-on, p-asan,
p-tsan): zero warnings, ctest green.

On 2026-10-18 the owner decided to close Phase 8 without real DSS credentials, hardware tokens, a
CryptoPro licence and Windows QA. Those items are listed below as **not verified**. This is a scope
decision, not a verification result.

## Implemented
- Network layer (ADR-0042): http/https only, SSRF guard, per-hop redirect checks, consent gate, libcurl behind `RIVET_ENABLE_NETWORK`.
- RFC 3161 time-stamps (ADR-0043); OCSP/CRL, fail-closed (ADR-0044).
- LTV (ADR-0045): `/DSS` + VRI, upgrade and archive DocTimeStamp renewal.
- PDF encryption (ADR-0046): AES-256 (V5/R6) and AES-128 (V4/R4), constrained post-save encryptor.
- Cloud QES client (ADR-0047): CryptoPro DSS adapter, against a mock only.
- Signature and password UI controllers (ADR-0048): tested with fake views only.
- Module-trust checks before `dlopen`; verifier resource budgets; 9 libFuzzer targets with replay tests.
- Security review: `docs/security/phase8-review.md`.

## Verified
- Unit, integration and fuzz-replay tests on all CI platforms (macOS, Ubuntu/GCC, Windows/MSVC with OpenSSL).
- Real PKCS#11 signing through SoftHSM2 on Linux CI (the job fails if the test skips).
- 120 s libFuzzer smoke run per target: no product crash.
- Sanitizers (ASan, TSan) locally, 0 findings.
- Windows file signing: found and fixed on CI (temp-file sharing mode and stale directory size in the atomic writer; test left a stream open).

## External interop
- Acrobat: BLOCKED, interactive access denied by user.
- OpenSSL `cms`/`ts`, `pdfsig`, qpdf, mutool: not installed on the CI runners or locally, tests SKIP. Not verified.
- Live TSA, live OCSP/CRL, real CA: not verified (mocks and local test CAs only).

## CI
Six of six jobs pass. Crypto is ON on linux, macOS and both Windows jobs; OFF on macos-universal.

## PAdES B-B / B-T / B-LT / B-LTA, CAdES
Produced and self-verified by Rivet's own verifier. No third-party validator was run.

## CryptoPro
`CryptoProApi.cpp` has never been built, since the SDK is absent. No real CryptoPro signing. Not verified.

## PKCS#11
SoftHSM2 only. Rutoken, JaCarta: not verified.

## DSS
Cloud QES implementation complete. Real cloud QES E2E: BLOCKED, credentials unavailable.

## Encryption
Output checked by Rivet's own reader and tests. No qpdf, mutool or Acrobat cross-check. PDFium cannot
write encrypted PDFs, so the constrained encryptor refuses signed input and several structures.

## Security
See `docs/security/phase8-review.md`. Open, low priority: legacy `std::string` password fields not wiped; a race between module check and `dlopen`; no ESIC, delta CRL or `nameConstraints`.

## Not verified (accepted by the owner as out of closure scope)
Real DSS credentials; hardware tokens; a valid CryptoPro licence; Windows QA; Acrobat interop; live
TSA/OCSP/CA; GUI visual check (the GUI was never launched); the Windows `ModuleTrust` branch.

## Remaining blockers
None for the accepted scope. Not delivered: release packaging, Apple notarization, Windows code-signing, OCR parity.

## Verdict
Phase 8 — CLOSED ✅ / Rivet 0.1.0 Alpha READY — within the scope above, as decided by the owner. The "READY" part
holds for the code state only: no installer, notarized or signed binary exists.
