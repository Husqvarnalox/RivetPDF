# ADR-0039: GOST certificates and Russian qualified signatures (evidence model)

Status: Accepted

Date: 2026-10-17

## Context

Russian users sign with GOST R 34.10-2012 keys, usually held in CryptoPro CSP
containers or on PKCS#11 tokens. Rivet must read such certificates, display
them meaningfully, and tell the user what technical evidence a signature
carries. It must never decide, or appear to decide, whether a signature is
legally a qualified electronic signature (KEP/КЭП).

## Decision

### 1. Backend-free certificate reading

`parseX509` (`src/crypto/X509Parse.cpp`, built on `Der.hpp`) reads X.509 without
any crypto backend. OpenSSL 3 does not understand GOST keys, so every
certificate, GOST or not, goes through this one reader; the OpenSSL provider is
used only as an oracle in tests (equivalence on RSA and EC certificates).

| Purpose | OID |
|---|---|
| Public key GOST R 34.10-2012 256 | 1.2.643.7.1.1.1.1 |
| Public key GOST R 34.10-2012 512 | 1.2.643.7.1.1.1.2 |
| Signature GOST R 34.10-2012 / 34.11-2012 256 | 1.2.643.7.1.1.3.2 |
| Signature GOST R 34.10-2012 / 34.11-2012 512 | 1.2.643.7.1.1.3.3 |
| Digest Streebog 256 / 512 | 1.2.643.7.1.1.2.2 / 1.2.643.7.1.1.2.3 |
| Subject INN / SNILS / OGRN / OGRNIP | 1.2.643.3.131.1.1 / 1.2.643.100.3 / 1.2.643.100.1 / 1.2.643.100.5 |
| INN of a legal entity (innLE) | 1.2.643.100.4 |

Subject text follows the RFC 2253 convention (reversed RDN order, escaping);
unknown attribute types render as dotted OID plus `#hex`. The reader is bounded
(16 MiB input, DER depth and size limits), never throws, and maps every
malformed input to `CorruptData`.

### 2. Classification model

`classify()` (`Qualification.hpp`) collects evidence and sorts a signature into
a descriptive class. Classes are cumulative checks, not legal states.

| Class | Required evidence |
|---|---|
| Unknown | nothing known about key, algorithm or certificate |
| SimpleCrypto | non-GOST algorithm, valid signature, no further evidence |
| GostDigitalSignature | GOST R 34.10-2012 algorithm; nothing more |
| AdvancedUnqualified | identity attributes (CN plus INN, SNILS, OGRN or OGRNIP), signature key usage, non-software key custody, certificate valid at the signing time |
| QualifiedEvidence | everything above, GOST key, immediate issuer linked by AKI to SKI and listed in the accredited CA list, a certificate policy OID the list marks as qualified, and a provider declared as a certified CSP that the list names |

`QualifiedEvidence` means only "every technical indicator Rivet can check is
present". The result carries the individual findings (`Evidence`) and short
reasons so the UI can show what was and was not found. A GOST algorithm OID
alone never raises a class above `GostDigitalSignature`.

### 3. No accredited CA list ships

Accreditation is a legal fact that changes over time and is published by the
competent authority. A list baked into the binary would be stale, unauditable
by the user and would carry an implicit legal claim. Rivet therefore ships no
list: without one, `QualifiedEvidence` is unreachable. The application loads a
list from a configurable path; it can be replaced without rebuilding.

List file format (UTF-8, LF or CRLF, at most 1 MiB and 100000 lines, strict:
an unknown directive or malformed line is an error naming the line):

```
rivet-accredited-ca-list 1
# comment
updated 2026-01-31
source <free text>
ca sha256=<64 hex> [ski=<hex>] name=<rest of line>
policy <dotted OID>
csp <provider-id>
```

A missing file is `Io`, malformed content `CorruptData`, size bounds
`LimitExceeded`. The list is data the user (or their organization) chooses to
trust; Rivet does not fetch it from the network.

### 4. Display model

`CertificateDisplay` (`makeCertificateDisplay`) is a pure data model with no UI
dependency: person name (surname and given name), organization, issuer name,
validity, key algorithm label (for example "GOST R 34.10-2012 256"), identity
attributes (INN, SNILS, OGRN, OGRNIP) and fingerprints. The UI renders it; it
makes no judgement. The classification is shown beside it as evidence with
`kLegalDisclaimer`.

### 5. Legal status is outside the software

Whether a signature is a qualified electronic signature, and whether it has
legal force in a given context, is decided by law, the accreditation of the
certification authority, the certified status of the signing tool and the
circumstances of use. Rivet does not and cannot establish this. User-facing
text uses "technical evidence" wording and never "legally valid".

## Consequences

- GOST certificates display correctly without a GOST-capable backend; signing
  and verification of GOST signatures still need CryptoPro CSP or a token.
- Users and integrators who want `QualifiedEvidence` must supply and maintain
  their own list; the default experience tops out at `AdvancedUnqualified`
  for GOST identities. This is intentional.
- The class names are stable machine keys (`simple_crypto`,
  `gost_digital_signature`, `advanced_unqualified`, `qualified_evidence`) so
  stored results stay comparable across versions.
- Wording in the UI, documentation and marketing must stay consistent with
  section 5; changing it needs a new ADR.
- Certification of the installed CSP build and licence validity are not
  verified by Rivet (`CertifiedCspDeclared` is a declaration by provider family).
