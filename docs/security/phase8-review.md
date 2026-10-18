<!-- SPDX-License-Identifier: MPL-2.0 -->
# Phase 8 security review (network, crypto, signatures, encryption)

Scope: `src/net`, `src/crypto`, `src/pdf/sign`, `src/pdf/encrypt`, `src/core/io/AtomicFileWriter`.
Method: source inspection of the cited lines plus the fuzz harnesses in `tests/fuzz/`. Line numbers are approximate (the code moves; re-grep before relying on one) and refer to the
tree at the commit that added this file. Nothing here was checked against a real vendor token, a real CryptoPro CSP,
a real DSS service or a live TSA/OCSP responder; all of that is listed under "Not verified".

"Verified" means: I read the cited code and, where stated, a test or fuzz run exercises it. It is not a proof.

## Fuzzing evidence

Harnesses (`tests/fuzz/`): DER/X.509/Certificate, CMS, RFC 3161 token, OCSP + CRL, PDF signature/ByteRange/DSS reader,
PDF encrypt input scanner, net (Url/Ip/Http/redirect-SSRF policy), JsonLite. Seeds are generated at run time from the
test-support builders; nothing is committed.

- libFuzzer (Homebrew LLVM clang 23.1.3, `-fsanitize=fuzzer,address,undefined`, built with `-DRIVET_BUILD_FUZZERS=ON`):
  every target ran about 120 s. Executions: cms 1.14M, der 1.10M, json 2.00M, net 3.69M, pdfencrypt 0.10M,
  revocation 0.86M, timestamp 1.57M, x509 1.94M, pdfsig 0.17M (second run, see below).
  No crash, hang, timeout or OOM in the target code. The corpora were not kept (they live in a scratch directory).
- One crash was reported by the pdfsig target. It was a false positive of the harness invariant (it demanded that a
  "covers whole file" ByteRange end exactly at EOF, while the verifier deliberately accepts trailing whitespace,
  `PdfSignatureVerifier.cpp:1059-1067`). The invariant was relaxed to "whitespace only after the ByteRange" and the
  saved crash input now replays clean. No product bug.
- Harness bugs found while bringing the targets up (not product bugs): an inverted net `checkUrl` invariant and a DER writer
  round-trip assumption. Both were harness-side.
- Deterministic replay tests (`fuzz_replay_*`, label `fuzz`, part of `ctest -LE slow`): seeds plus a bounded xorshift
  mutation loop, no libFuzzer needed (works with GCC/MSVC). About 6 s total under ASan/UBSan. `fuzz_replay_long_*`
  (200000 mutations, label `slow`) are opt-in and were NOT run to completion.
- The fuzzers do not reach: real TLS, real sockets, libcurl, PKCS#11/CSP modules, PDFium. 60 to 120 s per target is a smoke
  run, not a campaign; coverage of deep chain-building paths relies on self-issued certificates being added as anchors.

## Findings fixed in this change

| # | Area | Defect | Fix | Test |
|---|------|--------|-----|------|
| F1 | PKCS#11 loading | `Pkcs11Module::acquire` accepted a relative module path, so the module was resolved against the working directory / loader search path (library planting). | Relative paths are refused (`Pkcs11Module.cpp`, `acquire`). | `relativeModulePathIsRefused` |
| F2 | CryptoPro loading | `CspApi::load` accepted a relative `libraryDirectory`. | Relative directories are refused (`CryptoProApi.cpp`, `load`). | `cryptoProRelativeLibraryDirectoryIsRefused` |
| F3 | Encrypt secrets | Passwords, file key and R6/R4 intermediate hashes were never zeroised in `src/pdf/encrypt`. | RAII `Wipe` guards on every secret intermediate and error path (`StandardHandler.cpp:40`, `:257`, `:331`, `:403`), `EncryptionSetup` zeroes the file key (`:306`), object keys wiped (`:542`); `EncryptSecrets` + `encryptPdf(pdf, options, secrets)` take passwords in `SecretBuffer` (`PdfEncrypt.hpp:79`, `PdfEncrypt.cpp:714-718`). | `secretsOverloadMatchesStringOverloadAndIgnoresOptionFields`, `secretsOverloadErrorPathsAreTypedAndLeaveBuffersIntact`, `preparedPasswordsAreWipedByTheCallersGuardContract` |
| F4 | System proxy vs SSRF | With `useSystemProxy` the connect-time peer check sees the proxy, not the target. | `checkProxyDestination` refuses host names unless `allowedHosts` is set (IP literals still classified), called from `checkUrl` (`Policy.cpp:84,96`); proxy address policy via `allowPrivateProxy` (`CurlTransport.cpp:154`); ADR-0042 addendum. | `netPolicySystemProxyKeepsTheSsrfInvariant`, `netClientSystemProxyDoesNotBypassDestinationPolicy` |
| F5 | Module trust | No ownership/permission/symlink checks before `dlopen`. | `checkModuleFile` (`ModuleTrust.cpp:71`): absolute path, symlinks resolved, regular file, owner root/euid, not group/other-writable, parent not world-writable (sticky allowed); Windows best effort. Used in `Pkcs11Module.cpp:122` and `CryptoProApi.cpp:28`; override `Pkcs11Options::allowInsecureModulePath` / `CryptoProOptions::allowInsecureLibraryPath` (default false). | `tests/crypto/TestModuleTrust.cpp` (6 POSIX tests, 1 Windows) |
| F6 | OCSP delegated responder | A delegated responder without `id-pkix-ocsp-nocheck` was accepted. | Rejected fail-closed with `EvidenceFailure::ResponderRevocationUnchecked` (`Ocsp.cpp:289-294`); EKU and validity checks unchanged. | `TestRevocation.cpp` (`responderNoNocheck` case) |
| F7 | Verifier budgets | Only fixed constants bounded signature count and chain search. | `VerifyLimits` (`PdfSignatureVerifier.hpp:42`): maxSignatures 256 (`PdfSignatureVerifier.cpp:933-975`, `LimitExceeded`), DSS cert/CRL/OCSP caps (`:761-773`, `LtvInfo::limitExceeded`), TSA candidates (`:735`), `PathOptions::maxSearchSteps` (`PathBuilder.cpp:97`). | `documentWithTooManySignaturesIsRefusedWithAClearStatus`, `ltvBudgetsFailClosedWithClearReasons` |

## Area-by-area

### 1. Network: SSRF, redirects, downgrade, size and time limits
Checked:
- Scheme: https only unless `allowHttp` (`Policy.cpp:58-64`, default false `Policy.hpp:19`). Userinfo in URLs rejected
  (`Url.cpp:106`), URL length capped (`Url.cpp:82`), other schemes rejected (`Url.cpp:96`).
- Address policy: IP literals in any notation are normalised by the URL parser; IPv4 loopback, link-local/metadata,
  CGNAT, TEST-NETs, 6to4 relay, IPv4-mapped IPv6, 6to4 `2002::/16`, ULA and fe80::/10 are classified (`Ip.cpp:64-102`).
  Host names `localhost`, `.localhost`, `.local`, `.internal` refused (`Policy.cpp:52-81`).
- Connect-time check: libcurl `OPENSOCKETFUNCTION` classifies the real peer address after DNS and refuses unknown
  address families (`CurlTransport.cpp:111-134`, installed at `:199`). This is the DNS-rebinding defence.
- Redirects: libcurl `FOLLOWLOCATION=0` and `MAXREDIRS=0` (`CurlTransport.cpp:180,190`); the client drives each hop and
  re-runs `checkUrl` on it (`Client.cpp:42`), caps hops (`Client.cpp:35,69`), refuses https to http
  (`Client.cpp:72-73`), and drops the body on 303/301/302-POST (`Client.cpp:76-80`). Protocols restricted to http(s)
  (`CurlTransport.cpp:170-174`). `NETRC` ignored, HTTP auth off (`:178-180`).
- Limits: body cap enforced in the write callback (`:50-58`) and by `MAXFILESIZE` (`:189`), header cap 64 KiB (`:27,66-69`),
  total deadline across hops (`Client.cpp:36,47`), connect/total timeouts (`:187-188`). Defaults 4 MiB, 10 s, 30 s
  (`Policy.hpp:24-26`).
- Proxy: `useSystemProxy=false` makes libcurl ignore proxy environment variables (`Policy.hpp:21`, `CurlTransport.cpp:185`).
- Caller-set `Authorization` is rejected by `validateHeaders` (see `CryptoProDss.cpp:34-36`), so a redirect cannot carry it.
- Tests: `fuzz_replay_net`, 3.7M libFuzzer executions of the Url/Ip/policy invariants (IP literal in any notation is
  classified; a redirect never downgrades).
Result: sound by inspection and by fuzz of the pure logic.
Residual risk:
- Fixed (F4): with `useSystemProxy=true` host names need an `allowedHosts` list (the connect-time check sees the proxy, not
  the target); the default stays off. Remaining: a proxy that resolves an allowed host name to a private address is trusted.
- Redirected 307/308 POSTs re-send the body to the new (checked, https) host. The TSA and OCSP bodies are public data; the DSS
  client forbids redirects (`CryptoProDss.cpp:200,213,380`).
- libcurl itself and its TLS backend are trusted.

### 2. TLS verification
Checked: `SSL_VERIFYPEER=1`, `SSL_VERIFYHOST=2`, minimum TLS 1.2 (`CurlTransport.cpp:182-184`); no code path disables
verification (grepped `CurlTransport.cpp` for the `VERIFY`/`SSL_OPTIONS` options: only these three). The default
transport and the mock transport are separate (`DefaultTransport.cpp`, `MockTransport.cpp`).
Not verified: which CA bundle libcurl uses on each platform, certificate pinning (none), OCSP stapling, behaviour on a
libcurl built without TLS 1.2 (the option would fail and the setup error is surfaced via `setup`, `:161-165`).
Residual risk: platform trust store contents; no pinning for the DSS or TSA hosts.

### 3. PIN and secret handling, logging
Checked:
- `SecretBuffer` zeroises with `SecureZeroMemory` or a volatile loop plus compiler barrier (`SecretBuffer.cpp:15-26`),
  and wipes in the destructor (`:58`).
- PKCS#11: the PIN is requested without a lock held, never sent when empty (so it does not burn a retry), cleared right
  after `C_Login` (`Pkcs11IdentityStore.cpp:660-670`), attempts bounded (`:32,:655`), locked/final-try tokens handled
  (`:650-651,:672-677`), protected-authentication-path tokens never see a Rivet PIN (`:652-655`).
- CryptoPro: PIN held in a `SecretBuffer`, passed to `PP_*_PIN` (`CryptoProBackend.cpp:496-499`); the buffer is
  destroyed (zeroed) on scope exit, attempts bounded (`:25`).
- DSS: access token and request bodies are wiped after use (`CryptoProDss.cpp:117-119,299,306,318,321,373,384,396,399`);
  token only goes to an https URL without query (`:156-159`).
Not verified: that no UI/dialog layer (`src/app`, `src/ui`, being written concurrently) logs or retains a PIN; that
`log::` calls never print a URL with a token (no secrets are placed in URLs by the code reviewed, tokens go in the JSON
body); crash-dump and swap exposure (no `mlock`).
Fixed (F3): encrypt passwords, file key and intermediate hashes are wiped on all paths; callers can pass `EncryptSecrets`.
Residual risk: the `std::string` fields of `EncryptOptions` (legacy overload) and the caller's own copies are not owned by the
module and are not wiped; the UTF-8/Latin-1 conversion of an `ByteView` into libc/std containers inside OpenSSL-backed
provider calls may leave copies in provider memory; the CryptoPro PIN stays in memory for the whole sign call rather than
being cleared right after `setProvParam`. Low severity, local attacker with memory access only.

### 4. Temp files and atomic writes
Checked: `AtomicFileWriter` creates a hidden temp next to the destination with `O_CREAT|O_EXCL|O_CLOEXEC`, random
suffix, narrowed to 0600 immediately (`AtomicFileWriter.cpp:500-528`), follows symlinks to the real destination
(`:114-130,208-220`), uses `F_FULLFSYNC`/`fsync` (`:150-158`) and `FlushFileBuffers`/`ReplaceFileW` on Windows
(`:381,:400-417`), unlinks the temp on abort/destruction (`:540,549`). The signer writes the output through it, reads the
temp back, checks the original bytes are an exact prefix (hash compare) and that the new signature verifies before
`commit()` (`PdfSigner.cpp:984-1030`), so a failed or tampered signing never replaces the user's file.
Not verified: behaviour on network file systems, the Windows branch (not built here).
Residual risk: the temp file briefly exists with the umask-derived mode before `fchmod` (it is empty at that moment).

### 5. Cancellation
Checked: the transport polls the cancel token and deadline from the libcurl progress callback and aborts
(`CurlTransport.cpp:84-97,:229`); the client checks it between hops (`Client.cpp:56`); PKCS#11 and CryptoPro PIN loops check
it before and after the prompt (`Pkcs11IdentityStore.cpp:657,:667`, `CryptoProBackend.cpp:478`); DSS retries wait
cancellably (`CryptoProDss.cpp:204,224-225`); the offline LTV evaluation passes `options.cancel` into chain building
(`PdfSignatureVerifier.cpp:849`). Async wrappers use `AsyncScope` (`TimeStampClient.cpp:~120-135`).
Not verified: cancellation inside a single blocking PKCS#11 or CSP call (those cannot be interrupted; the call returns when
the token does), cancellation while a remote QES user is approving on a phone beyond the DSS poll loop.

### 6. Timestamp, OCSP and CRL: fail-closed logic
Checked:
- A revocation outcome is acceptable only if it is neither Revoked nor (Unknown with hard-fail)
  (`RevocationChecker.cpp:41-45`); any problem to obtain evidence collapses to Unknown with a reason
  (`:123-137`, `:279`), aggregated worst-wins (`:307-318`).
- Time: future `thisUpdate`/`producedAt` rejected, `nextUpdate < thisUpdate` rejected, stale evidence rejected, a missing
  `nextUpdate` bounded by `maxAge` (`Evidence.cpp:44-80`). Historical validation accepts a statement dated after the
  validation time (revocation would only be added, not removed, and a revocation after the time does not apply).
- OCSP: signer is the issuing CA or a delegated responder certificate issued by that CA, carrying id-kp-OCSPSigning and valid
  at `producedAt` (`Ocsp.cpp:259-297`), signature checked (`:298`), nonce must be echoed when sent (`:305-313`), CertID
  must match name hash, key hash and serial (`:196-215`), HTTP status 200 and Content-Type enforced (`:343-350`).
- CRL: issuer name and key identifier must match the certificate's issuer, issuer must have cRLSign, inner/outer algorithm
  must agree, signature verified (`Crl.cpp:124-154,179-181`). Only direct CRLs are supported (`:147-148`).
- Timestamp token: message imprint, nonce, TSA certificate EKU exactly id-kp-timeStamping and chain to a trusted anchor
  (`TimeStampToken.cpp:399-448`); a granted response without a token is an error (`:484`); the client verifies before
  returning (`TimeStampClient.cpp:118-124`).
- Fuzz: 0.86M OCSP/CRL and 1.57M token executions with no crash or hang (the harness checks parse/verify do not violate its
  basic invariants; it is not a proof that no forged response verifies).
Not verified: that every caller treats `RevocationOutcome::acceptable()` as the only gate (callers in `src/app` are being
written); a delegated responder must now carry `id-pkix-ocsp-nocheck` (F6) so it is not itself revocation-checked, a CA-signed responder without it is refused;
`genTime` skew policy against the local clock was not reviewed; delta CRLs and indirect CRLs are documented limitations
(`PdfSignatureVerifier.cpp:756`).
Residual risk: soft-fail mode (hardFail off) is a user-visible weaker setting by design.

### 7. Signature wrapping and ByteRange coverage
Checked (`PdfSignatureVerifier.cpp:1019-1068`): ByteRange must be four integers, start at 0, be non-negative, in bounds, not
overlap, not pass EOF; `/Contents` must be a hex string whose source span is exactly the gap between the two ranges
(`:1052-1055`), so a second signature value cannot hide in the gap; `/Contents` capped at 8 MiB; duplicate dictionary keys
rejected (`:993-996`); the signature dictionary must be an in-use plain object of the revision that the ByteRange ends in
(`signedEntry->a != entry->a` check inside `analyseModifications`, plus `k` revision matching, `:1078-1089`);
`coversWholeFile` requires only whitespace after the range (`:1060-1067`); later revisions go through the
modification analyzer, anything not provably allowed becomes `Unknown` (not `None`), with "data follows the last revision"
for trailing bytes (`:1089-1109`). Signer also self-verifies its output (`PdfSigner.cpp:1017-1027`).
Fuzz: 171k pdfsig executions in the 120 s run after the invariant fix (the first run aborted early on the harness false
positive), no product crash.
Not verified: the modification analyzer's allowed-change rules (DocMDP levels, field locks) were not audited line by
line; not compared to other readers' view of the same file (parser-differential attacks).
Residual risk: parser differentials between Rivet's reader and PDFium/Acrobat on malformed files remain the classic
signature-wrapping risk; Rivet fails to `Unknown` rather than guessing, which is the right bias.

### 8. DSS / VRI trust
Checked: `evaluateLtv` is offline by construction (no network object is passed, `PdfSignatureVerifier.cpp:752`,
`policy.allowNetwork=false` `:831`). Trust anchors come only from `VerifyOptions` (`:826-827,:840`); certificates, OCSP and
CRLs embedded in the document only populate the candidate pool and evidence store (`:820-833`), so embedded material can
supply evidence but can never add a trust anchor. Without caller anchors the LTV evaluation stops with a reason
(`:799-801`). VRI keys are the hash of the signature (`vriKey`), a missing VRI is reported and optionally fatal
(`:788-791`). B-LT requires both signer and TSA chains complete (`:861-865`), B-LTA additionally needs a valid document
timestamp after the revision that introduced the evidence (`:868-880`). DSS stream counts and sizes are capped
(`PdfDss.hpp:21-23`).
Not verified: the remaining B-LTA code after line 880, and `LtvChain.cpp` was only skimmed (`kMaxMissing`).
Residual risk: policies, name constraints and delta CRLs are not evaluated (stated in the produced report,
`:757`).

### 9. PKCS#11 and CryptoPro dlopen loading
Checked: PKCS#11 `dlopen(RTLD_NOW|RTLD_LOCAL)` / `LoadLibraryW` on a path the user or admin configures
(`Pkcs11Module.cpp:30-45`); the path must exist, be absolute (F1) and the library must export `C_GetFunctionList`; module
instances are cached by normalised path (`:108-125`), calls are serialised and the handle is finalised before unload.
Token-supplied strings are stripped of control characters before UI/log use (`trimField`, `:96-105`) and bounded
(`kMaxTokenMessage`, certificate sizes `Pkcs11IdentityStore.cpp:27-35`). CryptoPro: fixed default directories under
`/opt/cprocsp/lib*` or an explicit absolute directory (F2) (`CryptoProApi.cpp:34-66`), handles intentionally never
`dlclose`d.
Fixed (F5): both loaders call `checkModuleFile` before `dlopen` (ownership, permissions, symlink resolution, parent
directory). NOT checked: signature/code-signing verification of the module. A module is arbitrary native code that runs with
the user's rights, so the trust decision is the user's.
Residual risk: a check-then-`dlopen` window (TOCTOU) remains; the Windows branch is best effort (absolute drive path, regular
non-symlink file, no ACL inspection) and was not built here; the explicit override disables the permission checks. The
provider-level override option is tested only through the helper. Tested against a mock module only, never a vendor module.

### 10. Remote QES privacy modes
Checked: default `RemotePrivacyMode::HashOnly` (`CryptoProDss.hpp:40`); `WholeDocument` requires the caller to pass the exact
consent text (`CryptoProDss.cpp:160-163`) and the document bytes (`:352-354`); in `HashOnly` the document is never
serialised even if the caller supplied it (`:393-396`); the cancel notice never carries the document (`:366-368`);
https only, no query, no redirects (`:156-159,:213`), JSON size and depth capped (`JsonLite.hpp:20-21`,
`JsonLite.cpp:164-165,233`), retries only for 5xx with an idempotency key (`:228-234,:388`).
Not verified: the real DSS wire protocol (mock only; ADR-0047 says real E2E is blocked); that the UI shows the consent text
verbatim and does not pre-fill it; the service-side privacy behaviour.
Residual risk: the bearer token travels in the JSON body because the net layer forbids an `Authorization` header; this is
documented as an assumption (`CryptoProDss.cpp:34-36`).

### 11. Encryption writer correctness and limits
Checked: refuses already-encrypted input (`PdfEncrypt.cpp:455`), documents that contain signatures (`:563-564`, encrypt
before sign), object streams and xref streams are unsupported (`:571`), refuses a call with no password and no restriction
(`:694`), passwords capped at 4096 bytes (`:697`), file size capped at 2 GiB, nesting depth 64, 16M tokens per object
(`:26-28`). Randomness (salts, IVs, file key, Perms filler) comes from the crypto provider (`StandardHandler.cpp:42-45,248,258,318,367,414`),
fresh 16-byte IV per string/stream (`:414-418`), R6 hash (`:181`), UTF-8 passwords validated and truncated at 127 bytes for
R6 (`:157-163`), AES-128 passwords limited to Latin-1 (`:175`). 96k libFuzzer executions of the input scanner (ASan/UBSan)
with no crash; the encryption round trip and password checks are covered by the existing tests in `tests/pdf/encrypt`.
Not verified: interoperability with Acrobat / other viewers for every permission combination (only the repo's own tests
and PDFium decrypt were used), RC4/R4 password-entry derivation against the spec beyond the existing known-answer tests.
Fixed (F3): secrets are wiped in this module; see section 3 for the remaining unavoidable copies.

### 12. Parser DoS limits
Checked and found in the code: DER depth 24, 1M children, 1 GiB input (`Der.hpp:22-24`); certificate 16 MiB
(`X509Parse.cpp:38`); CMS signers 1024 and certificates 4096 (`Cms.cpp:19-20`); chain length 10 and search steps 4096
(`Certificate.cpp:30`, `PathBuilder.cpp:31-33`); PDF object depth 48, array 4M, dict 65k entries, string 64 MiB
(`PdfObject.hpp:30-33`); xref sections 8192, entries 16M, object stream 256 MiB, field nodes 200k (`PdfFile.hpp:27-30`);
signatures 4096 (`PdfSignatureVerifier.cpp:24`); signature contents 8 MiB; DSS 4096 items, 8 MiB each, 64 MiB total
(`PdfDss.hpp:21-23`); JSON 4 MiB, depth 8; HTTP 4 MiB body, 64 KiB headers. Fuzz runs found no unbounded time or memory in
any parser within the 120 s runs (libFuzzer's default 1200 s timeout and 2 GiB RSS limit were never hit).
Not verified: worst-case CPU of `PathBuilder` combined with 4096 signatures (each with its own chain build) was not
measured; `kMaxInputSize` of 1 GiB for DER is a large ceiling that relies on callers having already bounded the input
(the net layer and the signature `/Contents` cap do); inflate limits at each `inflateZlib` caller were not individually
audited.
Fixed (F7): configurable caps (`VerifyOptions::limits`, default 256 signatures) fail with a clear `LimitExceeded` status. Residual risk: the worst-case CPU under the default caps was not measured.

## Not verified (summary)
- Real TLS behaviour, real TSA/OCSP/CRL servers, real DSS service, real PKCS#11 vendor modules, real CryptoPro CSP.
- Windows code paths (`AtomicFileWriter` Windows branch, CNG store, `LoadLibraryW`) were read but not built or run here.
- UI/controller layers (`src/app`, `src/ui`): PIN prompt, consent text display, logging.
- Modification-analyzer allowed-change semantics, B-LTA tail logic, delta/indirect CRLs.
- Long-run fuzzing (hours), structure-aware coverage of chain building, differential tests against other PDF readers.

## Suggested follow-ups (larger than a small fix)
1. (done, F5) Module file trust checks. Still open: document the expected install path; code-signing verification.
2. (done, F3) Encrypt secret wiping. Still open: migrate callers to `EncryptSecrets`; clear the CryptoPro PIN right after `setProvParam`.
3. (done, F6) Delegated OCSP responders require `ocsp-nocheck`.
4. (done, F7) Verifier budgets. Still open: measure worst-case CPU under the defaults.
5. Longer CI fuzzing (OSS-Fuzz style) with the committed seed generators.
