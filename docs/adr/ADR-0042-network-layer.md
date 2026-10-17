# ADR-0042: Constrained network layer (`rivet_net`)

Status: accepted.

## Context

Signature validation and signing want three kinds of outbound requests: RFC 3161 timestamps, OCSP and CRL fetches. Rivet is local-first: it must make no network call unless the user has allowed it, and the URLs involved (certificate extensions, redirects) come from untrusted documents. That makes the fetcher a classic SSRF surface.

## Decision

Everything network-related lives in `src/net` (`rivet::net`). The application reaches the network only through `NetworkClient::fetch()` / `runNetworkAsync()`; there is no other entry point and no telemetry.

- **URL parser** (`parseUrl`): http/https only; rejects userinfo, backslashes, whitespace/control/non-ASCII bytes, `%` or odd characters in hosts, non-punycode IDN, bad ports, URLs over 2048 bytes. Fragments are stripped, hosts lowercased, one trailing dot removed. Numeric hosts (`127.1`, `0x7f.0.0.1`, `0177.0.0.1`, `2130706433`, `[::1]`) are canonicalised to an `IpAddress` so policy classifies the real address, never a name handed to a resolver.
- **`NetworkPolicy`**: https only by default, private networks off, optional allow-list (`host` or `*.suffix`; the wildcard needs a non-empty label before the suffix, so `*.example.com` does not match `example.com`, `evilexample.com` or `example.com.evil.com`), 3 redirects, 4 MiB, 10 s connect, 30 s total, fixed `User-Agent: Rivet`. No cookies, no Referer, no caller-supplied credentials (Cookie/Authorization/Host/... headers are rejected), no option to disable certificate verification.
- **SSRF guard** (`isForbiddenAddress`): IPv4 0/8, 10/8, 100.64/10, 127/8, 169.254/16, 172.16/12, 192.0.0/24, 192.168/16, 198.18/15, documentation nets, multicast, 240/4 and broadcast; IPv6 `::`, `::1`, fe80::/10, fec0::/10, fc00::/7, ff00::/8, 2001:db8::/32, Teredo, and the IPv4-embedding forms (`::ffff:a.b.c.d`, `::a.b.c.d`, 64:ff9b::/96, 2002::/16) whose embedded IPv4 address is classified. `localhost`, `*.localhost`, `*.local`, `*.internal` are refused as names.
- **`NetworkClient`** owns the redirect loop. Every hop re-runs online-validation gate, scheme, allow-list, host/IP policy; https to http redirects are refused; the total timeout is one budget across hops; the response-size cap and cancellation are enforced by both the transport and the client (defence in depth). The transport interface is strictly one hop.
- **`INetworkTransport`**: `CurlTransport` (libcurl, `RIVET_ENABLE_NETWORK=ON`) and `MockTransport` (in the library, for tests and later TSA/OCSP/CRL tests: scripted replies, redirects, latency/hang, oversize, scripted connect address). With the option OFF `makeDefaultTransport()` returns a transport failing with `NetFailure::BackendUnavailable`.
- **Online validation** (`OnlineValidationMode` Off / Ask / On, default Ask) is enforced by `OnlineValidationGate` inside `NetworkClient`, so no caller can skip it. Off makes zero transport calls with a distinct error. Ask uses an injected async `IConsentPrompt` (default denies), once per host and session, redirect hosts included; the prompt is never called under a lock.
- **Errors**: `NetFailure` (InvalidRequest, PolicyViolation, ForbiddenAddress, OnlineValidationOff, ConsentDenied, RedirectLimit, RedirectDowngrade, BadRedirect, ResponseTooLarge, Timeout, Cancelled, BackendUnavailable, Transport) carried in `core::Error` as subsystem `net.<name>`; `failureOf()` recovers it. No new `core::ErrorCode` values.

## Threat model

| Threat | Mitigation |
| --- | --- |
| SSRF to loopback, LAN, cloud metadata | Policy check on URL literals in every notation, plus the connect-time check below |
| DNS rebinding / name resolving to a private address | libcurl `OPENSOCKETFUNCTION` classifies the peer address actually about to be connected (every candidate address), before `connect()`; unknown families fail closed |
| Redirect to an internal target, scheme switch, downgrade | Redirects are not followed by libcurl; each hop is re-parsed and re-validated; protocols restricted to http/https; https to http refused; limit 3 |
| Response bomb / slow-loris | Write callback aborts at the cap (also without Content-Length), `MAXFILESIZE`, header size cap, connect and total timeouts, cancellation polled in the progress callback |
| Header / request smuggling | Strict URL charset, header name/value validation, forbidden header list |
| Credential or identity leakage | No cookies, netrc, auth, Referer; constant UA; no proxy from the environment unless `useSystemProxy` |
| Hidden network use | Gate (Off/Ask/On) in the client; library default transport only exists behind the option |

Limits: when `useSystemProxy` is enabled the peer address seen is the proxy's, so the guard applies to the proxy only. The client cannot resolve names itself (no DNS in the portable layer); hostname checks rely on the connect-time guard.

## Why libcurl

Mature TLS 1.2+ verification with the platform trust store, HTTP/1.1 edge cases and timeouts, and the one hook we need (`OPENSOCKETFUNCTION`) for a rebinding-proof peer check. It is already a system library on macOS and common on Linux. Hand-rolling HTTP/TLS or using Boost.Asio / platform HTTP stacks would add a larger dependency or per-platform code with no equivalent connect hook. It is optional, behind `RIVET_ENABLE_NETWORK`; the app builds and runs without it.

## Platform status

- macOS (system libcurl) and Linux/GCC: built with `RIVET_ENABLE_NETWORK=ON` in CI; loopback-server tests run with no internet.
- macOS universal job: network OFF for now (a Homebrew single-arch libcurl could be picked up by `find_package`).
- Windows/MSVC: network OFF in CI (no libcurl provisioning yet); portable code compiles, loopback tests are skipped. A WinHTTP transport behind the same `INetworkTransport` interface is the expected follow-up.
