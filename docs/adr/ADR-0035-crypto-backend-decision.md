# ADR-0035: Crypto backend decision

Status: Accepted

Date: 2026-10-17

## Decision

The portable software backend is **OpenSSL 3.x** (`find_package(OpenSSL 3)`,
optional via `RIVET_ENABLE_CRYPTO`). Only documented public APIs are used and
the code is kept compatible with 3.x (it also builds against 4.x). GOST and
qualified (QES) production signing are **deferred** to later CryptoPro CSP and
PKCS#11 workstreams behind the same provider interfaces. `gost-engine` is not
used. Windows CNG and macOS Security.framework remain possible future
platform providers for key storage; they are not the portable backend.

## Comparison

| Axis | OpenSSL 3 | Botan | Windows CNG | macOS Security.framework | CryptoPro CSP | PKCS#11 |
|---|---|---|---|---|---|---|
| RSA / ECDSA | yes | yes | yes | yes | yes | token-dependent |
| GOST R 34.10/11-2012 | only via third-party gost-engine | no (partial, not production) | no | no | yes (certified) | token-dependent |
| CMS / PKCS#7 | full (we use only primitives) | partial | partial | via CMS APIs, Apple-only | yes | not applicable |
| PKCS#12 | yes | yes | partial | import only | yes | not applicable |
| X.509 parsing | yes | yes | yes | yes | yes | not applicable |
| OCSP / CRL | yes | partial | yes | yes | yes | not applicable |
| RFC 3161 | yes | no | no | no | yes | not applicable |
| Licensing | Apache-2.0 | BSD-2 | OS component | OS component | proprietary, per-seat | module licensed by token vendor |
| Size / footprint | large, one shared dependency | medium, C++ native | none (OS) | none (OS) | external install | external driver |
| Hardware keys | via PKCS#11 provider only | limited | smart cards via CNG | smart cards via CryptoTokenKit | yes | yes (its purpose) |
| Portability | all targets | all targets | Windows only | macOS only | Windows/macOS/Linux, vendor build | all, driver per vendor |

## Rationale

- OpenSSL covers every portable need of this phase (RSA, RSA-PSS, ECDSA,
  SHA-2, X.509, PKCS#12, key generation) on all three desktop platforms with a
  permissive license and a ubiquitous, audited implementation.
- Botan is a credible alternative but adds a second C++ dependency without
  covering RFC 3161 or GOST; OpenSSL is already present on every target.
- CNG and Security.framework are platform-specific; using them as the base
  would fork behaviour per OS and violate the portable-layer rule.
- GOST and QES need certified implementations (CryptoPro CSP) or hardware
  tokens (PKCS#11); the provider interfaces already model both, including
  blocking sign calls and cancellation.

## Consequences

- `RIVET_ENABLE_CRYPTO` defaults OFF; CI and developers opt in where OpenSSL
  is installed. With it OFF the null provider reports `BackendUnavailable`.
- No OpenSSL type appears in a public Rivet header.
- Adding CryptoPro or PKCS#11 later is a new `CryptoProvider` /
  `SigningIdentity` implementation, not a model change.
- OpenSSL becomes a runtime dependency of builds that enable crypto; its
  version is reported at configure time.
