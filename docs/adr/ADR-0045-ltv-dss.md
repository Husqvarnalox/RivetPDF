# ADR-0045: PAdES long-term validation (B-LT, B-LTA) and /DSS

Status: accepted

## Context

B-T proves when a signature existed. To validate it years later, without the
CA's responders still being reachable, the document must carry the
certificates and revocation evidence (PAdES B-LT) and, to survive algorithm and
certificate expiry, an archival document time-stamp over them (B-LTA).

## Decisions

### Layout

- Validation data lives in the catalog `/DSS` dictionary (`/Certs`, `/OCSPs`,
  `/CRLs`, each an array of streams) and per signature in `/DSS /VRI`, keyed by
  the uppercase hex SHA-1 of that signature's `/Contents` DER. A VRI entry lists
  only the streams relevant to that signature (and its time-stamp's TSA chain).
- Always an incremental revision (`RevisionWriter`): the original bytes are an
  exact prefix of the output and earlier signatures stay valid. Existing
  `/DSS` content is merged; streams are deduplicated by SHA-256 and reused.
- A revision that only adds `/DSS` is classified by the verifier as a
  validation-data-only change: modification status stays None for earlier
  signatures.
- Encrypted PDFs and files over 2 GiB are refused.

### Evidence rules

- Historical validation: a signer chain is judged at the signature
  time-stamp's genTime, the TSA chain at that same genTime. OCSP/CRL evidence is
  acceptable if it was produced at or after the validation time or covers it,
  and is itself validly signed. Evidence is consulted from the store (the
  `/DSS`) before the network.
- The path is built by `crypto::buildValidationPath`: DFS with backtracking over
  the embedded certificates, `/DSS` certificates and (optionally) AIA
  caIssuers, with pathLen, basicConstraints, keyUsage keyCertSign and validity
  at the given time checked; loops and over-long chains fail.
- Network only through `rivet::net` (Off / Ask / On gate, SSRF policy,
  size limits). Verification never touches the network. No hard-coded URLs.
- Everything in a file is untrusted; weak algorithms are verify-only.

### Fail closed

Without `LtvOptions::allowPartial`, missing mandatory evidence returns
`EvidenceUnavailable` listing the certificates, and no output is produced
(file variants leave the destination untouched). With `allowPartial`, what
could be collected is written and the result reports the level actually
reached plus the reason (`levelDowngradeReason`). Every produced file is
self-verified; the reported level is the verifier's, never the requested one.
B-B -> B-T upgrades are refused (a signature time-stamp must be requested
while signing).

### B-LTA and renewal

B-LTA is B-LT plus a valid DocTimeStamp whose byte range covers the revision
that introduced the evidence. `upgradePdf` appends it; `addArchiveTimestamp`
renews: it adds the evidence for the newest archive time-stamp's TSA chain to
the `/DSS` and appends a fresh DocTimeStamp over it.

### CLI

`rivet-pdf-sign --level B-B|B-T|B-LT|B-LTA`, `--anchor`, `--ocsp/--crl on|off`,
`--fetch-issuers`, `--allow-partial`, and an `upgrade` subcommand. The gate is
On in the CLI: invoking it with these options is the consent to contact the
hosts named in the certificates. `rivet-pdf-signature-inspect` shows level,
`/DSS`, VRI and evidence completeness.

## Limitations

- Delta CRLs are supported (ADR-0044); the evidence embeds the base and the delta.
- No ETSI EN 319 102-1 ("ESIC") conformance verification: levels are Rivet's
  honest structural/evidence measure. What is checked is the `/Extensions /ESIC`
  declaration: the verifier notes in `LtvInfo::limitations` a document that has a
  `/DSS` but no ESIC declaration (never fatal on its own). The full EN 319 102-1
  validation procedure is not implemented.
- nameConstraints are enforced during path building (`crypto/NameConstraints`, ADR-0044);
  certificate policies are not evaluated.
- Renewal covers only the latest archive time-stamp's chain, not older ones.
- OCSP responders are accepted when signed by the issuer or a delegated
  responder certificate per the ADR-0044 rules.
