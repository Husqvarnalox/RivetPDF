# ADR-0041: PKCS#11 provider and platform key stores

Status: accepted (PKCS#11 section). The platform-store section is appended below the marker by its own change.

## Part 1: Generic PKCS#11 provider

### Context

Smart cards and USB tokens (Rutoken, JaCarta, SoftHSM, Yubikey PIV through OpenSC, ...) expose private keys only through the vendor's PKCS#11 (Cryptoki) module. Rivet needs a signing identity backed by such a token without linking against any vendor SDK, without copying proprietary headers into the repository and without ever exporting a private key.

### Decision

`src/crypto/providers/pkcs11` implements `IdentityStore` (see ADR-0034) for any PKCS#11 v2.40 module.

- **Run-time loading.** The module path is configuration (`Pkcs11Options::modulePath`). It is loaded with `dlopen` (`LoadLibraryW` on Windows). The only symbol resolved by name is `C_GetFunctionList`; every other entry is called through the returned table. A NULL table entry yields `CKR_FUNCTION_NOT_SUPPORTED`, never a null call, so partial modules (some vendor modules omit optional functions) degrade instead of crashing.
- **Own ABI header.** `Pkcs11Abi.hpp` is written from the specification: types, the used structs, the used constants and the complete 68-entry `CK_FUNCTION_LIST`. It is private to `src/crypto/providers/pkcs11`; no PKCS#11 type appears in a public header. Structure packing follows the spec (1-byte packing on Windows, natural alignment elsewhere).
- **Build switch.** `RIVET_ENABLE_PKCS11` defaults to OFF. OFF compiles only a factory whose store reports `isAvailable() == false` and fails `list()` / `open()` with `BackendUnavailable`. ON defines `RIVET_CRYPTO_PKCS11=1` and links `${CMAKE_DL_LIBS}`. Both modes build with `-Werror`.
- **Module lifetime.** One `Pkcs11Module` per normalised library path is shared process-wide while any store holds it, so `C_Initialize` / `C_Finalize` are paired exactly once. `CKR_CRYPTOKI_ALREADY_INITIALIZED` means another component owns the library: it is shared and never finalized by Rivet. Finalize and `dlclose` happen under the cache mutex so a concurrent load of the same path cannot see a half-finalized library.
- **Loading is lazy** (first `isAvailable()` / `list()` / `open()`), so constructing a store never blocks or fails. A missing file, a file that is not a shared library, a library without `C_GetFunctionList` and a failing `C_Initialize` all produce `isAvailable() == false` with a reason text and no crash.

### Threading

- All calls into a module go through a per-module call gate (a mutex held for a single call). Many vendor modules are not thread safe in practice even when they accept `CKF_OS_LOCKING_OK`.
- The gate and every other lock are never held while a `PinPrompt` callback runs. Prompts run on the thread that runs `sign()` (a `runCryptoAsync` worker), never on the main thread.
- `Pkcs11Session` is RAII: a session never outlives the call that opened it.

### Identity model

- `list()` opens a read-only session per token with a present token, reads certificate objects and pairs them with private keys by `CKA_ID`. Listing never logs in and never prompts.
- **Identity id** (opaque, stable while the token is the same): `<slot>/<sanitised token serial>/<hex CKA_ID>`, with `:dupN` appended for several certificates sharing a `CKA_ID`, and `-<ordinal>` instead of the id when `CKA_ID` is empty (such an identity lists but `open()` fails with `NoPrivateKey`). The slot number is only a hint: `open()` and `sign()` locate the token by serial, so a token moved to another reader is still found.
- Most tokens hide private objects until login. `privateKeyAvailable` is therefore also true when a public key object with the same `CKA_ID` exists and the token requires login. A token with a certificate and neither key reports `privateKeyAvailable = false` and `open()` returns `NoPrivateKey`.
- CA-only certificates without a key are not offered as identities; they are used (bounded to 10) to build `chainDer()` from certificates found on the same token.
- Expired certificates are listed with `certificateExpired = true`. Selection policy belongs to the application.
- `KeyLocation` is `Token` with the token label and the slot description. `assurance()` is `HardwareToken`.

### Signing

- Each `sign()` opens its own session, logs in lazily and closes the session at the end. The identity keeps neither a PIN nor a login between signatures; closing the last session logs the token out. Each signature therefore asks for the PIN again (documented behaviour; a PIN cache would mean retaining the secret).
- **PIN handling.** The PIN exists only in a `SecretBuffer`, is passed to `C_Login` and is zeroed immediately afterwards. It is never logged, formatted into a message, or stored. Error messages contain the PKCS#11 return code as a hexadecimal number and a short phrase only.
- **Retry policy.** Up to 3 PIN attempts per login (`PinPromptReason::Required`, then `Incorrect`, then `LastAttempt` when the token reports `CKF_USER_PIN_FINAL_TRY`), then `WrongPassword`. Cancelling the prompt, or having no prompt, gives `Cancelled`. `CKR_PIN_LOCKED` or `CKF_USER_PIN_LOCKED` gives `PinLocked` without prompting. Tokens with a protected authentication path (`CKF_PROTECTED_AUTHENTICATION_PATH`, PIN pad) log in without a prompt.
- **Recovery.** A session-class failure (`CKR_SESSION_HANDLE_INVALID`, `CKR_SESSION_CLOSED`, `CKR_USER_NOT_LOGGED_IN`) triggers exactly one retry with a fresh session and a forced login. A second failure is reported as `SessionExpired`. `CKR_DEVICE_REMOVED`, `CKR_TOKEN_NOT_PRESENT` and an unknown slot map to `DeviceRemoved`.
- **Cancellation.** `CancelToken` is polled before every module call that can be skipped. A call already inside the module completes and its result is discarded as `Cancelled`.

### Mechanism selection

The token's mechanism list (`C_GetMechanismList`) is read once; when the entry is missing or fails, every mechanism is treated as possibly available and the token's own error decides.

| Algorithm | Preferred mechanism | Fallback |
| --- | --- | --- |
| RSA PKCS#1 v1.5 SHA-256/384/512 | `CKM_SHA{256,384,512}_RSA_PKCS` | hash in software, `CKM_RSA_PKCS` over a DigestInfo |
| RSA-PSS SHA-256/384/512 | `CKM_SHA{256,384,512}_RSA_PKCS_PSS` | hash in software, `CKM_RSA_PKCS_PSS` |
| ECDSA SHA-256/384/512 | `CKM_ECDSA_SHA{256,384,512}` | hash in software, `CKM_ECDSA` |

- PSS uses `CK_RSA_PKCS_PSS_PARAMS` with MGF1 over the same digest and salt length equal to the digest length (the only length Rivet creates).
- ECDSA tokens return raw `r || s`; the provider re-encodes it as a DER `ECDSA-Sig-Value` (CMS and CAdES require DER).
- `signDigest` signs an externally computed digest: RSA PKCS#1 through `CKM_RSA_PKCS` with the DigestInfo prefix, PSS through `CKM_RSA_PKCS_PSS`, ECDSA through `CKM_ECDSA`.
- The creation policy (no SHA-1, no RSA below 2048) is enforced in the provider as defence in depth.

### GOST mechanisms: vendor-specific, check your SDK

GOST R 34.10-2012 mechanism numbers live in the `CKM_VENDOR_DEFINED` space and are not standardised by OASIS. Different vendors (and different versions of the same SDK) use different values and different parameter conventions. Rivet therefore ships **no built-in GOST mechanism values**. The application supplies the values documented by its token SDK through `GostMechanisms`:

- `sign256` / `sign512`: the token signs a precomputed 32 / 64 byte digest;
- `signWithDigest256` / `signWithDigest512`: the token hashes the message itself;
- `digest256` / `digest512`: the token-side Streebog digest mechanisms.

A value that is not configured disables the corresponding GOST path and the algorithm is not listed in `supportedSignatures()`. This is untested against any real GOST token in this repository: no GOST token or SDK is available to the build, and the mock is RSA / ECDSA only. Treat GOST over PKCS#11 as unverified until a real device has been exercised. The CryptoPro CSP provider (ADR-0040) is the supported GOST route on macOS and Windows.

### ABI cross-check

The ABI header was written from the specification and then compared structurally against the CryptoPro-supplied headers in `/opt/cprocsp/include/cppkcs11/` (`pkcs11.h`, `pkcs11t.h`, `pkcs11f.h`; not copied into the repository, they are proprietary). A scratch program (kept outside the repository) compiled both definitions on LP64 macOS arm64 and compared `sizeof` / `offsetof` of 11 structs, the offsets of all 68 `CK_FUNCTION_LIST` entries and the values of 100 constants. All 233 comparisons were identical. The CryptoPro headers are a v2.20-era variant: they name the last `CK_C_INITIALIZE_ARGS` field `LibraryParameters` and the flag `CKF_OS_LOCKING_OK`, with no `CKF_OS_LOCKING_THREADS`; Rivet uses the spec flag name `CKF_OS_LOCKING_OK` and the layout is the same. `Pkcs11Abi.hpp` also carries `static_assert`s of the LP64 sizes. The comparison was not run on Windows (1-byte packing) or on 32-bit targets.

### Testing

- `tests/crypto/providers/mock_pkcs11` builds `rivet_mock_pkcs11`, a SHARED library exporting `C_GetFunctionList` and a test control function `rivet_mock_set_scenario(const char*)`. It is a software token (OpenSSL EVP, RSA-2048 and ECDSA P-256, PIN `1234`) with scenarios: `ok`, `wrong-pin`, `locked`, `token-removed`, `session-expired-once`, `session-expired-always`, `key-missing`, `expired-cert`, `no-slots`, `init-fails`, `already-initialized`, `raw-mechanisms`, `no-mechlist`, `sparse` (NULL table entries), `open-token`. It is built only when `RIVET_ENABLE_PKCS11` and `RIVET_ENABLE_CRYPTO` are ON.
- `TestPkcs11.cpp` drives the real provider against that module (listing and key pairing, RSA PKCS#1 / PSS / ECDSA signatures verified with `EVP_DigestVerify` against the certificate's public key, PIN retry and exhaustion, cancel, lock, removal, session recovery, missing key, expired certificate, no slots, module sharing and `C_Finalize` pairing, no PIN in any message, `runCryptoAsync`).
- Tests that need an external module print `SKIP` and pass when it is absent: an enumeration-only smoke test of `/opt/cprocsp/lib/libcppkcs11.dylib`, and a SoftHSM2 test (`SOFTHSM2_MODULE`, or the usual Homebrew / Debian paths, `softhsm2-util` with `SOFTHSM2_CONF` in a temporary directory) that initialises a token and lists it. A fresh SoftHSM2 token holds no certificate, so that test proves module loading, initialisation and enumeration, not signing.
- **What the mock proves and what it does not.** The mock proves that the provider's call sequence, error mapping, PIN handling and encodings are internally consistent and match the PKCS#11 specification as the mock reads it. It does not prove interoperability with any vendor module. Real tokens differ in mechanism lists, attribute visibility, session limits, behaviour of `C_Login` after removal and in their handling of `CKA_ID`. Before relying on a particular token, run the provider against it.

### Consequences

- Any PKCS#11 token works without a vendor SDK at build time; the application chooses the module path.
- Without `RIVET_ENABLE_PKCS11` nothing is loaded and nothing changes for existing builds.
- A signature asks for the PIN each time. That is deliberate (no retained secret) and may be revisited with an explicit, user-visible session policy.
- Vendor modules run in-process: a crashing module crashes Rivet. Out-of-process isolation is a possible later hardening.

<!-- platform-stores-section-below -->

## Part 2: Platform key stores

### Decision

`makePlatformIdentityStore()` (`src/crypto/providers/platform/`) returns an
`IdentityStore` with assurance `PlatformKeystore`. Keys stay in the OS store
and are non-exportable; Rivet only asks the OS to sign.

- **macOS** (`MacKeychainStore_macos.mm`, Security.framework): enumerates
  identities (certificate plus private key) from the default keychain search
  list; signs RSA PKCS#1, RSA-PSS and ECDSA over messages or digests. The OS
  shows its own access prompt, so no `PinPrompt` is used. GOST keys are
  unsupported (typed error). The chain is whatever SecTrust returns, so an
  untrusted private CA yields a leaf-only chain.
- **Windows** (`CngStore_windows.cpp`, certificate store plus CNG / NCrypt):
  written to the same contract but UNTESTED: it was reviewed only, has not been
  compiled or run, and is in no CI job. Treat it as a draft until a Windows
  build exercises it.
- **Linux**: no platform keystore. The store reports `isAvailable() == false`
  with a reason; use PKCS#11 or software keys.

### Testing

The macOS test creates a temporary keychain non-interactively, imports a
throwaway PKCS#12 with RSA-2048 and EC P-256 identities, signs with every
supported algorithm, verifies the signatures with OpenSSL, and removes the
keychain. It uses the deprecated `SecKeychain*` APIs (suppressed locally) and
skips if a future macOS removes them. The Windows path has no tests.
